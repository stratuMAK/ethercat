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
   Userspace EtherCAT master library lifecycle implementation.
*/

/****************************************************************************/

#include "pal.h"

#include "../device.h"
#include "../master.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/****************************************************************************/

/** Seconds of inactivity before giving up on the initial bus scan. */
#define EC_SCAN_PROGRESS_TIMEOUT 5

/****************************************************************************/

static atomic_flag lib_initialized = ATOMIC_FLAG_INIT;

/****************************************************************************/

int ecrt_lib_init(ec_log_cb_t log_cb, const char *socket_path)
{
    if (atomic_flag_test_and_set(&lib_initialized)) {
        ec_log(EC_LOG_WARNING, "ecrt_lib_init() called more than once\n");
        return -EBUSY;
    }

    ec_log_set_callback(log_cb);
    ec_master_init_static();

    /* Without an application log callback, RT-context messages go
     * through the lock-free ring drained by this thread; failure to
     * start it is non-fatal (falls back to direct stderr). */
    if (!log_cb && ec_pal_log_start() != 0) {
        ec_log(EC_LOG_WARNING, "Failed to start log drainer thread;"
                " falling back to direct stderr logging\n");
    }

    if (ec_pal_work_init() != 0) {
        ec_log(EC_LOG_ERR, "Failed to create system workqueue\n");
        ec_pal_log_stop();
        atomic_flag_clear(&lib_initialized);
        return -1;
    }

    if (ec_pal_irq_work_init() != 0) {
        ec_log(EC_LOG_ERR, "Failed to create IRQ work queue\n");
        ec_pal_work_cleanup();
        atomic_flag_clear(&lib_initialized);
        return -1;
    }

    /* Start IPC server only if socket_path is provided. */
    if (socket_path) {
        int ret = ec_ipc_server_start(socket_path);
        if (ret < 0) {
            ec_log(EC_LOG_WARNING,
                    "Failed to start IPC server on %s: %d; "
                    "ethercat tool will not be able to connect\n",
                    socket_path, ret);
            /* Non-fatal — master still usable via API */
        }
    }

    return 0;
}

/****************************************************************************/

/****************************************************************************/

/** Pin the NIC IRQs of the master's transports to \a cpu (-1: leave them).
 *  Once, at startup: the library does not move them afterwards.  A failure
 *  is reported and not fatal -- the bus works with the IRQs where they are,
 *  only its timing is at the mercy of that CPU's other load. */
static void pin_transport_irqs(ec_master_t *master, int cpu)
{
    ec_transport_t *transports[2];
    unsigned int i;
    int ret;

    if (cpu < 0) {
        EC_MASTER_INFO(master, "Transport IRQs not pinned (no CPU given).\n");
        return;
    }

    transports[0] = master->pal.transport;
    transports[1] = master->pal.backup_transport;
    for (i = 0; i < 2; i++) {
        if (!transports[i]) {
            continue;
        }
        ret = ec_transport_set_cpu_affinity(transports[i], cpu);
        if (ret > 0) {
            EC_MASTER_INFO(master, "Pinned %d transport IRQ(s) of %s to CPU %d.\n",
                    ret, transports[i]->interface, cpu);
        } else if (ret == -ENOSYS) {
            EC_MASTER_WARN(master, "Transport of %s cannot pin its IRQs,"
                    " leaving them where they are.\n", transports[i]->interface);
        } else {
            EC_MASTER_WARN(master, "Failed to pin the transport IRQs of %s"
                    " to CPU %d: %d\n", transports[i]->interface, cpu, ret);
        }
    }
}

/****************************************************************************/

ec_master_t *ecrt_startup_master(unsigned int index,
        ec_transport_t *transport,
        ec_transport_t *backup_transport,
        unsigned int debug_level,
        int run_on_cpu,
        int irq_cpu)
{
    ec_master_t *master;
    int ret;

    if (!transport) {
        ec_log(EC_LOG_ERR, "Main transport must not be NULL\n");
        return NULL;
    }

    if (!transport->interface[0]) {
        ec_log(EC_LOG_ERR, "Main transport interface name must not be empty\n");
        return NULL;
    }

#if EC_MAX_NUM_DEVICES < 2
    if (backup_transport) {
        ec_log(EC_LOG_ERR, "Backup transport given, but the master was built"
                " without redundancy support (configure --with-devices)\n");
        return NULL;
    }
#endif

    master = malloc(sizeof(ec_master_t));
    if (!master) {
        ec_log(EC_LOG_ERR, "Failed to allocate master context\n");
        return NULL;
    }
    memset(master, 0, sizeof(ec_master_t));

    /* Borrow transport pointers — caller owns them, library opens/closes */
    master->pal.transport = transport;
    master->pal.backup_transport = backup_transport;

    /* Open main transport (interface stored in transport->interface) */
    ret = ec_transport_open(master->pal.transport);
    if (ret < 0) {
        ec_log(EC_LOG_ERR, "Failed to open transport on %s: %d\n",
                master->pal.transport->interface, ret);
        goto out_free;
    }

    /* Get MAC address from transport */
    ret = ec_transport_get_mac(master->pal.transport, master->pal.main_mac);
    if (ret < 0) {
        ec_log(EC_LOG_ERR, "Failed to get MAC address: %d\n", ret);
        goto out_close_main;
    }

    /* Open backup transport and get its MAC (if present) */
    if (master->pal.backup_transport) {
        ret = ec_transport_open(master->pal.backup_transport);
        if (ret < 0) {
            ec_log(EC_LOG_ERR, "Failed to open backup transport on %s: %d\n",
                    master->pal.backup_transport->interface, ret);
            goto out_close_main;
        }

        ret = ec_transport_get_mac(master->pal.backup_transport,
                master->pal.backup_mac);
        if (ret < 0) {
            ec_log(EC_LOG_ERR, "Failed to get backup MAC address: %d\n", ret);
            goto out_close_backup;
        }
    }

    /* Initialize master */
    ret = ec_master_init(master, index, master->pal.main_mac,
            master->pal.backup_mac, debug_level,
            run_on_cpu < 0 ? 0xffffffff : (unsigned int)run_on_cpu);
    if (ret < 0) {
        ec_log(EC_LOG_ERR, "Failed to initialize master: %d\n", ret);
        goto out_close_backup;
    }

    /* Store transport reference in device; interface name borrowed from transport */
    master->devices[EC_DEVICE_MAIN].pal.transport = master->pal.transport;
    master->devices[EC_DEVICE_MAIN].name = master->pal.transport->interface;

    /* Open device */
    ret = ec_device_open(&master->devices[EC_DEVICE_MAIN]);
    if (ret < 0) {
        ec_log(EC_LOG_ERR, "Failed to open device: %d\n", ret);
        goto out_clear_master;
    }

#if EC_MAX_NUM_DEVICES > 1
    /* Set up backup device if backup transport is present */
    if (master->pal.backup_transport) {
        master->devices[EC_DEVICE_BACKUP].pal.transport =
                master->pal.backup_transport;
        master->devices[EC_DEVICE_BACKUP].name =
                master->pal.backup_transport->interface;
        ret = ec_device_open(&master->devices[EC_DEVICE_BACKUP]);
        if (ret < 0) {
            ec_log(EC_LOG_ERR, "Failed to open backup device: %d\n", ret);
            goto out_close_main_device;
        }
    }
#endif

    /* The NIC IRQs go to their CPU before the idle phase starts the bus
     * scan: on a CPU shared with the rest of the system the scan's replies
     * can be delivered later than the datagram timeout, and the scan
     * fails (the slaves' SII is never read). */
    pin_transport_irqs(master, irq_cpu);

    /* Enter idle phase */
    ret = ec_master_enter_idle_phase(master);
    if (ret < 0) {
        ec_log(EC_LOG_ERR, "Failed to enter idle phase: %d\n", ret);
        goto out_close_backup_device;
    }

    /* Wait for the initial bus scan to complete before returning to the
     * caller. This ensures that when the application calls
     * ecrt_master_slave_config(), slaves have been scanned and their SII
     * data (including default PDO mappings) is available.
     *
     * Activity-based scan timeout: reset on every scan begun and every
     * slave scanned.  This handles both empty buses (timeout after ~5s) and
     * large or rescanned buses (keeps waiting while the scan progresses). */
    {
        /* Progress is any scan begun or slave scanned: a repeated startup
         * scan (EC_INITIAL_SCAN_ATTEMPTS) finds the same slaves again, so
         * the slave count alone would read as a stall. */
        unsigned int prev_progress = master->scan_progress;
        while (!master->initial_scan_done) {
            ec_wq_wait_timeout(master->scan_queue, master->initial_scan_done,
                    EC_SCAN_PROGRESS_TIMEOUT);
            if (master->initial_scan_done)
                break;
            if (master->scan_progress == prev_progress) {
                /* No progress — bus empty or stalled */
                EC_MASTER_WARN(master,
                        "Initial bus scan timed out after %u seconds"
                        " without progress. %u slave(s) found so far."
                        " Scan continues in background.\n",
                        EC_SCAN_PROGRESS_TIMEOUT, master->slave_count);
                break;
            }
            /* Progress was made, keep waiting */
            EC_MASTER_DBG(master, 1,
                    "Scan progress: %u slave(s) found so far,"
                    " resetting timeout.\n", master->slave_count);
            prev_progress = master->scan_progress;
        }
    }
    EC_MASTER_DBG(master, 1, "Initial bus scan complete, %u slave(s) found.\n",
            master->slave_count);

    /* Register master in global registry for IPC dispatch. */
    ec_master_registry_add(master);

    return master;

out_close_backup_device:
#if EC_MAX_NUM_DEVICES > 1
    if (master->pal.backup_transport) {
        ec_device_close(&master->devices[EC_DEVICE_BACKUP]);
    }
out_close_main_device:
#endif
    ec_device_close(&master->devices[EC_DEVICE_MAIN]);
out_clear_master:
    ec_master_clear(master);
out_close_backup:
    if (master->pal.backup_transport) {
        ec_transport_close(master->pal.backup_transport);
    }
out_close_main:
    ec_transport_close(master->pal.transport);
out_free:
    free(master);
    return NULL;
}

/****************************************************************************/

/** Internal master teardown — stops threads, closes devices/transports, frees
 *  memory. Does NOT touch the registry (caller is responsible for removing
 *  the master from the registry before calling this).
 */
static void release_master_internal(ec_master_t *master)
{
    if (master->phase != EC_ORPHANED) {
        if (master->active) {
            ec_master_leave_operation_phase(master);
        }
        ec_master_leave_idle_phase(master);
    }

#if EC_MAX_NUM_DEVICES > 1
    if (master->pal.backup_transport) {
        ec_device_close(&master->devices[EC_DEVICE_BACKUP]);
    }
#endif
    ec_device_close(&master->devices[EC_DEVICE_MAIN]);
    ec_master_clear(master);

    /* Close transports — caller still owns them and must call
     * ec_transport_destroy() afterwards. */
    if (master->pal.backup_transport) {
        ec_transport_close(master->pal.backup_transport);
    }
    if (master->pal.transport) {
        ec_transport_close(master->pal.transport);
    }

    free(master);
}

/****************************************************************************/

void ecrt_release_master(ec_master_t *master)
{
    if (!master) return;

    /* Unregister master from global registry before shutting down. */
    ec_master_registry_remove(master);

    release_master_internal(master);
}

/****************************************************************************/

void ecrt_lib_cleanup(void)
{
    unsigned int active_masters = ec_master_registry_count();
    ec_master_t *master;

    if (active_masters) {
        ec_log(EC_LOG_WARNING,
                "ecrt_lib_cleanup() called with %u master(s) still active, releasing them\n",
                active_masters);
        /* pop_first() unregisters under lock; release_master_internal()
         * performs teardown without a redundant registry scan. */
        while ((master = ec_master_registry_pop_first()) != NULL)
            release_master_internal(master);
    }

    ec_ipc_server_stop();
    ec_pal_irq_work_cleanup();
    ec_pal_work_cleanup();
    ec_pal_log_stop(); /* last: drains remaining messages */
    atomic_flag_clear(&lib_initialized);
}

/****************************************************************************/
