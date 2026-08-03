/******************************************************************************
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
 *****************************************************************************/

/**
 * \file
 * Raw socket transport implementation using AF_PACKET.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <net/ethernet.h>
#include <linux/if_packet.h>
#include <arpa/inet.h>

#include "ectp.h"
#include "irq_pin.h"

/****************************************************************************/

/** Private data for raw socket transport */
typedef struct {
    int socket_fd;                     /**< AF_PACKET socket */
    int if_index;                      /**< Interface index */
    struct sockaddr_ll socket_addr;    /**< Socket address for sending */
    uint8_t mac_addr[6];               /**< Interface MAC address */
    ec_irq_set_t irqs;                 /**< Cached NIC IRQs (count 0 = not discovered) */
} ec_transport_raw_t;

/****************************************************************************/

/**
 * Open raw socket transport on interface.
 */
static int raw_open(ec_transport_t *transport, const char *interface)
{
    ec_transport_raw_t *raw;
    struct ifreq ifr;
    int ret;
    int flags;

    /* Allocate private data */
    raw = calloc(1, sizeof(ec_transport_raw_t));
    if (!raw) {
        return -ENOMEM;
    }

    raw->socket_fd = -1;
    transport->priv = raw;

    /* Create AF_PACKET socket */
    raw->socket_fd = socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC,
            htons(EC_TRANSPORT_ETHERTYPE));
    if (raw->socket_fd < 0) {
        ret = -errno;
        fprintf(stderr, "Failed to create raw socket: %s\n", strerror(errno));
        goto err_free;
    }

    /* Get interface index */
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, interface, IFNAMSIZ - 1);
    ifr.ifr_name[IFNAMSIZ - 1] = '\0';
    if (ioctl(raw->socket_fd, SIOCGIFINDEX, &ifr) < 0) {
        ret = -errno;
        fprintf(stderr, "Failed to get interface index for %s: %s\n",
                interface, strerror(errno));
        goto err_close;
    }
    raw->if_index = ifr.ifr_ifindex;

    /* Get MAC address */
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, interface, IFNAMSIZ - 1);
    ifr.ifr_name[IFNAMSIZ - 1] = '\0';
    if (ioctl(raw->socket_fd, SIOCGIFHWADDR, &ifr) < 0) {
        ret = -errno;
        fprintf(stderr, "Failed to get MAC address for %s: %s\n",
                interface, strerror(errno));
        goto err_close;
    }
    memcpy(raw->mac_addr, ifr.ifr_hwaddr.sa_data, 6);

    /* Bind socket to interface */
    memset(&raw->socket_addr, 0, sizeof(raw->socket_addr));
    raw->socket_addr.sll_family = AF_PACKET;
    raw->socket_addr.sll_protocol = htons(EC_TRANSPORT_ETHERTYPE);
    raw->socket_addr.sll_ifindex = raw->if_index;
    raw->socket_addr.sll_halen = ETH_ALEN;
    /* Broadcast destination for EtherCAT */
    memset(raw->socket_addr.sll_addr, 0xff, ETH_ALEN);

    if (bind(raw->socket_fd, (struct sockaddr *)&raw->socket_addr,
             sizeof(raw->socket_addr)) < 0) {
        ret = -errno;
        fprintf(stderr, "Failed to bind socket to %s: %s\n",
                interface, strerror(errno));
        goto err_close;
    }

    /* Set socket to non-blocking mode */
    flags = fcntl(raw->socket_fd, F_GETFL, 0);
    if (flags < 0) {
        ret = -errno;
        goto err_close;
    }
    if (fcntl(raw->socket_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        ret = -errno;
        goto err_close;
    }

    /* Discover NIC IRQs for affinity pinning (best-effort, non-fatal) */
    ec_irq_discover(interface, &raw->irqs);

    return 0;

err_close:
    close(raw->socket_fd);
    raw->socket_fd = -1;
err_free:
    free(raw);
    transport->priv = NULL;
    return ret;
}

/****************************************************************************/

/**
 * Close raw socket transport.
 */
static void raw_close(ec_transport_t *transport)
{
    ec_transport_raw_t *raw = transport->priv;

    if (raw) {
        if (raw->socket_fd >= 0) {
            close(raw->socket_fd);
            raw->socket_fd = -1;
        }
        free(raw);
        transport->priv = NULL;
    }
}

/****************************************************************************/

/**
 * Get TX buffer pointer.
 */
static uint8_t *raw_get_tx_buffer(ec_transport_t *transport)
        ECRT_RT_ATTR;
static uint8_t *raw_get_tx_buffer(ec_transport_t *transport)
{
    return transport->tx_buffer;
}

/****************************************************************************/

/**
 * Send frame.
 */
/* TRUSTED: sendto() on a socket opened with O_NONBLOCK is a
 * nonblocking syscall; the function-effects analysis cannot see
 * that. */
static int raw_send(ec_transport_t *transport, size_t size)
        ECRT_RT_ATTR;
ECRT_RT_TRUSTED_BEGIN
static int raw_send(ec_transport_t *transport, size_t size)
{
    ec_transport_raw_t *raw = transport->priv;
    ssize_t ret;

    if (!raw || raw->socket_fd < 0) {
        return -ENODEV;
    }

    if (size > EC_TRANSPORT_MAX_FRAME_SIZE) {
        return -EINVAL;
    }

    do {
        ret = sendto(raw->socket_fd, transport->tx_buffer, size, 0,
                     (struct sockaddr *)&raw->socket_addr,
                     sizeof(raw->socket_addr));
    } while (ret < 0 && errno == EINTR);

    if (ret < 0) {
        return -errno;
    }

    if ((size_t)ret != size) {
        return -EIO;
    }

    return 0;
}
ECRT_RT_TRUSTED_END

/****************************************************************************/

/**
 * Receive frame (non-blocking).
 */
/* TRUSTED: recvfrom(MSG_DONTWAIT) is a nonblocking syscall; the
 * function-effects analysis cannot see that. */
static int raw_receive(ec_transport_t *transport, uint8_t *buffer,
        size_t max_size) ECRT_RT_ATTR;
ECRT_RT_TRUSTED_BEGIN
static int raw_receive(ec_transport_t *transport, uint8_t *buffer, size_t max_size)
{
    ec_transport_raw_t *raw = transport->priv;
    struct sockaddr_ll addr;
    socklen_t addr_len = sizeof(addr);
    ssize_t ret;

    if (!raw || raw->socket_fd < 0) {
        return -ENODEV;
    }

    /* Keep trying until we get a real packet or no more data */
    while (1) {
        addr_len = sizeof(addr);  /* Reset for each call */
        ret = recvfrom(raw->socket_fd, buffer, max_size, MSG_DONTWAIT,
                       (struct sockaddr *)&addr, &addr_len);

        if (ret < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0;  /* No data available */
            }
            return -errno;
        }

        /* Filter out our own transmitted packets - try again */
        if (addr.sll_pkttype == PACKET_OUTGOING) {
            continue;  /* Skip this packet, try next */
        }

        return (int)ret;
    }
}
ECRT_RT_TRUSTED_END

/****************************************************************************/

/**
 * Get link state.
 */
static int raw_get_link_state(ec_transport_t *transport)
{
    ec_transport_raw_t *raw = transport->priv;
    struct ifreq ifr;

    if (!raw || raw->socket_fd < 0) {
        return -ENODEV;
    }

    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, IFNAMSIZ, "%.*s", IFNAMSIZ - 1, transport->interface);

    if (ioctl(raw->socket_fd, SIOCGIFFLAGS, &ifr) < 0) {
        return -errno;
    }

    /* Check if interface is up and running */
    if ((ifr.ifr_flags & IFF_UP) && (ifr.ifr_flags & IFF_RUNNING)) {
        return 1;  /* Link up */
    }

    return 0;  /* Link down */
}

/****************************************************************************/

/**
 * Get MAC address.
 */
static int raw_get_mac(ec_transport_t *transport, uint8_t mac[6])
{
    ec_transport_raw_t *raw = transport->priv;

    if (!raw) {
        return -ENODEV;
    }

    memcpy(mac, raw->mac_addr, 6);
    return 0;
}

/****************************************************************************/

/**
 * Get file descriptor for polling.
 */
static int raw_get_fd(ec_transport_t *transport)
{
    ec_transport_raw_t *raw = transport->priv;

    if (!raw) {
        return -1;
    }

    return raw->socket_fd;
}

/****************************************************************************/

/**
 * Set CPU affinity for all NIC IRQs.
 */
static int raw_set_cpu_affinity(ec_transport_t *transport, int cpu)
{
    ec_transport_raw_t *raw = transport->priv;

    if (!raw || raw->irqs.count <= 0) {
        return -ENODEV;
    }

    return ec_irq_set_affinity(&raw->irqs, cpu);
}

/****************************************************************************/

/** Raw socket transport operations */
const ec_transport_ops_t ec_transport_raw_ops = {
    .name = "raw",
    .open = raw_open,
    .close = raw_close,
    .get_tx_buffer = raw_get_tx_buffer,
    .send = raw_send,
    .receive = raw_receive,
    .get_link_state = raw_get_link_state,
    .get_mac = raw_get_mac,
    .get_fd = raw_get_fd,
    .set_cpu_affinity = raw_set_cpu_affinity,
};

/****************************************************************************/
