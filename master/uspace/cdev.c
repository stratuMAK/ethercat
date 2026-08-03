/*****************************************************************************
 *
 *  Copyright (C) 2006-2024  Florian Pose, Ingenieurgemeinschaft IgH
 *
 *  This file is part of the IgH EtherCAT Master.
 *
 *  The IgH EtherCAT Master is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License version 2, as
 *  published by the Free Software Foundation.
 *
 *  The IgH EtherCAT Master is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General
 *  Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with the IgH EtherCAT Master; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 ****************************************************************************/

/**
   \file
   Userspace EtherCAT master IPC character device server.

   Implements a Unix domain socket server that allows the \a ethercat
   command-line tool to communicate with a userspace master running inside
   an application process.  The protocol matches the \c ec_ipc_request_t /
   \c ec_ipc_response_t wire format defined in \c master/ec_ioctl_data.h.
*/

/****************************************************************************/

#include "pal.h"

#include "../master.h"
#include "../slave.h"
#include "../slave_config.h"
#include "../domain.h"
#include "../sdo.h"
#include "../sdo_entry.h"
#include "../pdo.h"
#include "../pdo_list.h"
#include "../pdo_entry.h"
#include "../master_globals.h"
#include "../ec_ioctl_data.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <poll.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>

#ifndef UNIX_PATH_MAX
#define UNIX_PATH_MAX (sizeof(((struct sockaddr_un *)0)->sun_path))
#endif

/** Maximum payload size we are willing to receive (protect against OOM). */
#define EC_IPC_MAX_PAYLOAD 65536U

/** Maximum response payload the daemon allocates on behalf of a client.
 * Sized for the largest legitimate consumers (FoE file reads, big
 * domain images); primarily a cap so a crafted size field cannot make
 * the (mlocked) master process malloc up to 4 GiB. All length checks
 * against client-supplied sizes are computed in uint64_t so oversized
 * 32-bit fields cannot wrap them. */
#define EC_IPC_MAX_RESPONSE (16U * 1024U * 1024U)

/** Maximum number of concurrent tool connections. */
#define EC_IPC_MAX_CLIENTS 16

/****************************************************************************/

/** Global singleton IPC server state. */
typedef struct {
    _Atomic int      sock_fd;        /**< Listening socket fd (-1 if inactive). Handshake between listener thread and ec_ipc_server_stop(). */
    char             sock_path[UNIX_PATH_MAX]; /**< Unix socket filesystem path. */
    ec_thread_t     *thread;         /**< Listener thread handle. */
    atomic_int       shutdown;       /**< Non-zero to request shutdown. */
} ec_cdev_t;

static ec_cdev_t g_cdev = { .sock_fd = -1 };

/****************************************************************************/

/** Global master registry — protected by registry_rwlock. */
static ec_master_t *master_registry[EC_MAX_MASTERS];
static unsigned int registry_master_count;
static pthread_rwlock_t registry_rwlock = PTHREAD_RWLOCK_INITIALIZER;

/****************************************************************************/

void ec_master_registry_add(ec_master_t *master)
{
    if (!master || master->index >= EC_MAX_MASTERS)
        return;
    pthread_rwlock_wrlock(&registry_rwlock);
    if (master_registry[master->index] == NULL)
        registry_master_count++;
    master_registry[master->index] = master;
    pthread_rwlock_unlock(&registry_rwlock);
}

void ec_master_registry_remove(ec_master_t *master)
{
    if (!master || master->index >= EC_MAX_MASTERS)
        return;
    pthread_rwlock_wrlock(&registry_rwlock);
    if (master_registry[master->index] == master) {
        master_registry[master->index] = NULL;
        if (registry_master_count > 0)
            registry_master_count--;
    }
    pthread_rwlock_unlock(&registry_rwlock);
}

ec_master_t *ec_master_registry_find(unsigned int index)
{
    ec_master_t *master;
    if (index >= EC_MAX_MASTERS)
        return NULL;
    pthread_rwlock_rdlock(&registry_rwlock);
    master = master_registry[index];
    pthread_rwlock_unlock(&registry_rwlock);
    return master;
}

/** Acquire the registry read lock and return the master pointer.
 * The read lock is held on return and must be released by
 * ec_master_registry_put().  Returns NULL (with lock released) if not found. */
static ec_master_t *ec_master_registry_get(unsigned int index)
{
    ec_master_t *master;
    if (index >= EC_MAX_MASTERS)
        return NULL;
    pthread_rwlock_rdlock(&registry_rwlock);
    master = master_registry[index];
    if (!master)
        pthread_rwlock_unlock(&registry_rwlock);
    return master;
}

/** Release the registry read lock acquired by ec_master_registry_get(). */
static void ec_master_registry_put(ec_master_t *master)
{
    if (master)
        pthread_rwlock_unlock(&registry_rwlock);
}

unsigned int ec_master_registry_count(void)
{
    unsigned int count;
    pthread_rwlock_rdlock(&registry_rwlock);
    count = registry_master_count;
    pthread_rwlock_unlock(&registry_rwlock);
    return count;
}

ec_master_t *ec_master_registry_pop_first(void)
{
    ec_master_t *master = NULL;
    unsigned int i;

    pthread_rwlock_wrlock(&registry_rwlock);
    for (i = 0; i < EC_MAX_MASTERS; i++) {
        if (master_registry[i]) {
            master = master_registry[i];
            master_registry[i] = NULL;
            registry_master_count--;
            break;
        }
    }
    pthread_rwlock_unlock(&registry_rwlock);

    return master;
}

/****************************************************************************/
/* Helper: send exactly \a len bytes; retry on EINTR.                         */
/****************************************************************************/

static int send_all(int fd, const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    size_t remaining = len;

    while (remaining > 0) {
        ssize_t n = send(fd, p, remaining, MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0)
                return -errno;
            return -ECONNRESET;
        }
        p += n;
        remaining -= (size_t)n;
    }
    return 0;
}

/****************************************************************************/
/* Helper: receive exactly \a len bytes; return 0 on success, -errno on error,
 * -ECONNRESET on clean EOF.                                                   */
/****************************************************************************/

static int recv_all(int fd, void *buf, size_t len)
{
    char *p = (char *)buf;
    size_t remaining = len;

    while (remaining > 0) {
        ssize_t n = recv(fd, p, remaining, MSG_WAITALL);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -errno;
        }
        if (n == 0)
            return -ECONNRESET;
        p += n;
        remaining -= (size_t)n;
    }
    return 0;
}

/****************************************************************************/
/* Helper: send a framed IPC response.                                        */
/****************************************************************************/

static int send_response(int fd, int32_t ret_val,
        const void *data, uint32_t data_size)
{
    ec_ipc_response_t resp;
    int ret;

    resp.ret       = ret_val;
    resp.data_size = data_size;

    ret = send_all(fd, &resp, sizeof(resp));
    if (ret < 0)
        return ret;

    if (data && data_size > 0)
        ret = send_all(fd, data, data_size);

    return ret;
}

/** Send a response with both struct data and a trailing data blob. */
static int send_response_with_trailing(int fd, int32_t ret_val,
        const void *data, uint32_t data_size,
        const void *trailing, uint32_t trailing_size)
{
    ec_ipc_response_t resp;
    int ret;

    resp.ret       = ret_val;
    resp.data_size = data_size + trailing_size;

    ret = send_all(fd, &resp, sizeof(resp));
    if (ret < 0)
        return ret;

    if (data && data_size > 0) {
        ret = send_all(fd, data, data_size);
        if (ret < 0)
            return ret;
    }

    if (trailing && trailing_size > 0)
        ret = send_all(fd, trailing, trailing_size);

    return ret;
}

/****************************************************************************/
/* Command dispatch                                                            */
/****************************************************************************/

#include "../../include/ecrt_tool.h"

/** EC_CMD_MODULE — return version magic and master count. */
static int dispatch_module(int fd, ec_master_t *master,
        const uint8_t *req, uint32_t req_size)
{
    ec_tool_module_t io;
    int ret;
    (void)master;
    (void)req;
    (void)req_size;

    ret = ecrt_tool_get_module(&io);
    if (ret)
        return send_response(fd, ret, NULL, 0);
    return send_response(fd, 0, &io, sizeof(io));
}

/** EC_CMD_MASTER — return master status. */
static int dispatch_master(int fd, ec_master_t *master,
        const uint8_t *req, uint32_t req_size)
{
    ec_tool_master_t io;
    int ret;
    (void)req;
    (void)req_size;

    ret = ecrt_tool_get_master(master, &io);
    if (ret)
        return send_response(fd, ret, NULL, 0);
    return send_response(fd, 0, &io, sizeof(io));
}

/** EC_CMD_SLAVE — return slave information (input: slave position). */
static int dispatch_slave(int fd, ec_master_t *master,
        const uint8_t *req, uint32_t req_size)
{
    ec_tool_slave_t io;
    int ret;

    if (req_size != sizeof(io))
        return send_response(fd, -EINVAL, NULL, 0);
    memcpy(&io, req, sizeof(io));

    ret = ecrt_tool_get_slave(master, &io);
    if (ret)
        return send_response(fd, ret, NULL, 0);
    return send_response(fd, 0, &io, sizeof(io));
}

/** EC_CMD_SLAVE_SYNC — slave sync manager information. */
static int dispatch_slave_sync(int fd, ec_master_t *master,
        const uint8_t *req, uint32_t req_size)
{
    ec_tool_slave_sync_t io;
    int ret;

    if (req_size != sizeof(io))
        return send_response(fd, -EINVAL, NULL, 0);
    memcpy(&io, req, sizeof(io));

    ret = ecrt_tool_get_slave_sync(master, &io);
    if (ret)
        return send_response(fd, ret, NULL, 0);
    return send_response(fd, 0, &io, sizeof(io));
}

/** EC_CMD_SLAVE_SYNC_PDO — slave sync manager PDO information. */
static int dispatch_slave_sync_pdo(int fd, ec_master_t *master,
        const uint8_t *req, uint32_t req_size)
{
    ec_tool_slave_sync_pdo_t io;
    int ret;

    if (req_size != sizeof(io))
        return send_response(fd, -EINVAL, NULL, 0);
    memcpy(&io, req, sizeof(io));

    ret = ecrt_tool_get_slave_sync_pdo(master, &io);
    if (ret)
        return send_response(fd, ret, NULL, 0);
    return send_response(fd, 0, &io, sizeof(io));
}

/** EC_CMD_SLAVE_SYNC_PDO_ENTRY — slave sync manager PDO entry information. */
static int dispatch_slave_sync_pdo_entry(int fd, ec_master_t *master,
        const uint8_t *req, uint32_t req_size)
{
    ec_tool_slave_sync_pdo_entry_t io;
    int ret;

    if (req_size != sizeof(io))
        return send_response(fd, -EINVAL, NULL, 0);
    memcpy(&io, req, sizeof(io));

    ret = ecrt_tool_get_slave_sync_pdo_entry(master, &io);
    if (ret)
        return send_response(fd, ret, NULL, 0);
    return send_response(fd, 0, &io, sizeof(io));
}

/** EC_CMD_DOMAIN — domain information. */
static int dispatch_domain(int fd, ec_master_t *master,
        const uint8_t *req, uint32_t req_size)
{
    ec_tool_domain_t io;
    int ret;

    if (req_size != sizeof(io))
        return send_response(fd, -EINVAL, NULL, 0);
    memcpy(&io, req, sizeof(io));

    ret = ecrt_tool_get_domain(master, &io);
    if (ret)
        return send_response(fd, ret, NULL, 0);
    return send_response(fd, 0, &io, sizeof(io));
}

/** EC_CMD_DOMAIN_FMMU — domain FMMU information. */
static int dispatch_domain_fmmu(int fd, ec_master_t *master,
        const uint8_t *req, uint32_t req_size)
{
    ec_tool_domain_fmmu_t io;
    int ret;

    if (req_size != sizeof(io))
        return send_response(fd, -EINVAL, NULL, 0);
    memcpy(&io, req, sizeof(io));

    ret = ecrt_tool_get_domain_fmmu(master, &io);
    if (ret)
        return send_response(fd, ret, NULL, 0);
    return send_response(fd, 0, &io, sizeof(io));
}

/** EC_CMD_MASTER_DEBUG — set master debug level. */
static int dispatch_master_debug(int fd, ec_master_t *master,
        const uint8_t *req, uint32_t req_size)
{
    uint32_t level;
    int ret;

    if (req_size != sizeof(uint32_t))
        return send_response(fd, -EINVAL, NULL, 0);
    memcpy(&level, req, sizeof(uint32_t));

    ret = ecrt_tool_set_debug(master, (unsigned int)level);
    return send_response(fd, ret, NULL, 0);
}

/** EC_CMD_MASTER_RESCAN — trigger a bus rescan. */
static int dispatch_master_rescan(int fd, ec_master_t *master,
        const uint8_t *req, uint32_t req_size)
{
    int ret;
    (void)req;
    (void)req_size;

    EC_MASTER_DBG(master, 1, "Got rescan command via IPC.\n");
    ret = ecrt_tool_rescan(master);
    return send_response(fd, ret, NULL, 0);
}

/** EC_CMD_SLAVE_STATE — request a slave state change. */
static int dispatch_slave_state(int fd, ec_master_t *master,
        const uint8_t *req, uint32_t req_size)
{
    ec_tool_slave_state_t io;
    int ret;

    if (req_size != sizeof(io))
        return send_response(fd, -EINVAL, NULL, 0);
    memcpy(&io, req, sizeof(io));

    ret = ecrt_tool_set_slave_state(master, &io);
    if (ret)
        return send_response(fd, ret, NULL, 0);
    return send_response(fd, 0, NULL, 0);
}

/** EC_CMD_SLAVE_SDO — slave SDO information. */
static int dispatch_slave_sdo(int fd, ec_master_t *master,
        const uint8_t *req, uint32_t req_size)
{
    ec_tool_slave_sdo_t io;
    int ret;

    if (req_size != sizeof(io))
        return send_response(fd, -EINVAL, NULL, 0);
    memcpy(&io, req, sizeof(io));

    ret = ecrt_tool_get_slave_sdo(master, &io);
    if (ret)
        return send_response(fd, ret, NULL, 0);
    return send_response(fd, 0, &io, sizeof(io));
}

/** EC_CMD_SLAVE_SDO_ENTRY — slave SDO entry information. */
static int dispatch_slave_sdo_entry(int fd, ec_master_t *master,
        const uint8_t *req, uint32_t req_size)
{
    ec_tool_slave_sdo_entry_t io;
    int ret;

    if (req_size != sizeof(io))
        return send_response(fd, -EINVAL, NULL, 0);
    memcpy(&io, req, sizeof(io));

    ret = ecrt_tool_get_slave_sdo_entry(master, &io);
    if (ret)
        return send_response(fd, ret, NULL, 0);
    return send_response(fd, 0, &io, sizeof(io));
}

/** EC_CMD_CONFIG — slave configuration information. */
static int dispatch_config(int fd, ec_master_t *master,
        const uint8_t *req, uint32_t req_size)
{
    ec_tool_config_t io;
    int ret;

    if (req_size != sizeof(io))
        return send_response(fd, -EINVAL, NULL, 0);
    memcpy(&io, req, sizeof(io));

    ret = ecrt_tool_get_config(master, &io);
    if (ret)
        return send_response(fd, ret, NULL, 0);
    return send_response(fd, 0, &io, sizeof(io));
}

/** EC_CMD_CONFIG_PDO — slave configuration PDO information. */
static int dispatch_config_pdo(int fd, ec_master_t *master,
        const uint8_t *req, uint32_t req_size)
{
    ec_tool_config_pdo_t io;
    int ret;

    if (req_size != sizeof(io))
        return send_response(fd, -EINVAL, NULL, 0);
    memcpy(&io, req, sizeof(io));

    ret = ecrt_tool_get_config_pdo(master, &io);
    if (ret)
        return send_response(fd, ret, NULL, 0);
    return send_response(fd, 0, &io, sizeof(io));
}

/** EC_CMD_CONFIG_PDO_ENTRY — slave configuration PDO entry information. */
static int dispatch_config_pdo_entry(int fd, ec_master_t *master,
        const uint8_t *req, uint32_t req_size)
{
    ec_tool_config_pdo_entry_t io;
    int ret;

    if (req_size != sizeof(io))
        return send_response(fd, -EINVAL, NULL, 0);
    memcpy(&io, req, sizeof(io));

    ret = ecrt_tool_get_config_pdo_entry(master, &io);
    if (ret)
        return send_response(fd, ret, NULL, 0);
    return send_response(fd, 0, &io, sizeof(io));
}

/** EC_CMD_CONFIG_SDO — slave configuration SDO information. */
static int dispatch_config_sdo(int fd, ec_master_t *master,
        const uint8_t *req, uint32_t req_size)
{
    ec_tool_config_sdo_t io;
    int ret;

    if (req_size != sizeof(io))
        return send_response(fd, -EINVAL, NULL, 0);
    memcpy(&io, req, sizeof(io));

    ret = ecrt_tool_get_config_sdo(master, &io);
    if (ret)
        return send_response(fd, ret, NULL, 0);
    return send_response(fd, 0, &io, sizeof(io));
}

/** EC_CMD_CONFIG_IDN — slave configuration IDN information. */
static int dispatch_config_idn(int fd, ec_master_t *master,
        const uint8_t *req, uint32_t req_size)
{
    ec_tool_config_idn_t io;
    int ret;

    if (req_size != sizeof(io))
        return send_response(fd, -EINVAL, NULL, 0);
    memcpy(&io, req, sizeof(io));

    ret = ecrt_tool_get_config_idn(master, &io);
    if (ret)
        return send_response(fd, ret, NULL, 0);
    return send_response(fd, 0, &io, sizeof(io));
}

/** EC_CMD_CONFIG_FLAG — slave configuration feature flag information. */
static int dispatch_config_flag(int fd, ec_master_t *master,
        const uint8_t *req, uint32_t req_size)
{
    ec_tool_config_flag_t io;
    int ret;

    if (req_size != sizeof(io))
        return send_response(fd, -EINVAL, NULL, 0);
    memcpy(&io, req, sizeof(io));

    ret = ecrt_tool_get_config_flag(master, &io);
    if (ret)
        return send_response(fd, ret, NULL, 0);
    return send_response(fd, 0, &io, sizeof(io));
}

#ifdef EC_EOE

/** EC_CMD_EOE_HANDLER — EoE handler information. */
static int dispatch_eoe_handler(int fd, ec_master_t *master,
        const uint8_t *req, uint32_t req_size)
{
    ec_tool_eoe_handler_t io;
    int ret;

    if (req_size != sizeof(io))
        return send_response(fd, -EINVAL, NULL, 0);
    memcpy(&io, req, sizeof(io));

    ret = ecrt_tool_get_eoe_handler(master, &io);
    if (ret)
        return send_response(fd, ret, NULL, 0);
    return send_response(fd, 0, &io, sizeof(io));
}

/** EC_CMD_SLAVE_EOE_IP_PARAM — Set EoE IP parameters. */
static int dispatch_eoe_ip(int fd, ec_master_t *master,
        const uint8_t *req, uint32_t req_size)
{
    ec_tool_eoe_ip_t io;
    int ret;

    if (req_size != sizeof(io))
        return send_response(fd, -EINVAL, NULL, 0);
    memcpy(&io, req, sizeof(io));

    ret = ecrt_tool_set_eoe_ip(master, &io);
    if (ret)
        return send_response(fd, ret, &io, sizeof(io));
    return send_response(fd, 0, &io, sizeof(io));
}

#endif /* EC_EOE */

/****************************************************************************/
/* Trailing-data dispatch functions                                            */
/****************************************************************************/

/** EC_CMD_DOMAIN_DATA — return domain process data. */
static int dispatch_domain_data(int fd, ec_master_t *master,
        const uint8_t *req, uint32_t req_size)
{
    ec_tool_domain_data_t io;
    uint8_t *buf;
    int ret;

    if (req_size < sizeof(io))
        return send_response(fd, -EINVAL, NULL, 0);
    memcpy(&io, req, sizeof(io));

    if (!io.data_size || io.data_size > EC_IPC_MAX_RESPONSE)
        return send_response(fd, -EINVAL, NULL, 0);

    buf = malloc(io.data_size);
    if (!buf)
        return send_response(fd, -ENOMEM, NULL, 0);

    io.target = buf;
    ret = ecrt_tool_get_domain_data(master, &io);
    if (ret) {
        free(buf);
        return send_response(fd, ret, NULL, 0);
    }

    ret = send_response_with_trailing(fd, 0,
            &io, sizeof(io), buf, io.data_size);
    free(buf);
    return ret;
}

/** EC_CMD_SLAVE_SDO_UPLOAD — read an SDO entry from a slave. */
static int dispatch_slave_sdo_upload(int fd, ec_master_t *master,
        const uint8_t *req, uint32_t req_size)
{
    ec_tool_slave_sdo_upload_t io;
    uint8_t *target;
    int ret;

    if (req_size < sizeof(io))
        return send_response(fd, -EINVAL, NULL, 0);
    memcpy(&io, req, sizeof(io));

    if (!io.target_size || io.target_size > EC_IPC_MAX_RESPONSE)
        return send_response(fd, -EINVAL, NULL, 0);

    target = malloc(io.target_size);
    if (!target)
        return send_response(fd, -ENOMEM, NULL, 0);

    io.target = target;
    ret = ecrt_tool_sdo_upload(master, &io);

    if (!ret) {
        int send_ret = send_response_with_trailing(fd, 0,
                &io, sizeof(io), target, io.data_size);
        free(target);
        return send_ret;
    } else {
        free(target);
        return send_response(fd, ret, &io, sizeof(io));
    }
}

/** EC_CMD_SLAVE_SDO_DOWNLOAD — write an SDO entry to a slave. */
static int dispatch_slave_sdo_download(int fd, ec_master_t *master,
        const uint8_t *req, uint32_t req_size)
{
    ec_tool_slave_sdo_download_t io;
    int ret;

    if (req_size < sizeof(io))
        return send_response(fd, -EINVAL, NULL, 0);
    memcpy(&io, req, sizeof(io));

    if ((uint64_t)req_size < sizeof(io) + (uint64_t)io.data_size)
        return send_response(fd, -EINVAL, NULL, 0);

    io.data = (uint8_t *)(req + sizeof(io));
    ret = ecrt_tool_sdo_download(master, &io);
    return send_response(fd, ret, &io, sizeof(io));
}

/** EC_CMD_SLAVE_SII_READ — read SII words from a slave's EEPROM. */
static int dispatch_slave_sii_read(int fd, ec_master_t *master,
        const uint8_t *req, uint32_t req_size)
{
    ec_tool_slave_sii_t io;
    uint16_t *words;
    int ret;

    if (req_size < sizeof(io))
        return send_response(fd, -EINVAL, NULL, 0);
    memcpy(&io, req, sizeof(io));

    if (!io.nwords || io.nwords > EC_IPC_MAX_RESPONSE / 2)
        return send_response(fd, -EINVAL, NULL, 0);

    words = malloc((size_t)io.nwords * 2);
    if (!words)
        return send_response(fd, -ENOMEM, NULL, 0);

    io.words = words;
    ret = ecrt_tool_sii_read(master, &io);
    if (ret) {
        free(words);
        return send_response(fd, ret, NULL, 0);
    }

    ret = send_response_with_trailing(fd, 0,
            &io, sizeof(io), words, io.nwords * 2);
    free(words);
    return ret;
}

/** EC_CMD_SLAVE_SII_WRITE — write SII words to a slave's EEPROM. */
static int dispatch_slave_sii_write(int fd, ec_master_t *master,
        const uint8_t *req, uint32_t req_size)
{
    ec_tool_slave_sii_t io;
    int ret;

    if (req_size < sizeof(io))
        return send_response(fd, -EINVAL, NULL, 0);
    memcpy(&io, req, sizeof(io));

    if (!io.nwords)
        return send_response(fd, 0, NULL, 0);

    if ((uint64_t)req_size < sizeof(io) + (uint64_t)io.nwords * 2)
        return send_response(fd, -EINVAL, NULL, 0);

    io.words = (uint16_t *)(req + sizeof(io));
    ret = ecrt_tool_sii_write(master, &io);
    return send_response(fd, ret, NULL, 0);
}

/** EC_CMD_SLAVE_REG_READ — read slave registers. */
static int dispatch_slave_reg_read(int fd, ec_master_t *master,
        const uint8_t *req, uint32_t req_size)
{
    ec_tool_slave_reg_t io;
    uint8_t *data;
    int ret;

    if (req_size < sizeof(io))
        return send_response(fd, -EINVAL, NULL, 0);
    memcpy(&io, req, sizeof(io));

    if (!io.size)
        return send_response(fd, 0, NULL, 0);
    if (io.size > EC_IPC_MAX_RESPONSE)
        return send_response(fd, -EINVAL, NULL, 0);

    data = malloc(io.size);
    if (!data)
        return send_response(fd, -ENOMEM, NULL, 0);

    io.data = data;
    ret = ecrt_tool_reg_read(master, &io);
    if (ret) {
        free(data);
        return send_response(fd, ret, NULL, 0);
    }

    ret = send_response_with_trailing(fd, 0,
            &io, sizeof(io), data, io.size);
    free(data);
    return ret;
}

/** EC_CMD_SLAVE_REG_WRITE — write slave registers. */
static int dispatch_slave_reg_write(int fd, ec_master_t *master,
        const uint8_t *req, uint32_t req_size)
{
    ec_tool_slave_reg_t io;
    int ret;

    if (req_size < sizeof(io))
        return send_response(fd, -EINVAL, NULL, 0);
    memcpy(&io, req, sizeof(io));

    if (!io.size)
        return send_response(fd, 0, NULL, 0);

    if ((uint64_t)req_size < sizeof(io) + (uint64_t)io.size)
        return send_response(fd, -EINVAL, NULL, 0);

    io.data = (uint8_t *)(req + sizeof(io));
    ret = ecrt_tool_reg_write(master, &io);
    return send_response(fd, ret, NULL, 0);
}

/** EC_CMD_SLAVE_FOE_READ — read a file from a slave via FoE. */
static int dispatch_slave_foe_read(int fd, ec_master_t *master,
        const uint8_t *req, uint32_t req_size)
{
    ec_tool_slave_foe_t io;
    uint8_t *buffer;
    int ret;

    if (req_size < sizeof(io))
        return send_response(fd, -EINVAL, NULL, 0);
    memcpy(&io, req, sizeof(io));

    if (!io.buffer_size || io.buffer_size > EC_IPC_MAX_RESPONSE)
        return send_response(fd, -EINVAL, NULL, 0);

    buffer = malloc(io.buffer_size);
    if (!buffer)
        return send_response(fd, -ENOMEM, NULL, 0);

    io.buffer = buffer;
    ret = ecrt_tool_foe_read(master, &io);
    if (ret) {
        int send_ret = send_response(fd, ret, &io, sizeof(io));
        free(buffer);
        return send_ret;
    }

    ret = send_response_with_trailing(fd, 0,
            &io, sizeof(io), buffer, io.data_size);
    free(buffer);
    return ret;
}

/** EC_CMD_SLAVE_FOE_WRITE — write a file to a slave via FoE. */
static int dispatch_slave_foe_write(int fd, ec_master_t *master,
        const uint8_t *req, uint32_t req_size)
{
    ec_tool_slave_foe_t io;
    int ret;

    if (req_size < sizeof(io))
        return send_response(fd, -EINVAL, NULL, 0);
    memcpy(&io, req, sizeof(io));

    if ((uint64_t)req_size < sizeof(io) + (uint64_t)io.buffer_size)
        return send_response(fd, -EINVAL, NULL, 0);

    io.buffer = (uint8_t *)(req + sizeof(io));
    ret = ecrt_tool_foe_write(master, &io);
    return send_response(fd, ret, &io, sizeof(io));
}

/** EC_CMD_SLAVE_SOE_READ — read an SoE IDN from a slave. */
static int dispatch_slave_soe_read(int fd, ec_master_t *master,
        const uint8_t *req, uint32_t req_size)
{
    ec_tool_slave_soe_read_t io;
    uint8_t *data;
    int ret;

    if (req_size < sizeof(io))
        return send_response(fd, -EINVAL, NULL, 0);
    memcpy(&io, req, sizeof(io));

    if (!io.mem_size || io.mem_size > EC_IPC_MAX_RESPONSE)
        return send_response(fd, -EINVAL, NULL, 0);

    data = malloc(io.mem_size);
    if (!data)
        return send_response(fd, -ENOMEM, NULL, 0);

    io.data = data;
    ret = ecrt_tool_soe_read(master, &io);
    if (ret) {
        free(data);
        return send_response(fd, ret, &io, sizeof(io));
    }

    ret = send_response_with_trailing(fd, 0,
            &io, sizeof(io), data, io.data_size);
    free(data);
    return ret;
}

/** EC_CMD_SLAVE_SOE_WRITE — write an SoE IDN to a slave. */
static int dispatch_slave_soe_write(int fd, ec_master_t *master,
        const uint8_t *req, uint32_t req_size)
{
    ec_tool_slave_soe_write_t io;
    int ret;

    if (req_size < sizeof(io))
        return send_response(fd, -EINVAL, NULL, 0);
    memcpy(&io, req, sizeof(io));

    if ((uint64_t)req_size < sizeof(io) + (uint64_t)io.data_size)
        return send_response(fd, -EINVAL, NULL, 0);

    io.data = (uint8_t *)(req + sizeof(io));
    ret = ecrt_tool_soe_write(master, &io);
    return send_response(fd, ret, &io, sizeof(io));
}

/****************************************************************************/
/* Poll-based event loop                                                       */
/****************************************************************************/

/**
 * Handle exactly one request from a connected client fd.
 *
 * Reads the request header and payload, dispatches the command, and sends
 * the response.  Uses \a payload / \a payload_cap as a reusable buffer
 * owned by the caller.
 *
 * \return 0 to keep the connection open, non-zero to close it.
 */
static int handle_client_request(int fd, uint8_t **payload,
        uint32_t *payload_cap)
{
    ec_ipc_request_t req;
    ec_master_t *master;
    int ret;

    ret = recv_all(fd, &req, sizeof(req));
    if (ret < 0)
        return 1; /* client disconnected or error */

    /* Validate version magic. */
    if (req.version_magic != EC_IPC_VERSION_MAGIC) {
        ec_log(EC_LOG_WARNING,
                "IPC: version magic mismatch (%u vs %u); dropping client\n",
                req.version_magic, EC_IPC_VERSION_MAGIC);
        send_response(fd, -EINVAL, NULL, 0);
        return 1;
    }

    /* Reject oversized payloads. */
    if (req.data_size > EC_IPC_MAX_PAYLOAD) {
        ec_log(EC_LOG_WARNING,
                "IPC: payload too large (%u); dropping client\n",
                req.data_size);
        send_response(fd, -EINVAL, NULL, 0);
        return 1;
    }

    /* Grow the shared payload buffer if needed. */
    if (req.data_size > *payload_cap) {
        free(*payload);
        *payload_cap = 0;
        *payload = malloc(req.data_size);
        if (!*payload) {
            send_response(fd, -ENOMEM, NULL, 0);
            return 1;
        }
        *payload_cap = req.data_size;
    }

    if (req.data_size > 0) {
        ret = recv_all(fd, *payload, req.data_size);
        if (ret < 0)
            return 1;
    }

    /* EC_CMD_MODULE does not require a valid master. */
    if ((enum ec_tool_cmd)req.cmd == EC_CMD_MODULE) {
        dispatch_module(fd, NULL, *payload, req.data_size);
        return 0;
    }

    /* All other commands require a valid master. */
    master = ec_master_registry_get(req.master_index);
    if (!master) {
        send_response(fd, -EINVAL, NULL, 0);
        return 0;
    }

    /* Dispatch command. */
    switch ((enum ec_tool_cmd)req.cmd) {
        case EC_CMD_MODULE:
            /* handled above */
            break;
        case EC_CMD_MASTER:
            dispatch_master(fd, master, *payload, req.data_size);
            break;
        case EC_CMD_SLAVE:
            dispatch_slave(fd, master, *payload, req.data_size);
            break;
        case EC_CMD_SLAVE_SYNC:
            dispatch_slave_sync(fd, master, *payload, req.data_size);
            break;
        case EC_CMD_SLAVE_SYNC_PDO:
            dispatch_slave_sync_pdo(fd, master, *payload, req.data_size);
            break;
        case EC_CMD_SLAVE_SYNC_PDO_ENTRY:
            dispatch_slave_sync_pdo_entry(fd, master, *payload,
                    req.data_size);
            break;
        case EC_CMD_DOMAIN:
            dispatch_domain(fd, master, *payload, req.data_size);
            break;
        case EC_CMD_DOMAIN_FMMU:
            dispatch_domain_fmmu(fd, master, *payload, req.data_size);
            break;
        case EC_CMD_DOMAIN_DATA:
            dispatch_domain_data(fd, master, *payload, req.data_size);
            break;
        case EC_CMD_MASTER_DEBUG:
            dispatch_master_debug(fd, master, *payload, req.data_size);
            break;
        case EC_CMD_MASTER_RESCAN:
            dispatch_master_rescan(fd, master, *payload, req.data_size);
            break;
        case EC_CMD_SLAVE_STATE:
            dispatch_slave_state(fd, master, *payload, req.data_size);
            break;
        case EC_CMD_SLAVE_SDO:
            dispatch_slave_sdo(fd, master, *payload, req.data_size);
            break;
        case EC_CMD_SLAVE_SDO_ENTRY:
            dispatch_slave_sdo_entry(fd, master, *payload, req.data_size);
            break;
        case EC_CMD_SLAVE_SDO_UPLOAD:
            dispatch_slave_sdo_upload(fd, master, *payload, req.data_size);
            break;
        case EC_CMD_SLAVE_SDO_DOWNLOAD:
            dispatch_slave_sdo_download(fd, master, *payload, req.data_size);
            break;
        case EC_CMD_SLAVE_SII_READ:
            dispatch_slave_sii_read(fd, master, *payload, req.data_size);
            break;
        case EC_CMD_SLAVE_SII_WRITE:
            dispatch_slave_sii_write(fd, master, *payload, req.data_size);
            break;
        case EC_CMD_SLAVE_REG_READ:
            dispatch_slave_reg_read(fd, master, *payload, req.data_size);
            break;
        case EC_CMD_SLAVE_REG_WRITE:
            dispatch_slave_reg_write(fd, master, *payload, req.data_size);
            break;
        case EC_CMD_SLAVE_FOE_READ:
            dispatch_slave_foe_read(fd, master, *payload, req.data_size);
            break;
        case EC_CMD_SLAVE_FOE_WRITE:
            dispatch_slave_foe_write(fd, master, *payload, req.data_size);
            break;
        case EC_CMD_SLAVE_SOE_READ:
            dispatch_slave_soe_read(fd, master, *payload, req.data_size);
            break;
        case EC_CMD_SLAVE_SOE_WRITE:
            dispatch_slave_soe_write(fd, master, *payload, req.data_size);
            break;
#ifdef EC_EOE
        case EC_CMD_SLAVE_EOE_IP_PARAM:
            dispatch_eoe_ip(fd, master, *payload, req.data_size);
            break;
#endif
        case EC_CMD_CONFIG:
            dispatch_config(fd, master, *payload, req.data_size);
            break;
        case EC_CMD_CONFIG_PDO:
            dispatch_config_pdo(fd, master, *payload, req.data_size);
            break;
        case EC_CMD_CONFIG_PDO_ENTRY:
            dispatch_config_pdo_entry(fd, master, *payload, req.data_size);
            break;
        case EC_CMD_CONFIG_SDO:
            dispatch_config_sdo(fd, master, *payload, req.data_size);
            break;
        case EC_CMD_CONFIG_IDN:
            dispatch_config_idn(fd, master, *payload, req.data_size);
            break;
        case EC_CMD_CONFIG_FLAG:
            dispatch_config_flag(fd, master, *payload, req.data_size);
            break;
#ifdef EC_EOE
        case EC_CMD_CONFIG_EOE_IP_PARAM:
            dispatch_eoe_ip(fd, master, *payload, req.data_size);
            break;
        case EC_CMD_EOE_HANDLER:
            dispatch_eoe_handler(fd, master, *payload, req.data_size);
            break;
#endif
        default:
            send_response(fd, -ENOTTY, NULL, 0);
            break;
    }

    ec_master_registry_put(master);
    return 0;
}

/****************************************************************************/
/* Listener thread                                                             */
/****************************************************************************/

/**
 * Listener thread: poll()-based event loop that multiplexes the listening
 * socket and all connected client sockets in a single thread.
 *
 * On new connection: accept(), set SO_SNDTIMEO and SO_RCVTIMEO, add to poll array.
 * On client data:    read one full request, dispatch, send response.
 * On client error:   close fd and remove from poll array.
 * On shutdown:       exit the loop; close remaining client fds (the
 *                    listening socket is closed by ec_ipc_server_stop()).
 */
static int listener_fn(void *arg)
{
    ec_cdev_t *cdev = (ec_cdev_t *)arg;
    /* fds[0] = listening socket; fds[1..nfds-1] = connected clients */
    struct pollfd fds[1 + EC_IPC_MAX_CLIENTS];
    int nfds = 1;
    uint8_t *payload = NULL;
    uint32_t payload_cap = 0;
    int i;

    fds[0].fd     = cdev->sock_fd;
    fds[0].events = POLLIN;

    while (!atomic_load(&cdev->shutdown)) {
        int ret = poll(fds, (nfds_t)nfds, 1000);
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            if (!atomic_load(&cdev->shutdown))
                ec_log(EC_LOG_WARNING,
                        "IPC: poll() failed: %s\n", strerror(errno));
            break;
        }
        if (ret == 0)
            continue; /* timeout — recheck shutdown flag */

        /* New connection on the listening socket. */
        if (fds[0].revents & POLLIN) {
            struct sockaddr_un client_addr;
            socklen_t client_len = sizeof(client_addr);
            int client_fd = accept4(cdev->sock_fd,
                    (struct sockaddr *)&client_addr, &client_len,
                    SOCK_CLOEXEC);
            if (client_fd >= 0) {
                if (nfds < 1 + EC_IPC_MAX_CLIENTS) {
                    struct timeval tv;
                    tv.tv_sec  = 5;
                    tv.tv_usec = 0;
                    if (setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO,
                            &tv, sizeof(tv)) < 0)
                        ec_log(EC_LOG_WARNING,
                                "IPC: SO_SNDTIMEO failed: %s\n",
                                strerror(errno));
                    if (setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO,
                            &tv, sizeof(tv)) < 0)
                        ec_log(EC_LOG_WARNING,
                                "IPC: SO_RCVTIMEO failed: %s\n",
                                strerror(errno));
                    fds[nfds].fd      = client_fd;
                    fds[nfds].events  = POLLIN;
                    fds[nfds].revents = 0;
                    nfds++;
                } else {
                    ec_log(EC_LOG_WARNING,
                            "IPC: too many connections, rejecting\n");
                    close(client_fd);
                }
            } else if (!atomic_load(&cdev->shutdown)) {
                if (errno != EINTR && errno != EAGAIN &&
                        errno != EWOULDBLOCK)
                    ec_log(EC_LOG_WARNING,
                            "IPC: accept() failed: %s\n", strerror(errno));
            }
        }

        /* Service existing client connections. */
        for (i = 1; i < nfds; ) {
            if (!fds[i].revents) {
                i++;
                continue;
            }

            if (fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                close(fds[i].fd);
                fds[i] = fds[--nfds];
                fds[i].revents = 0; /* don't re-process moved entry's stale events */
                continue; /* recheck same index (now holds last entry) */
            }

            if (fds[i].revents & POLLIN) {
                int drop = handle_client_request(fds[i].fd,
                        &payload, &payload_cap);
                if (drop) {
                    close(fds[i].fd);
                    fds[i] = fds[--nfds];
                    fds[i].revents = 0; /* don't re-process moved entry's stale events */
                    continue; /* recheck same index */
                }
            }

            i++;
        }
    }

    /* Close all remaining client connections. */
    for (i = 1; i < nfds; i++)
        close(fds[i].fd);

    /* The listening socket is NOT closed here: ec_ipc_server_stop() owns
     * it and closes it after joining this thread. A close here could
     * race stop()'s shutdown() wakeup — the kernel may reuse the fd
     * number for an unrelated descriptor in between. */

    free(payload);
    return 0;
}

/****************************************************************************/
/* Public API                                                                  */
/****************************************************************************/

/**
 * ec_ipc_server_start - start the global IPC server.
 *
 * Creates a Unix-domain listening socket at \a socket_path and starts the
 * listener thread.  Called by ecrt_lib_init() when socket_path is not NULL.
 *
 * \return 0 on success, negative error code on failure.
 */
int ec_ipc_server_start(const char *socket_path)
{
    ec_cdev_t *cdev = &g_cdev;
    struct sockaddr_un addr;
    int sock_fd;
    int ret;

    if (cdev->sock_fd != -1)
        return 0; /* already running */

    /* A path that does not fit sun_path would be silently truncated and
     * the socket would appear somewhere else — reject it instead. */
    if (strlen(socket_path) >= sizeof(cdev->sock_path)) {
        ec_log(EC_LOG_ERR, "IPC: socket path too long (max %zu): %s\n",
                sizeof(cdev->sock_path) - 1, socket_path);
        return -ENAMETOOLONG;
    }

    snprintf(cdev->sock_path, sizeof(cdev->sock_path), "%s", socket_path);
    cdev->thread   = NULL;
    atomic_store(&cdev->shutdown, 0);

    /* Create the listening socket. */
    sock_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (sock_fd < 0) {
        ec_log(EC_LOG_ERR, "IPC: socket() failed: %s\n", strerror(errno));
        return -errno;
    }

    /* Remove any stale socket file. */
    unlink(cdev->sock_path);

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    /* sock_path fits sun_path (both UNIX_PATH_MAX, length checked above). */
    memcpy(addr.sun_path, cdev->sock_path, strlen(cdev->sock_path) + 1);

    if (bind(sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ret = -errno;
        ec_log(EC_LOG_ERR, "IPC: bind() to %s failed: %s\n",
                cdev->sock_path, strerror(errno));
        close(sock_fd);
        return ret;
    }

    /* The tool API includes write operations (SDO download, state changes,
     * register/SII writes), so the socket must not be world-connectable.
     * Permissions are checked at connect() time and no connection can be
     * established before listen(), so restricting here is race-free.
     * Access for a tool group is granted by chown()ing the socket file
     * after startup (see ec_master --socket-group). */
    if (chmod(cdev->sock_path, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP) < 0) {
        ret = -errno;
        ec_log(EC_LOG_ERR, "IPC: chmod() of %s failed: %s\n",
                cdev->sock_path, strerror(errno));
        close(sock_fd);
        unlink(cdev->sock_path);
        return ret;
    }

    if (listen(sock_fd, 8) < 0) {
        ret = -errno;
        ec_log(EC_LOG_ERR, "IPC: listen() failed: %s\n", strerror(errno));
        close(sock_fd);
        unlink(cdev->sock_path);
        return ret;
    }

    cdev->sock_fd = sock_fd;

    /* Start listener thread. */
    cdev->thread = ec_thread_run(listener_fn, cdev, "ec_ipc_listen");
    if (IS_ERR(cdev->thread)) {
        ret = (int)PTR_ERR(cdev->thread);
        cdev->thread = NULL;
        ec_log(EC_LOG_ERR, "IPC: failed to start listener thread: %d\n", ret);
        close(cdev->sock_fd);
        cdev->sock_fd = -1;
        unlink(cdev->sock_path);
        return ret;
    }

    ec_log(EC_LOG_INFO, "IPC server listening on %s\n", cdev->sock_path);
    return 0;
}

/**
 * ec_ipc_server_stop - shut down the global IPC server.
 *
 * Signals the listener thread to stop by setting the shutdown flag and calling
 * shutdown() on the listening socket (which wakes poll()), then waits for the
 * single listener thread to exit.  The listening fd is owned and closed
 * exclusively by this function, after the join — the listener thread never
 * closes it, so shutdown() here can never hit a reused fd number.  The
 * socket file is removed from the filesystem after the thread has joined.
 */
void ec_ipc_server_stop(void)
{
    ec_cdev_t *cdev = &g_cdev;

    if (!cdev->thread)
        return; /* was never started or already stopped */

    /* Signal shutdown to the listener thread. */
    atomic_store(&cdev->shutdown, 1);

    /* Wake poll() on the listening socket so the listener thread sees the
     * shutdown flag quickly.  sock_fd stays valid until we close it below,
     * after the join. */
    shutdown(cdev->sock_fd, SHUT_RDWR);

    /* Wait for the listener thread to exit (it closes client fds itself). */
    ec_thread_stop(cdev->thread);
    cdev->thread = NULL;

    close(cdev->sock_fd);
    cdev->sock_fd = -1;

    /* Remove the socket file. */
    unlink(cdev->sock_path);

    ec_log(EC_LOG_INFO, "IPC server on %s stopped\n", cdev->sock_path);
}

/****************************************************************************/
