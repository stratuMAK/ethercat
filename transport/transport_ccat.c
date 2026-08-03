/******************************************************************************
 *
 *  Network Driver for Beckhoff CCAT communication controller
 *  Copyright (C) 2014-2015  Beckhoff Automation GmbH
 *  Author: Patrick Bruenn <p.bruenn@beckhoff.com>
 *
 *  Userspace transport adaptation:
 *  Copyright (C) 2024  Sascha Ittner
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
 * CCAT EIM (Embedded I/O Memory) transport implementation.
 *
 * Directly accesses the Beckhoff CCAT FPGA PCI device from userspace
 * via sysfs resource mmap (BAR 0). No kernel module required.
 *
 * The "interface" string is the PCI BDF address, e.g. "0000:01:00.0".
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "ectp.h"

/****************************************************************************/

/** PCI vendor/device IDs */
#define CCAT_PCI_VENDOR_ID  0x15EC
#define CCAT_PCI_DEVICE_ID  0x5000

/** CCAT info block types */
#define CCATINFO_NOTUSED           0x0000
#define CCATINFO_ETHERCAT_NODMA    0x0003

/** Frame slot size in EIM mode */
#define CCAT_FRAME_SIZE     0x0800

/** Maximum Ethernet payload within a CCAT EIM frame */
#define CCAT_MAX_PAYLOAD    (CCAT_FRAME_SIZE - sizeof(struct ccat_eim_frame_hdr))

/** TX FIFO level register offset from MAC base */
#define TX_FIFO_LEVEL_OFFSET  0x20
#define TX_FIFO_LEVEL_MASK    0x3F

/** Link state: MII register + 0x8 + 4, bit 24 */
#define MII_LINK_OFFSET       0x0C
#define MII_LINK_BIT          (1 << 24)

/** MAC address offset from MII base */
#define MII_MAC_OFFSET        0x08

/****************************************************************************/

/** CCAT info block as read from BAR 0 (16 bytes) */
struct ccat_info_block {
    uint16_t type;
    uint16_t rev;
    union {
        uint32_t config;
        struct {
            uint16_t tx_size;
            uint16_t rx_size;
        };
    };
    uint32_t addr;
    uint32_t size;
};

/** MAC info block (7 x uint32_t offsets from function base) */
struct ccat_mac_infoblock {
    uint32_t reserved;
    uint32_t mii;
    uint32_t tx_fifo;
    uint32_t mac;
    uint32_t rx_mem;
    uint32_t tx_mem;
    uint32_t misc;
};

/** EIM frame header (as seen in memory-mapped RX/TX regions) */
struct ccat_eim_frame_hdr {
    uint16_t length;
    uint16_t reserved3;
    uint32_t tx_flags;
    uint64_t timestamp;
};

/****************************************************************************/

/** Private data for CCAT EIM transport */
typedef struct {
    int bar0_fd;                /**< fd for sysfs resource0 */
    void *bar0;                 /**< mmap'd BAR 0 base */
    size_t bar0_size;           /**< BAR 0 size */

    /* Register pointers (all within bar0 mmap) */
    volatile void *mii;         /**< MII register base */
    volatile void *tx_fifo_reg; /**< TX FIFO kick register */
    volatile void *rx_fifo_reg; /**< RX FIFO register (tx_fifo + 0x10) */
    volatile void *mac;         /**< MAC register base */
    volatile void *rx_mem;      /**< RX frame memory base */
    volatile void *tx_mem;      /**< TX frame memory base */

    /* FIFO state */
    size_t tx_size;             /**< Total TX memory size (from info block) */
    size_t rx_size;             /**< Total RX memory size (from info block) */
    uint32_t rx_offset;         /**< Current RX frame offset */
    uint32_t tx_offset;         /**< Current TX frame offset */

    /* Cached MAC address */
    uint8_t mac_addr[6];
} ec_transport_ccat_t;

/****************************************************************************/

/** Read a 8-bit value from memory-mapped register */
static inline uint8_t ccat_read8(volatile void *addr)
{
    return *(volatile uint8_t *)addr;
}

/** Read a 16-bit value from memory-mapped register */
static inline uint16_t ccat_read16(volatile void *addr)
{
    return *(volatile uint16_t *)addr;
}

/** Read a 32-bit value from memory-mapped register */
static inline uint32_t ccat_read32(volatile void *addr)
{
    return *(volatile uint32_t *)addr;
}

/** Write a 16-bit value to memory-mapped register */
static inline void ccat_write16(volatile void *addr, uint16_t val)
{
    *(volatile uint16_t *)addr = val;
}

/** Write a 32-bit value to memory-mapped register */
static inline void ccat_write32(volatile void *addr, uint32_t val)
{
    *(volatile uint32_t *)addr = val;
    __sync_synchronize();  /* write memory barrier */
}

/****************************************************************************/

/**
 * Get BAR 0 size from sysfs.
 */
static ssize_t ccat_get_bar0_size(const char *bdf)
{
    char path[256];
    FILE *f;
    unsigned long long start, end, flags;
    int ret;

    ret = snprintf(path, sizeof(path),
                   "/sys/bus/pci/devices/%s/resource", bdf);
    if (ret < 0 || (size_t)ret >= sizeof(path))
        return -EINVAL;

    f = fopen(path, "r");
    if (!f)
        return -errno;

    /* First line is BAR 0 */
    ret = fscanf(f, "%llx %llx %llx", &start, &end, &flags);
    fclose(f);

    if (ret != 3 || end <= start)
        return -EINVAL;

    return (ssize_t)(end - start + 1);
}

/**
 * Verify this is actually a CCAT device.
 */
static int ccat_verify_device(const char *bdf)
{
    char path[256];
    FILE *f;
    unsigned int val;
    int ret;

    /* Check vendor */
    ret = snprintf(path, sizeof(path),
                   "/sys/bus/pci/devices/%s/vendor", bdf);
    if (ret < 0 || (size_t)ret >= sizeof(path))
        return -EINVAL;

    f = fopen(path, "r");
    if (!f)
        return -errno;
    ret = fscanf(f, "%x", &val);
    fclose(f);
    if (ret != 1 || val != CCAT_PCI_VENDOR_ID) {
        fprintf(stderr, "CCAT: %s vendor 0x%04x != expected 0x%04x\n",
                bdf, val, CCAT_PCI_VENDOR_ID);
        return -ENODEV;
    }

    /* Check device */
    ret = snprintf(path, sizeof(path),
                   "/sys/bus/pci/devices/%s/device", bdf);
    if (ret < 0 || (size_t)ret >= sizeof(path))
        return -EINVAL;

    f = fopen(path, "r");
    if (!f)
        return -errno;
    ret = fscanf(f, "%x", &val);
    fclose(f);
    if (ret != 1 || val != CCAT_PCI_DEVICE_ID) {
        fprintf(stderr, "CCAT: %s device 0x%04x != expected 0x%04x\n",
                bdf, val, CCAT_PCI_DEVICE_ID);
        return -ENODEV;
    }

    return 0;
}

/**
 * Find the EtherCAT EIM function block in BAR 0.
 *
 * Scans info blocks starting at BAR 0 base. The number of function blocks
 * is at offset 0x4 (byte).
 *
 * Returns 0 on success and fills in the info_block struct.
 */
static int ccat_find_eim_function(void *bar0,
                                  struct ccat_info_block *info_out)
{
    uint8_t num_func = *(volatile uint8_t *)((uint8_t *)bar0 + 4);
    unsigned int i;
    struct ccat_info_block *blocks = (struct ccat_info_block *)bar0;

    for (i = 0; i < num_func; i++) {
        if (blocks[i].type == CCATINFO_ETHERCAT_NODMA) {
            *info_out = blocks[i];
            return 0;
        }
    }

    fprintf(stderr, "CCAT: no EIM function block found (%u blocks scanned)\n",
            num_func);
    return -ENOENT;
}

/****************************************************************************/

/**
 * Open CCAT EIM transport.
 *
 * @param interface PCI BDF address, e.g. "0000:01:00.0"
 */
static int ccat_open(ec_transport_t *transport, const char *interface)
{
    ec_transport_ccat_t *ccat;
    char path[256];
    ssize_t bar0_size;
    struct ccat_info_block info;
    struct ccat_mac_infoblock mac_info;
    void *func_base;
    int ret;

    if (!interface || !interface[0]) {
        fprintf(stderr, "CCAT: PCI BDF address required (e.g. \"0000:01:00.0\")\n");
        return -EINVAL;
    }

    /* Validate BDF format (basic check) */
    if (strlen(interface) < 7 || strlen(interface) > 15) {
        fprintf(stderr, "CCAT: invalid PCI BDF address: %s\n", interface);
        return -EINVAL;
    }

    /* Verify vendor/device */
    ret = ccat_verify_device(interface);
    if (ret)
        return ret;

    /* Allocate private data */
    ccat = calloc(1, sizeof(ec_transport_ccat_t));
    if (!ccat)
        return -ENOMEM;

    ccat->bar0_fd = -1;
    transport->priv = ccat;

    /* Get BAR 0 size */
    bar0_size = ccat_get_bar0_size(interface);
    if (bar0_size <= 0) {
        fprintf(stderr, "CCAT: failed to determine BAR 0 size for %s\n",
                interface);
        ret = (bar0_size < 0) ? (int)bar0_size : -EIO;
        goto err_free;
    }
    ccat->bar0_size = (size_t)bar0_size;

    /* Open and mmap BAR 0 via sysfs */
    ret = snprintf(path, sizeof(path),
                   "/sys/bus/pci/devices/%s/resource0", interface);
    if (ret < 0 || (size_t)ret >= sizeof(path)) {
        ret = -EINVAL;
        goto err_free;
    }

    ccat->bar0_fd = open(path, O_RDWR | O_SYNC | O_CLOEXEC);
    if (ccat->bar0_fd < 0) {
        ret = -errno;
        fprintf(stderr, "CCAT: failed to open %s: %s\n",
                path, strerror(errno));
        goto err_free;
    }

    ccat->bar0 = mmap(NULL, ccat->bar0_size, PROT_READ | PROT_WRITE,
                       MAP_SHARED, ccat->bar0_fd, 0);
    if (ccat->bar0 == MAP_FAILED) {
        ret = -errno;
        fprintf(stderr, "CCAT: failed to mmap BAR 0: %s\n", strerror(errno));
        ccat->bar0 = NULL;
        goto err_close;
    }

    /* Find EIM function block */
    ret = ccat_find_eim_function(ccat->bar0, &info);
    if (ret)
        goto err_unmap;

    /* Parse MAC info block at function base */
    func_base = (uint8_t *)ccat->bar0 + info.addr;
    memcpy(&mac_info, func_base, sizeof(mac_info));

    /* Set up register pointers */
    ccat->mii = (uint8_t *)func_base + mac_info.mii;
    ccat->tx_fifo_reg = (uint8_t *)func_base + mac_info.tx_fifo;
    ccat->rx_fifo_reg = (uint8_t *)func_base + mac_info.tx_fifo + 0x10;
    ccat->mac = (uint8_t *)func_base + mac_info.mac;
    ccat->rx_mem = (uint8_t *)func_base + mac_info.rx_mem;
    ccat->tx_mem = (uint8_t *)func_base + mac_info.tx_mem;

    /* Store frame memory sizes from info block */
    ccat->rx_size = info.rx_size ? info.rx_size : CCAT_FRAME_SIZE;
    ccat->tx_size = info.tx_size ? info.tx_size : CCAT_FRAME_SIZE;

    /* Initialize FIFO positions */
    ccat->rx_offset = 0;
    ccat->tx_offset = 0;

    /* Read MAC address from MII + 8 */
    memcpy(ccat->mac_addr, (void *)((uint8_t *)ccat->mii + MII_MAC_OFFSET), 6);

    /* Disable MAC filter (same as kernel driver) */
    *(volatile uint8_t *)((uint8_t *)ccat->mii + 0x8 + 6) = 0;
    __sync_synchronize();

    /* Reset FIFOs */
    ccat_write32(ccat->tx_fifo_reg + 0x8, 0);
    ccat_write32(ccat->rx_fifo_reg + 0x8, 0);

    /* Clear all RX frame headers (mark as consumed) */
    {
        uint32_t offset;
        for (offset = 0; offset + CCAT_FRAME_SIZE <= ccat->rx_size;
             offset += CCAT_FRAME_SIZE) {
            volatile void *frame = (uint8_t *)ccat->rx_mem + offset;
            ccat_write16(frame, 0);
        }
    }

    fprintf(stderr, "CCAT: opened %s (BAR0 %zu bytes, EIM mode, "
            "MAC %02x:%02x:%02x:%02x:%02x:%02x)\n",
            interface, ccat->bar0_size,
            ccat->mac_addr[0], ccat->mac_addr[1], ccat->mac_addr[2],
            ccat->mac_addr[3], ccat->mac_addr[4], ccat->mac_addr[5]);

    return 0;

err_unmap:
    munmap(ccat->bar0, ccat->bar0_size);
    ccat->bar0 = NULL;
err_close:
    close(ccat->bar0_fd);
    ccat->bar0_fd = -1;
err_free:
    free(ccat);
    transport->priv = NULL;
    return ret;
}

/****************************************************************************/

/**
 * Close CCAT transport.
 */
static void ccat_close(ec_transport_t *transport)
{
    ec_transport_ccat_t *ccat = transport->priv;

    if (ccat) {
        if (ccat->bar0 && ccat->bar0 != MAP_FAILED) {
            /* Reset FIFOs before unmapping */
            ccat_write32(ccat->tx_fifo_reg + 0x8, 0);
            ccat_write32(ccat->rx_fifo_reg + 0x8, 0);
            munmap(ccat->bar0, ccat->bar0_size);
        }
        if (ccat->bar0_fd >= 0)
            close(ccat->bar0_fd);
        free(ccat);
        transport->priv = NULL;
    }
}

/****************************************************************************/

/**
 * Get TX buffer pointer.
 *
 * Returns a pointer into the transport's tx_buffer (in main memory).
 * The actual copy to CCAT hardware happens in send().
 */
static uint8_t *ccat_get_tx_buffer(ec_transport_t *transport)
        ECRT_RT_ATTR;
static uint8_t *ccat_get_tx_buffer(ec_transport_t *transport)
{
    return transport->tx_buffer;
}

/****************************************************************************/

/**
 * Send frame to CCAT via EIM.
 *
 * Copies frame data into the current TX slot in CCAT memory-mapped region,
 * then kicks the TX FIFO register.
 */
static int ccat_send(ec_transport_t *transport, size_t size)
        ECRT_RT_ATTR;
static int ccat_send(ec_transport_t *transport, size_t size)
{
    ec_transport_ccat_t *ccat = transport->priv;
    volatile void *frame;
    uint32_t addr_and_length;
    uint16_t len16;

    if (!ccat)
        return -ENODEV;

    if (size > CCAT_MAX_PAYLOAD || size == 0)
        return -EINVAL;

    /* Check TX FIFO level - can we send? */
    /* TX FIFO still busy: drop the frame (the datagrams time out and
     * are retried by the FSMs; cyclic domain data is resent next cycle
     * anyway). Deliberate: blocking here would stall the cyclic path,
     * and at EtherCAT cycle times the FIFO is empty again long before
     * the next frame. */
    if (ccat_read8((uint8_t *)ccat->mac + TX_FIFO_LEVEL_OFFSET)
        & TX_FIFO_LEVEL_MASK) {
        return -EBUSY;
    }

    /* Current TX frame slot */
    frame = (uint8_t *)ccat->tx_mem + ccat->tx_offset;

    /* Write frame length */
    len16 = (uint16_t)size;
    ccat_write16((void *)frame + offsetof(struct ccat_eim_frame_hdr, length),
                 len16);

    /* Copy frame data after header */
    memcpy((void *)((uint8_t *)frame + sizeof(struct ccat_eim_frame_hdr)),
           transport->tx_buffer, size);

    /* Kick TX FIFO: write offset to tx_fifo register */
    addr_and_length = ccat->tx_offset;
    ccat_write32((void *)ccat->tx_fifo_reg, addr_and_length);

    /* Advance to next TX slot */
    ccat->tx_offset += CCAT_FRAME_SIZE;
    if (ccat->tx_offset + CCAT_FRAME_SIZE > ccat->tx_size)
        ccat->tx_offset = 0;

    return (int)size;
}

/****************************************************************************/

/**
 * Receive frame from CCAT via EIM (non-blocking).
 *
 * Checks the current RX slot for a received frame. If available, copies
 * the frame data to the output buffer and clears the slot.
 *
 * Returns number of bytes received, 0 if no frame available, or negative
 * error code.
 */
static int ccat_receive(ec_transport_t *transport, uint8_t *buffer,
        size_t max_size) ECRT_RT_ATTR;
static int ccat_receive(ec_transport_t *transport, uint8_t *buffer,
                        size_t max_size)
{
    ec_transport_ccat_t *ccat = transport->priv;
    volatile void *frame;
    uint16_t len;
    size_t payload_len;

    if (!ccat)
        return -ENODEV;

    /* Check current RX slot */
    frame = (uint8_t *)ccat->rx_mem + ccat->rx_offset;
    len = ccat_read16((void *)frame);

    if (len == 0)
        return 0;  /* No frame available */

    /* Length field includes the EIM header overhead */
    if (len <= sizeof(struct ccat_eim_frame_hdr)) {
        /* Invalid/corrupt frame - clear and advance */
        ccat_write16((void *)frame, 0);
        ccat->rx_offset += CCAT_FRAME_SIZE;
        if (ccat->rx_offset + CCAT_FRAME_SIZE > ccat->rx_size)
            ccat->rx_offset = 0;
        return 0;
    }

    payload_len = len - sizeof(struct ccat_eim_frame_hdr);
    if (payload_len > max_size)
        payload_len = max_size;

    /* Copy frame data */
    memcpy(buffer, (void *)((uint8_t *)frame + sizeof(struct ccat_eim_frame_hdr)),
           payload_len);

    /* Clear frame (mark as consumed) */
    ccat_write16((void *)frame, 0);
    __sync_synchronize();

    /* Advance to next RX slot */
    ccat->rx_offset += CCAT_FRAME_SIZE;
    if (ccat->rx_offset + CCAT_FRAME_SIZE > ccat->rx_size)
        ccat->rx_offset = 0;

    return (int)payload_len;
}

/****************************************************************************/

/**
 * Get link state.
 *
 * Reads MII register at offset 0x0C (mii + 0x8 + 4), bit 24.
 */
static int ccat_get_link_state(ec_transport_t *transport)
{
    ec_transport_ccat_t *ccat = transport->priv;

    if (!ccat)
        return 0;

    return !!(ccat_read32((uint8_t *)ccat->mii + MII_LINK_OFFSET)
              & MII_LINK_BIT);
}

/****************************************************************************/

/**
 * Get MAC address.
 */
static int ccat_get_mac(ec_transport_t *transport, uint8_t mac[6])
{
    ec_transport_ccat_t *ccat = transport->priv;

    if (!ccat)
        return -ENODEV;

    memcpy(mac, ccat->mac_addr, 6);
    return 0;
}

/****************************************************************************/

/**
 * Get file descriptor - not applicable for CCAT (polling-based).
 */
static int ccat_get_fd(ec_transport_t *transport)
{
    (void)transport;
    return -1;
}

/****************************************************************************/

/** CCAT EIM transport operations */
const ec_transport_ops_t ec_transport_ccat_ops = {
    .name = "ccat",
    .open = ccat_open,
    .close = ccat_close,
    .get_tx_buffer = ccat_get_tx_buffer,
    .send = ccat_send,
    .receive = ccat_receive,
    .get_link_state = ccat_get_link_state,
    .get_mac = ccat_get_mac,
    .get_fd = ccat_get_fd,
    .set_cpu_affinity = NULL,
};
