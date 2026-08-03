/******************************************************************************
 *
 *  Copyright (C) 2026  Sascha Ittner <sascha.ittner@modusoft.de>
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
 * NIC IRQ affinity pinning implementation.
 *
 * Discovery is layered -- sysfs PCI attributes first, /proc/interrupts
 * last -- rather than scanning /proc/interrupts for every NIC. The
 * ordering is deliberate:
 *
 * - /proc/interrupts shows the action name from request_irq() time. Many
 *   PCI drivers request their MSI-X vectors at probe, BEFORE udev renames
 *   eth0 -> enp2s0, so the table permanently shows "eth0-TxRx-0" while
 *   the configured interface is "enp2s0" -- name-matching finds nothing.
 *   The msi_irqs/ directory hangs off the device object itself, listing
 *   every vector by number, immune to renames.
 *
 * - MSI-X action names are driver-invented ("eth0", "eth0-rx-0",
 *   "eth0-TxRx-1", ...); collecting ALL vectors of an interface from the
 *   table would need per-driver prefix heuristics -- exactly the
 *   knowledge the pin-all-vectors design exists to avoid. msi_irqs/ is
 *   complete by construction, including vectors whose action name
 *   carries no interface hint.
 *
 * - msi_irqs/ and device/irq are documented, machine-oriented sysfs ABI;
 *   /proc/interrupts is a human-oriented table whose layout varies with
 *   architecture and CPU count, and it only lists IRQs already
 *   requested (some drivers request at ndo_open, not probe).
 *
 * Platform devices (SoC NICs) have none of the PCI attributes, so for
 * them the table scan is the only option -- with the OF node name as a
 * rename-proof anchor beside the interface name. Should a platform
 * driver ever surface that requests its IRQs at probe under the
 * pre-rename name, the fix is a third match candidate (the pre-rename
 * name), not scanning /proc/interrupts for PCI devices too.
 */

#include "irq_pin.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <limits.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>

/****************************************************************************/

/**
 * Read a single integer from a sysfs file.
 */
static int read_sysfs_int(const char *path)
{
    char buf[32];
    int fd, n, val;

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }

    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) {
        return -1;
    }
    buf[n] = '\0';

    val = atoi(buf);
    return val;
}

/****************************************************************************/

/**
 * Insert an IRQ number into a set, ascending, ignoring duplicates.
 * Silently drops the IRQ when the set is full (irrelevant for the
 * dedicated NICs this is meant for).
 */
static void irq_set_insert(ec_irq_set_t *set, int irq)
{
    int i, j;

    if (irq <= 0 || set->count >= EC_IRQ_MAX_VECTORS) {
        return;
    }
    for (i = 0; i < set->count && set->irq[i] < irq; i++);
    if (i < set->count && set->irq[i] == irq) {
        return;
    }
    for (j = set->count; j > i; j--) {
        set->irq[j] = set->irq[j - 1];
    }
    set->irq[i] = irq;
    set->count++;
}

/****************************************************************************/

/**
 * Collect all MSI-X/MSI IRQs of a network interface, ascending.
 *
 * Looks in /sys/class/net/$iface/device/msi_irqs/ for IRQ entries.
 * All vectors are collected: which vector serves which queue (or none,
 * like igb's link/misc vector) is driver-specific, and on a dedicated
 * EtherCAT NIC the non-traffic vectors are near-silent anyway.
 */
static int find_msix_irqs(const char *iface, ec_irq_set_t *set)
{
    char path[PATH_MAX];
    DIR *dir;
    struct dirent *ent;

    snprintf(path, sizeof(path), "/sys/class/net/%s/device/msi_irqs", iface);
    dir = opendir(path);
    if (!dir) {
        return -1;
    }

    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        irq_set_insert(set, atoi(ent->d_name));
    }
    closedir(dir);

    return set->count > 0 ? set->count : -1;
}

/****************************************************************************/

int ec_irq_scan_proc_interrupts(const char *path, const char *name_a,
                                const char *name_b, ec_irq_set_t *set)
{
    FILE *f;
    char line[512];
    int before;

    if (!path || !set || (!name_a && !name_b)) {
        return -1;
    }

    f = fopen(path, "r");
    if (!f) {
        return -1;
    }

    before = set->count;
    while (fgets(line, sizeof(line), f)) {
        char *save, *tok, *p;
        int irq;

        /* Only rows introduced by an IRQ number are of interest --
         * "  31:  12345 ...  GICv2 189 Level  fd580000.ethernet".
         * The CPU header line and the IPI/Err/MIS summary rows carry
         * non-numeric labels and fall through here. */
        tok = strtok_r(line, " \t", &save);
        if (!tok) {
            continue;
        }
        for (p = tok; *p >= '0' && *p <= '9'; p++);
        if (p == tok || p[0] != ':' || p[1] != '\0') {
            continue;
        }
        irq = atoi(tok);
        if (irq <= 0) {
            continue;
        }

        /* Match any remaining token against the two names. Full-token
         * comparison only: the per-CPU counters and the hwirq number
         * are numeric and cannot collide, and a partial match ("eth0"
         * inside "eth0-rx-0") must NOT hit -- those are MSI vectors,
         * found via sysfs by the PCI paths. The comma in the separator
         * set splits the action names of shared interrupts. */
        while ((tok = strtok_r(NULL, " \t\n,", &save)) != NULL) {
            if ((name_a && strcmp(tok, name_a) == 0) ||
                (name_b && strcmp(tok, name_b) == 0)) {
                irq_set_insert(set, irq);
                break;
            }
        }
    }
    fclose(f);

    return set->count > before ? set->count - before : -1;
}

/****************************************************************************/

int ec_irq_discover(const char *iface, ec_irq_set_t *set)
{
    char path[PATH_MAX];
    char dev[PATH_MAX];
    const char *dev_name = NULL;
    ssize_t n;
    int irq;

    if (!iface || !set) {
        return -1;
    }

    set->count = 0;

    /* Try MSI-X first (multi-queue NICs) -- collect all vectors */
    if (find_msix_irqs(iface, set) > 0) {
        return set->count;
    }

    /* Fall back to legacy/MSI single IRQ */
    snprintf(path, sizeof(path), "/sys/class/net/%s/device/irq", iface);
    irq = read_sysfs_int(path);
    if (irq > 0) {
        set->irq[0] = irq;
        set->count = 1;
        return set->count;
    }

    /* Platform devices (SoC NICs -- e.g. bcmgenet on the Raspberry Pi 4)
     * are not PCI: there is no msi_irqs/ directory and no device/irq
     * attribute. Their interrupts do appear in /proc/interrupts, filed
     * under the requesting driver's action name -- usually the OF node
     * name the device symlink points at ("fd580000.ethernet"), sometimes
     * the interface name itself. Match either. */
    snprintf(path, sizeof(path), "/sys/class/net/%s/device", iface);
    n = readlink(path, dev, sizeof(dev) - 1);
    if (n > 0) {
        dev[n] = '\0';
        dev_name = strrchr(dev, '/');
        dev_name = dev_name ? dev_name + 1 : dev;
    }
    if (ec_irq_scan_proc_interrupts("/proc/interrupts", dev_name,
                                    iface, set) > 0) {
        return set->count;
    }

    return -1;
}

/****************************************************************************/

/**
 * Pin a single IRQ to a CPU via /proc/irq/$irq/smp_affinity.
 */
static int set_one_affinity(int irq, int cpu)
{
    char path[PATH_MAX];
    char mask[32];
    int fd, n, ret;

    snprintf(path, sizeof(path), "/proc/irq/%d/smp_affinity", irq);
    fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        return -errno;
    }

    /* Write hex CPU mask: bit N set for the target CPU */
    if (cpu < 32) {
        snprintf(mask, sizeof(mask), "%x", 1U << cpu);
    } else {
        /* CPUs 32-63: need two 32-bit words separated by comma */
        snprintf(mask, sizeof(mask), "%x,00000000", 1U << (cpu - 32));
    }

    n = strlen(mask);
    ret = write(fd, mask, n);
    close(fd);

    if (ret != n) {
        return ret < 0 ? -errno : -EIO;
    }

    return 0;
}

/****************************************************************************/

int ec_irq_set_affinity(const ec_irq_set_t *set, int cpu)
{
    int i, pinned = 0, err = -ENODEV;

    if (!set || set->count <= 0 || cpu < 0 || cpu >= 64) {
        return -EINVAL;
    }

    for (i = 0; i < set->count; i++) {
        int ret = set_one_affinity(set->irq[i], cpu);
        if (ret == 0) {
            pinned++;
        } else {
            err = ret;
        }
    }

    return pinned > 0 ? pinned : err;
}

/****************************************************************************/
