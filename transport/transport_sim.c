/*****************************************************************************
 *
 *  transport_sim — in-process simulated EtherCAT bus for unit tests.
 *
 *  See transport_sim.h for the concept. Emulation scope:
 *
 *  - Datagram commands: NOP, APRD/APWR, FPRD/FPWR, BRD (bitwise OR
 *    semantics)/BWR, and the logical commands LRD/LWR/LRW mapped through
 *    the FMMU configurations the master wrote to 0x0600 (working
 *    counters: read match +1, write match +1, LRW write match +2).
 *    DC commands (ARMW/FRMW) are implemented — see "Distributed
 *    clocks" below.
 *  - Auto-increment addressing: the position field is incremented by
 *    every slave the frame passes; the slave seeing position 0 processes.
 *  - Register space per slave, with side effects on:
 *      0x0120 AL control  -> AL status (0x0130) acks the requested state,
 *                            AL status code (0x0134) is cleared
 *      0x0502 SII control -> read op serves 2 words from the EEPROM image
 *                            at 0x0508, then clears the busy/op bits;
 *                            write ops set the error bit (unsupported)
 *  - Base information block (0x0000): 8 FMMUs, 8 sync managers, MII
 *    ports 0/1, optional 32-bit DC support.
 *  - Distributed clocks: a write to 0x0900 latches synthetic port
 *    receive times (chain topology, 100 ns per hop, base advancing
 *    with the frame counter) for the master's transmission delay
 *    measurement; ARMW/FRMW read the addressed slave's register and
 *    write the value to the DC-capable slaves downstream of it (the
 *    system time 0x0910 is a plain stored value, it does not advance).
 *  - Working counters: +1 per matching slave for read or write commands.
 *  - CoE mailbox: requests written into the receive mailbox (SM0
 *    buffer) are answered from a per-slave object dictionary into the
 *    send mailbox (SM1 buffer), raising the SM1 "mailbox full" status
 *    bit (0x080D bit 3) that the master polls via FPRD 0x808; fetching
 *    the send mailbox clears it. Expedited and single-segment normal
 *    SDO up/downloads are supported; segmented transfers are aborted
 *    (0x05040001), unknown objects abort with 0x06020000. The SDO
 *    Information Service (OD list, object and entry descriptions) is
 *    served from the same dictionary in single fragments; object names
 *    are "SimObj%04X", entry descriptions "Entry%02X".
 *
 *  Frames are processed synchronously in send() and queued for the next
 *  receive() call — one cycle of latency, like a real bus.
 *
 *  This file is part of the IgH EtherCAT Master.
 *
 *  The IgH EtherCAT Master is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License version 2, as
 *  published by the Free Software Foundation.
 *
 ****************************************************************************/

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "transport_sim.h"

/****************************************************************************/

#define SIM_ETH_HLEN 14
#define SIM_ECAT_HDR_LEN 2
#define SIM_DGRAM_HDR_LEN 10
#define SIM_RXQ_LEN 32

#define SIM_OD_ENTRIES 32
#define SIM_OD_DATA_SIZE 64

typedef struct {
    int present;
    uint16_t index;
    uint8_t subindex;
    size_t size;
    uint8_t data[SIM_OD_DATA_SIZE];
} sim_od_entry_t;

typedef struct {
    uint8_t regs[SIM_REG_SIZE];
    uint16_t eeprom[SIM_EEPROM_WORDS];
    uint16_t mbox_out_phys; /**< Receive mailbox (master -> slave). */
    uint16_t mbox_out_len;
    uint16_t mbox_in_phys;  /**< Send mailbox (slave -> master). */
    uint16_t mbox_in_len;
    uint16_t sm2_phys;      /**< Output process-data SM (master -> slave). */
    uint16_t sm2_len;
    uint16_t sm3_phys;      /**< Input process-data SM (slave -> master). */
    uint16_t sm3_len;
    uint8_t loopback;       /**< Echo output process data (SM2) back into the
                              input process data (SM3) after each frame, so a
                              HAL output pin round-trips to its input pin. */
    sim_od_entry_t od[SIM_OD_ENTRIES];
    unsigned long od_downloads; /**< Number of SDO download requests. */
    uint8_t eoe_frame[1600]; /**< EoE frame reassembly buffer. */
    size_t eoe_len; /**< Bytes assembled so far. */
    unsigned int sii_lost_scans; /**< Lose the replies to SII accesses during
                                   the first this many bus scans (see
                                   sim_bus_lose_sii). */
} sim_slave_t;

struct sim_bus {
    ec_transport_t transport; /**< Embedded transport instance (test path). */
    int transport_owned;      /**< 1: bus built by sim_open() from a file and
                                owned by the core's transport — freed in
                                sim_close(). 0: created by sim_bus_create() and
                                owned by the test, which calls sim_bus_destroy(). */
    sim_slave_t slaves[SIM_MAX_SLAVES];
    unsigned int nslaves;
    int link_up;
    unsigned long link_polls; /**< get_link calls (see sim_bus_link_polls). */
    unsigned long frame_count;
    unsigned long al_status_brds; /**< BRDs of the AL status register
                                    (see sim_bus_al_status_brd_count). */
    int irq_cpu;              /**< Last set_cpu_affinity CPU, -1 if never. */
    unsigned int scans;       /**< Bus scans begun: broadcast writes of the
                                station address register (0x0010). */
    int drop_frame;           /**< The frame being processed is lost. */
    long irq_pin_frames;      /**< frame_count at that call, -1 if never. */
    pthread_mutex_t lock;

    struct {
        uint8_t data[EC_TRANSPORT_MAX_FRAME_SIZE];
        size_t len;
        uint64_t due_ns; /**< CLOCK_MONOTONIC time it may be received. */
    } rxq[SIM_RXQ_LEN];
    uint64_t reply_delay_ns; /**< See sim_bus_set_reply_delay(). */
    unsigned int rxq_head; /**< Next slot to fill. */
    unsigned int rxq_tail; /**< Next slot to drain. */
};

/****************************************************************************/

/* TRUSTED: clock_gettime(CLOCK_MONOTONIC) is a nonblocking vDSO call. */
static uint64_t sim_now_ns(void) ECRT_RT_ATTR;
ECRT_RT_TRUSTED_BEGIN
static uint64_t sim_now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000000ULL + (uint64_t) ts.tv_nsec;
}
ECRT_RT_TRUSTED_END

static uint16_t sim_rd16(const uint8_t *p)
{
    return (uint16_t) (p[0] | (p[1] << 8));
}

static void sim_wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t) v;
    p[1] = (uint8_t) (v >> 8);
}

static uint32_t sim_rd32(const uint8_t *p)
{
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8)
            | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

/****************************************************************************/

/** Build the SII EEPROM image for one slave.
 *
 * Word layout (ETG.1000.6): 0x0004 alias, 0x0008 vendor id,
 * 0x000A product code, 0x000C revision, 0x000E serial, 0x0018..0x001C
 * mailbox configuration (zero: no mailbox), 0x0040 first category
 * header (0xFFFF: end marker).
 */
static void sim_slave_init_eeprom(sim_slave_t *slave,
        const sim_slave_identity_t *id)
{
    uint16_t *ee = slave->eeprom;

    memset(ee, 0, sizeof(slave->eeprom));

    ee[0x0004] = id->alias;
    ee[0x0008] = (uint16_t) id->vendor_id;
    ee[0x0009] = (uint16_t) (id->vendor_id >> 16);
    ee[0x000A] = (uint16_t) id->product_code;
    ee[0x000B] = (uint16_t) (id->product_code >> 16);
    ee[0x000C] = (uint16_t) id->revision_number;
    ee[0x000D] = (uint16_t) (id->revision_number >> 16);
    ee[0x000E] = (uint16_t) id->serial_number;
    ee[0x000F] = (uint16_t) (id->serial_number >> 16);

    if (id->mbox_out_len) {
        /* Bootstrap and standard mailbox configuration + supported
         * protocols (CoE). */
        ee[0x0014] = id->mbox_out_phys;
        ee[0x0015] = id->mbox_out_len;
        ee[0x0016] = id->mbox_in_phys;
        ee[0x0017] = id->mbox_in_len;
        ee[0x0018] = id->mbox_out_phys;
        ee[0x0019] = id->mbox_out_len;
        ee[0x001A] = id->mbox_in_phys;
        ee[0x001B] = id->mbox_in_len;
        ee[0x001C] = 0x0004; /* EC_MBOX_COE */
        if (id->eoe) {
            ee[0x001C] |= 0x0002; /* EC_MBOX_EOE */
        }
    }

    uint16_t *cat = ee + 0x0040;

    if (id->mbox_out_len) {
        /* General category (type 0x1E, 32 bytes): the CoE details byte
         * (data byte 5) advertises SDO access and PDO assignment /
         * configuration via CoE — required by the master's fsm_pdo
         * before it writes 0x1C1x / mapping objects via SDO. */
        cat[0] = 0x001E; /* category type */
        cat[1] = 16;     /* category size in words */
        memset(cat + 2, 0, 16 * sizeof(uint16_t));
        cat[4] = 0x0F00; /* data byte 5 = 0x0F: enable_sdo |
                            enable_sdo_info | enable_pdo_assign |
                            enable_pdo_configuration */
        cat += 18;
    }

    if (id->mbox_out_len || id->sm2_len || id->sm3_len) {
        /* Sync manager category (type 0x29), 4 SMs x 4 words: physical
         * start, default length, control/status, enable/type.
         * SM0 = mailbox out, SM1 = mailbox in (disabled slots when the
         * slave has no mailbox); SM2 = process data output, SM3 =
         * process data input. */
        cat[0] = 0x0029; /* category type */
        cat[1] = 16;     /* category size in words */

        memset(cat + 2, 0, 16 * sizeof(uint16_t));

        if (id->mbox_out_len) {
            /* SM0: mailbox out (type 1), one-buffer mode */
            cat[2] = id->mbox_out_phys;
            cat[3] = id->mbox_out_len;
            cat[4] = 0x0026; /* control 0x26, status 0 */
            cat[5] = 0x0101; /* enable 1, type 1 */

            /* SM1: mailbox in (type 2), one-buffer mode */
            cat[6] = id->mbox_in_phys;
            cat[7] = id->mbox_in_len;
            cat[8] = 0x0022; /* control 0x22, status 0 */
            cat[9] = 0x0201; /* enable 1, type 2 */
        }

        if (id->sm2_len || id->sm3_len) {
            /* SM2: process data output (type 3), buffered mode */
            cat[10] = id->sm2_phys;
            cat[11] = id->sm2_len;
            cat[12] = 0x0064; /* control 0x64, status 0 */
            cat[13] = 0x0301; /* enable 1, type 3 */

            /* SM3: process data input (type 4), buffered mode */
            cat[14] = id->sm3_phys;
            cat[15] = id->sm3_len;
            cat[16] = 0x0020; /* control 0x20, status 0 */
            cat[17] = 0x0401; /* enable 1, type 4 */
        }

        cat[18] = 0xFFFF; /* end-of-categories marker */
    } else {
        ee[0x0040] = 0xFFFF; /* end-of-categories marker */
    }
}

static void sim_slave_init_regs(sim_slave_t *slave, unsigned int pos,
        unsigned int nslaves, const sim_slave_identity_t *id)
{
    uint8_t *r = slave->regs;

    memset(r, 0, SIM_REG_SIZE);

    /* Base information block. */
    r[0x0000] = 0x11; /* type */
    r[0x0001] = 0x00; /* revision */
    sim_wr16(r + 0x0002, 0x0001); /* build */
    r[0x0004] = 8; /* FMMU count */
    r[0x0005] = 8; /* sync manager count */
    r[0x0007] = 0x0F; /* ports 0+1: MII, ports 2+3: not implemented */
    /* features: bit 2 = DC supported (bit 3 clear: 32-bit range) */
    r[0x0008] = id->dc_supported ? 0x04 : 0x00;

    /* DL status: bit 4+i = link on port i, bit 8+2i = loop closed on
     * port i, bit 9+2i = signal detected on port i. Port 0 is the
     * upstream port (link + signal, loop open); port 1 is downstream
     * (open towards the next slave, loop closed on the last one); ports
     * 2/3 are unimplemented (loop closed). The topology calculation
     * follows every port whose loop is open, so the last slave MUST
     * close port 1 or the recursion runs off the chain. */
    {
        uint16_t dl = 0x0010 | 0x0200 | 0x1000 | 0x4000;
        if (pos + 1 < nslaves) {
            dl |= 0x0020 | 0x0800; /* port 1: link + signal, loop open */
        } else {
            dl |= 0x0400; /* port 1: loop closed */
        }
        sim_wr16(r + 0x0110, dl);
    }

    /* AL status: INIT. */
    sim_wr16(r + 0x0130, 0x0001);
}

/****************************************************************************/

/****************************************************************************/
/* CoE mailbox emulation                                                    */
/****************************************************************************/

static sim_od_entry_t *sim_od_find(sim_slave_t *slave, uint16_t index,
        uint8_t subindex, int create)
{
    unsigned int i;
    sim_od_entry_t *free_entry = NULL;

    for (i = 0; i < SIM_OD_ENTRIES; i++) {
        sim_od_entry_t *e = &slave->od[i];
        if (e->present && e->index == index && e->subindex == subindex) {
            return e;
        }
        if (!e->present && !free_entry) {
            free_entry = e;
        }
    }
    if (create && free_entry) {
        free_entry->present = 1;
        free_entry->index = index;
        free_entry->subindex = subindex;
        free_entry->size = 0;
        return free_entry;
    }
    return NULL;
}

/** Write a mailbox response into the send mailbox and raise the SM1
 * "mailbox full" status bit (0x080D bit 3, polled via FPRD 0x808). */
static void sim_mbox_respond_type(sim_slave_t *slave, uint8_t type,
        const uint8_t *payload, uint16_t len)
{
    uint8_t *m = slave->regs + slave->mbox_in_phys;

    if ((size_t) slave->mbox_in_phys + 6 + len > SIM_REG_SIZE
            || 6 + len > slave->mbox_in_len) {
        return;
    }

    /* The send mailbox holds one message: a slave cannot queue a second
     * response until the master has read the first.  Dropping the new one
     * rather than overwriting is what lets a stale response actually reach
     * the next master, as it does on real hardware. */
    if (slave->regs[0x080D] & 0x08) {
        return;
    }

    sim_wr16(m, len); /* mailbox service data length */
    memcpy(m + 2, slave->regs + 0x0010, 2); /* station address */
    m[4] = 0x00; /* channel & priority */
    m[5] = type;
    memcpy(m + 6, payload, len);

    slave->regs[0x080D] |= 0x08;
}

/** CoE response convenience wrapper. */
static void sim_mbox_respond(sim_slave_t *slave, const uint8_t *payload,
        uint16_t len)
{
    sim_mbox_respond_type(slave, 0x03, payload, len);
}

static void sim_coe_abort(sim_slave_t *slave, uint16_t index,
        uint8_t subindex, uint32_t code)
{
    uint8_t rsp[10];

    sim_wr16(rsp, 0x2 << 12); /* SDO request... */
    rsp[2] = 0x4 << 5; /* ...abort SDO transfer */
    sim_wr16(rsp + 3, index);
    rsp[5] = subindex;
    rsp[6] = (uint8_t) code;
    rsp[7] = (uint8_t) (code >> 8);
    rsp[8] = (uint8_t) (code >> 16);
    rsp[9] = (uint8_t) (code >> 24);
    sim_mbox_respond(slave, rsp, sizeof(rsp));
}

/** Map an object-dictionary entry size to a CoE data type. */
static uint16_t sim_od_data_type(size_t size)
{
    switch (size) {
        case 1: return 0x0005; /* USINT */
        case 2: return 0x0006; /* UINT */
        case 4: return 0x0007; /* UDINT */
        default: return 0x0009; /* VISIBLE_STRING */
    }
}

static void sim_sdo_info_error(sim_slave_t *slave, uint32_t code)
{
    uint8_t rsp[10];

    sim_wr16(rsp, 0x8 << 12); /* SDO information */
    rsp[2] = 0x07; /* error response */
    rsp[3] = 0;
    sim_wr16(rsp + 4, 0); /* fragments left */
    rsp[6] = (uint8_t) code;
    rsp[7] = (uint8_t) (code >> 8);
    rsp[8] = (uint8_t) (code >> 16);
    rsp[9] = (uint8_t) (code >> 24);
    sim_mbox_respond(slave, rsp, sizeof(rsp));
}

/** Handle a CoE SDO Information Service request (opcode in the low 7
 * bits of the third byte): OD list, object description and entry
 * description, all served from the object dictionary in one fragment. */
static void sim_coe_sdo_info(sim_slave_t *slave, const uint8_t *coe,
        uint16_t len)
{
    uint8_t opcode = coe[2] & 0x7F;
    uint8_t rsp[6 + 10 + SIM_OD_DATA_SIZE];
    unsigned int i, j, n;
    uint16_t index;

    if (len < 8) {
        return;
    }

    switch (opcode) {
        case 0x01: /* Get OD List request */
            {
                uint16_t list_type = sim_rd16(coe + 6);

                sim_wr16(rsp, 0x8 << 12);
                rsp[2] = 0x02; /* Get OD List response */
                rsp[3] = 0;
                sim_wr16(rsp + 4, 0); /* fragments left */
                sim_wr16(rsp + 6, list_type);
                n = 0;
                for (i = 0; i < SIM_OD_ENTRIES; i++) {
                    if (!slave->od[i].present) {
                        continue;
                    }
                    for (j = 0; j < i; j++) { /* only once per index */
                        if (slave->od[j].present
                                && slave->od[j].index == slave->od[i].index) {
                            break;
                        }
                    }
                    if (j == i) {
                        sim_wr16(rsp + 8 + 2 * n, slave->od[i].index);
                        n++;
                    }
                }
                sim_mbox_respond(slave, rsp, (uint16_t) (8 + 2 * n));
            }
            break;
        case 0x03: /* Get object description request */
            {
                const sim_od_entry_t *first = NULL;
                uint8_t max_sub = 0;

                index = sim_rd16(coe + 6);
                for (i = 0; i < SIM_OD_ENTRIES; i++) {
                    const sim_od_entry_t *e = &slave->od[i];
                    if (e->present && e->index == index) {
                        if (!first) {
                            first = e;
                        }
                        if (e->subindex > max_sub) {
                            max_sub = e->subindex;
                        }
                    }
                }
                if (!first) {
                    sim_sdo_info_error(slave, 0x06020000);
                    return;
                }
                sim_wr16(rsp, 0x8 << 12);
                rsp[2] = 0x04; /* object description response */
                rsp[3] = 0;
                sim_wr16(rsp + 4, 0);
                sim_wr16(rsp + 6, index);
                sim_wr16(rsp + 8, sim_od_data_type(first->size));
                rsp[10] = max_sub;
                rsp[11] = max_sub ? 0x09 : 0x07; /* ARRAY / VAR */
                n = (unsigned int) snprintf((char *) rsp + 12,
                        sizeof(rsp) - 12, "SimObj%04X", index);
                sim_mbox_respond(slave, rsp, (uint16_t) (12 + n));
            }
            break;
        case 0x05: /* Get entry description request */
            {
                const sim_od_entry_t *entry;
                uint8_t subindex;

                if (len < 10) { /* needs index + subindex + value info */
                    sim_sdo_info_error(slave, 0x05040001);
                    return;
                }
                subindex = coe[8];
                index = sim_rd16(coe + 6);
                entry = sim_od_find(slave, index, subindex, 0);
                if (!entry) {
                    sim_sdo_info_error(slave, 0x06090011);
                    return;
                }
                sim_wr16(rsp, 0x8 << 12);
                rsp[2] = 0x06; /* entry description response */
                rsp[3] = 0;
                sim_wr16(rsp + 4, 0);
                sim_wr16(rsp + 6, index);
                rsp[8] = subindex;
                rsp[9] = coe[9]; /* value info echo */
                sim_wr16(rsp + 10, sim_od_data_type(entry->size));
                sim_wr16(rsp + 12, (uint16_t) (entry->size * 8));
                sim_wr16(rsp + 14, 0x003F); /* r/w in all states */
                n = (unsigned int) snprintf((char *) rsp + 16,
                        sizeof(rsp) - 16, "Entry%02X", subindex);
                sim_mbox_respond(slave, rsp, (uint16_t) (16 + n));
            }
            break;
        default:
            sim_sdo_info_error(slave, 0x05040001);
            break;
    }
}

/** Handle a CoE SDO request (expedited and single-segment normal
 * transfers; segmented transfers are aborted). */
static void sim_coe_request(sim_slave_t *slave, const uint8_t *coe,
        uint16_t len)
{
    uint8_t cs, ccs, subindex;
    uint16_t index;
    sim_od_entry_t *entry;
    uint8_t rsp[10 + SIM_OD_DATA_SIZE];

    if (len >= 8 && (sim_rd16(coe) >> 12) == 0x8) {
        sim_coe_sdo_info(slave, coe, len);
        return;
    }

    if (len < 6 || (sim_rd16(coe) >> 12) != 0x2) {
        return; /* not an SDO request: ignore */
    }

    cs = coe[2];
    ccs = cs >> 5;
    index = sim_rd16(coe + 3);
    subindex = (cs & 0x10) ? 0 : coe[5]; /* complete access: subindex 0 */

    switch (ccs) {
        case 0x1: /* initiate download */
            {
                size_t dsize;
                const uint8_t *src;

                slave->od_downloads++;

                if (cs & 0x02) { /* expedited */
                    dsize = (cs & 0x01) ? 4 - ((cs >> 2) & 0x03) : 4;
                    src = coe + 6;
                    if (len < 6 + dsize) { /* short request: ignore */
                        return;
                    }
                } else { /* normal, single segment only */
                    if (len < 10) {
                        return;
                    }
                    dsize = sim_rd32(coe + 6);
                    src = coe + 10;
                    if (dsize > (size_t) (len - 10)) {
                        /* would need segmented transfer */
                        sim_coe_abort(slave, index, subindex, 0x05040001);
                        return;
                    }
                }
                if (dsize > SIM_OD_DATA_SIZE
                        || !(entry = sim_od_find(slave, index, subindex, 1))) {
                    sim_coe_abort(slave, index, subindex, 0x06040047);
                    return;
                }
                memcpy(entry->data, src, dsize);
                entry->size = dsize;

                sim_wr16(rsp, 0x3 << 12); /* SDO response */
                rsp[2] = 0x3 << 5; /* download response */
                sim_wr16(rsp + 3, index);
                rsp[5] = coe[5];
                memset(rsp + 6, 0, 4);
                sim_mbox_respond(slave, rsp, 10);
            }
            break;
        case 0x2: /* initiate upload */
            entry = sim_od_find(slave, index, subindex, 0);
            if (!entry) {
                sim_coe_abort(slave, index, subindex,
                        0x06020000 /* object does not exist */);
                return;
            }
            sim_wr16(rsp, 0x3 << 12); /* SDO response */
            sim_wr16(rsp + 3, index);
            rsp[5] = coe[5];
            if (entry->size <= 4) { /* expedited */
                rsp[2] = (uint8_t) ((0x2 << 5) | 0x02 | 0x01
                        | ((4 - entry->size) << 2));
                memset(rsp + 6, 0, 4);
                memcpy(rsp + 6, entry->data, entry->size);
                sim_mbox_respond(slave, rsp, 10);
            } else { /* normal, single segment */
                rsp[2] = (0x2 << 5) | 0x01;
                rsp[6] = (uint8_t) entry->size;
                rsp[7] = (uint8_t) (entry->size >> 8);
                rsp[8] = 0;
                rsp[9] = 0;
                memcpy(rsp + 10, entry->data, entry->size);
                sim_mbox_respond(slave, rsp, (uint16_t) (10 + entry->size));
            }
            break;
        default: /* segment transfers etc. are not supported */
            sim_coe_abort(slave, index, subindex, 0x05040001);
            break;
    }
}

/** Handle an EoE fragment (mailbox type 2): reassemble the Ethernet
 * frame and, on the last fragment, echo it back to the master as a
 * single slave-to-master EoE fragment — as if the slave's network
 * stack looped the frame. Wire format (ethernet.c): byte 0 frame type
 * (0 = fragment request), byte 1 bit 0 = last fragment, u16 at 2 =
 * fragment number (0-5) | offset/complete-size in 32-byte blocks
 * (6-11) | frame number (12-15). */
static void sim_eoe_request(sim_slave_t *slave, const uint8_t *data,
        uint16_t len)
{
    uint8_t frame_type, last;
    uint16_t word2, frag, off32, frame_no;
    size_t payload_len, pos;

    if (len < 4) {
        return;
    }
    frame_type = data[0] & 0x0F;
    last = data[1] & 0x01;
    word2 = sim_rd16(data + 2);
    frag = word2 & 0x3F;
    off32 = (word2 >> 6) & 0x3F;
    frame_no = (word2 >> 12) & 0x0F;
    payload_len = len - 4;

    if (frame_type != 0x00) {
        return; /* only fragment requests are emulated */
    }

    pos = frag ? (size_t) off32 * 32 : 0; /* fragment 0: field = size */
    if (frag == 0) {
        slave->eoe_len = 0;
    }
    if (pos + payload_len > sizeof(slave->eoe_frame)) {
        return;
    }
    memcpy(slave->eoe_frame + pos, data + 4, payload_len);
    slave->eoe_len = pos + payload_len;

    if (last) {
        /* Echo the complete frame back (single fragment). */
        uint8_t rsp[4 + sizeof(slave->eoe_frame)];
        uint16_t size_blocks = (uint16_t) (slave->eoe_len / 32 + 1);

        if (4 + slave->eoe_len + 6 > slave->mbox_in_len) {
            /* Slave->master fragmentation is not emulated; be loud so
             * a future MTU-sized test does not just time out. */
            fprintf(stderr, "sim: EoE echo of %zu bytes needs"
                    " fragmentation — frame dropped\n", slave->eoe_len);
            return;
        }
        rsp[0] = 0x00; /* fragment request */
        rsp[1] = 0x01; /* last fragment */
        sim_wr16(rsp + 2, (uint16_t) ((0 & 0x3F) | ((size_blocks & 0x3F) << 6)
                    | ((frame_no & 0x0F) << 12)));
        memcpy(rsp + 4, slave->eoe_frame, slave->eoe_len);
        sim_mbox_respond_type(slave, 0x02, rsp,
                (uint16_t) (4 + slave->eoe_len));
        slave->eoe_len = 0;
    }
}

/** Process a request written into the receive mailbox. */
static void sim_mbox_request(sim_slave_t *slave)
{
    const uint8_t *m = slave->regs + slave->mbox_out_phys;
    uint16_t dlen = sim_rd16(m);
    uint8_t type = m[5] & 0x0F;

    if (dlen < 2 || 6 + dlen > slave->mbox_out_len) {
        return;
    }

    if (type == 0x03) { /* CoE */
        sim_coe_request(slave, m + 6, dlen);
    } else if (type == 0x02) { /* EoE */
        sim_eoe_request(slave, m + 6, dlen);
    } else {
        /* Unsupported protocol: mailbox error reply (type 0). */
        uint8_t *r = slave->regs + slave->mbox_in_phys;
        sim_wr16(r, 4);
        memcpy(r + 2, slave->regs + 0x0010, 2);
        r[4] = 0x00;
        r[5] = 0x00; /* mailbox error */
        sim_wr16(r + 6, 0x0001); /* MBXERR command */
        sim_wr16(r + 8, 0x0001); /* code: syntax error */
        slave->regs[0x080D] |= 0x08;
    }
}

/****************************************************************************/

/** Apply write side effects after data has been copied into the register
 * space. \a adr / \a len describe the written range. */
static void sim_slave_write_effects(sim_slave_t *slave, uint16_t adr,
        uint16_t len)
{
    uint8_t *r = slave->regs;

    /* AL control (0x0120): acknowledge the requested state. */
    if (adr <= 0x0120 && 0x0120 < adr + len) {
        uint16_t ctl = sim_rd16(r + 0x0120);
        sim_wr16(r + 0x0130, ctl & 0x000F);
        sim_wr16(r + 0x0134, 0x0000); /* AL status code: no error */
    }

    /* SII control (0x0502): execute the requested EEPROM operation. */
    if (adr <= 0x0502 && 0x0502 < adr + len) {
        uint8_t op = r[0x0503] & 0x03;
        uint16_t waddr = sim_rd16(r + 0x0504);

        if (op & 0x01) { /* read: serve two words */
            unsigned int i;
            for (i = 0; i < 2; i++) {
                uint16_t w = (waddr + i < SIM_EEPROM_WORDS)
                        ? slave->eeprom[waddr + i] : 0xFFFF;
                sim_wr16(r + 0x0508 + 2 * i, w);
            }
            r[0x0502] &= (uint8_t) ~0x03;
            r[0x0503] = 0x00; /* idle: no busy, no error */
        } else if (op) { /* write/reload: not supported by the sim */
            r[0x0503] = 0x20; /* error on last command */
        }
    }

    /* Receive mailbox (SM0 buffer): process the request. */
    if (slave->mbox_out_len && adr <= slave->mbox_out_phys
            && slave->mbox_out_phys < adr + len) {
        sim_mbox_request(slave);
    }
}

/** Process one datagram against one slave. Returns the working counter
 * increment. */
static unsigned int sim_slave_process(sim_slave_t *slave, uint8_t cmd,
        uint16_t adr, uint8_t *data, uint16_t len)
{
    if ((size_t) adr + len > SIM_REG_SIZE) {
        return 0;
    }

    switch (cmd) {
        case 0x01: /* APRD */
        case 0x04: /* FPRD */
            memcpy(data, slave->regs + adr, len);
            /* Fetching the send mailbox empties it. */
            if (slave->mbox_in_len && adr == slave->mbox_in_phys) {
                slave->regs[0x080D] &= (uint8_t) ~0x08;
            }
            return 1;
        case 0x07: /* BRD: bitwise OR of all slaves' data */
            {
                uint16_t i;
                for (i = 0; i < len; i++) {
                    data[i] |= slave->regs[adr + i];
                }
            }
            return 1;
        case 0x02: /* APWR */
        case 0x05: /* FPWR */
        case 0x08: /* BWR */
            memcpy(slave->regs + adr, data, len);
            sim_slave_write_effects(slave, adr, len);
            return 1;
        default:
            return 0;
    }
}

/** Process a logical (FMMU-mapped) command against one slave.
 *
 * Walks the FMMU configuration pages the master wrote to 0x0600
 * (16 bytes each: logical start u32, size u16, start/end bit, physical
 * start u16, physical start bit, direction, enable) and copies every
 * overlap between the datagram's logical range and an enabled FMMU's
 * window. Returns the working counter increment. */
static unsigned int sim_slave_process_logical(sim_slave_t *slave,
        uint8_t cmd, uint32_t laddr, uint8_t *data, uint16_t len)
{
    unsigned int i, r = 0, w = 0;

    for (i = 0; i < 8; i++) {
        const uint8_t *fmmu = slave->regs + 0x0600 + 16 * i;
        uint32_t lstart = sim_rd32(fmmu);
        uint16_t size = sim_rd16(fmmu + 4);
        uint16_t phys = sim_rd16(fmmu + 8);
        uint8_t dir = fmmu[11];
        uint32_t ov_start, ov_end;

        if (!(sim_rd16(fmmu + 12) & 0x0001) || !size) {
            continue; /* FMMU not enabled */
        }

        ov_start = lstart > laddr ? lstart : laddr;
        ov_end = (lstart + size) < (laddr + len)
                ? (lstart + size) : (laddr + len);
        if (ov_start >= ov_end
                || (size_t) phys + (ov_end - lstart) > SIM_REG_SIZE) {
            continue;
        }

        if ((dir & 0x01) && (cmd == 0x0A || cmd == 0x0C)) {
            /* Input FMMU on LRD/LRW: slave memory -> frame. */
            memcpy(data + (ov_start - laddr),
                    slave->regs + phys + (ov_start - lstart),
                    ov_end - ov_start);
            r = 1;
        }
        if ((dir & 0x02) && (cmd == 0x0B || cmd == 0x0C)) {
            /* Output FMMU on LWR/LRW: frame -> slave memory. */
            memcpy(slave->regs + phys + (ov_start - lstart),
                    data + (ov_start - laddr),
                    ov_end - ov_start);
            w = 1;
        }
    }

    /* Working counter: reads +1, writes +1 (LWR) or +2 (LRW). */
    return r + (cmd == 0x0C ? 2 * w : w);
}

/** Latch synthetic DC port receive times on all slaves (triggered by a
 * write to 0x0900): chain topology, 100 ns per hop, base advancing with
 * the frame counter so consecutive measurements stay monotonic. */
static void sim_bus_latch_dc_times(sim_bus_t *bus)
{
    uint32_t base = (uint32_t) bus->frame_count * 1000u;
    unsigned int i;

    for (i = 0; i < bus->nslaves; i++) {
        uint8_t *r = bus->slaves[i].regs;

        sim_wr16(r + 0x0900, (uint16_t) (base + i * 100));
        sim_wr16(r + 0x0902, (uint16_t) ((base + i * 100) >> 16));
        if (i + 1 < bus->nslaves) { /* port 1: return pass of the frame */
            uint32_t t1 = base + (2 * (bus->nslaves - 1) - i) * 100;
            sim_wr16(r + 0x0904, (uint16_t) t1);
            sim_wr16(r + 0x0906, (uint16_t) (t1 >> 16));
        } else {
            sim_wr16(r + 0x0904, 0);
            sim_wr16(r + 0x0906, 0);
        }
        memset(r + 0x0908, 0, 8); /* ports 2 + 3: not connected */
    }
}

/** Process one datagram against the whole bus (in place). */
static void sim_bus_process_datagram(sim_bus_t *bus, uint8_t cmd,
        uint8_t *addr, uint8_t *data, uint16_t len, uint8_t *wkc_field)
{
    uint16_t wkc = sim_rd16(wkc_field);
    uint16_t offset = sim_rd16(addr + 2);
    unsigned int i;

    switch (cmd) {
        case 0x01: /* APRD */
        case 0x02: /* APWR */
            {
                /* Position addressing: the slave seeing position 0
                 * processes; every slave increments the field. */
                int16_t pos = (int16_t) sim_rd16(addr);
                for (i = 0; i < bus->nslaves; i++) {
                    if (pos == 0) {
                        wkc = (uint16_t) (wkc + sim_slave_process(
                                &bus->slaves[i], cmd, offset, data, len));
                    }
                    pos++;
                }
                sim_wr16(addr, (uint16_t) pos);
            }
            break;
        case 0x04: /* FPRD */
        case 0x05: /* FPWR */
            {
                uint16_t station = sim_rd16(addr);
                for (i = 0; i < bus->nslaves; i++) {
                    if (sim_rd16(bus->slaves[i].regs + 0x0010) == station) {
                        wkc = (uint16_t) (wkc + sim_slave_process(
                                &bus->slaves[i], cmd, offset, data, len));
                        if (offset >= 0x0500 && offset < 0x0510
                                && bus->scans <= bus->slaves[i].sii_lost_scans) {
                            bus->drop_frame = 1;
                        }
                    }
                }
            }
            break;
        case 0x07: /* BRD */
        case 0x08: /* BWR */
            {
                int16_t pos = (int16_t) sim_rd16(addr);
                if (cmd == 0x07 && offset == 0x0130) {
                    bus->al_status_brds++;
                }
                if (cmd == 0x08 && offset == 0x0010) {
                    bus->scans++; /* a scan begins: station addresses cleared */
                }
                for (i = 0; i < bus->nslaves; i++) {
                    wkc = (uint16_t) (wkc + sim_slave_process(
                            &bus->slaves[i], cmd, offset, data, len));
                    pos++;
                }
                sim_wr16(addr, (uint16_t) pos);
            }
            break;
        case 0x0A: /* LRD */
        case 0x0B: /* LWR */
        case 0x0C: /* LRW */
            {
                uint32_t laddr = sim_rd32(addr);
                for (i = 0; i < bus->nslaves; i++) {
                    wkc = (uint16_t) (wkc + sim_slave_process_logical(
                            &bus->slaves[i], cmd, laddr, data, len));
                }
            }
            break;
        case 0x0D: /* ARMW */
        case 0x0E: /* FRMW */
            {
                /* Read Multiple Write: the addressed slave provides the
                 * data; slaves the frame passes AFTERWARDS take the
                 * write. Used for DC drift compensation (0x0910). */
                int16_t pos = (int16_t) sim_rd16(addr);
                uint16_t station = sim_rd16(addr);
                int have_data = 0;

                for (i = 0; i < bus->nslaves; i++) {
                    sim_slave_t *slave = &bus->slaves[i];
                    int addressed = (cmd == 0x0D)
                            ? (pos == 0)
                            : (sim_rd16(slave->regs + 0x0010) == station);

                    if ((size_t) offset + len <= SIM_REG_SIZE) {
                        if (addressed) {
                            memcpy(data, slave->regs + offset, len);
                            have_data = 1;
                            wkc++;
                        } else if (have_data) {
                            memcpy(slave->regs + offset, data, len);
                            wkc++;
                        }
                    }
                    pos++;
                }
                if (cmd == 0x0D) {
                    sim_wr16(addr, (uint16_t) pos);
                }
            }
            break;
        default:
            /* NOP: no slave processes. */
            break;
    }

    /* A write covering 0x0900 latches the DC port receive times. */
    if ((cmd == 0x02 || cmd == 0x05 || cmd == 0x08)
            && offset <= 0x0900 && 0x0900 < (uint32_t) offset + len) {
        sim_bus_latch_dc_times(bus);
    }

    sim_wr16(wkc_field, wkc);
}

/** Process a full frame in place. Returns 0 if the frame was valid. */
static int sim_bus_process_frame(sim_bus_t *bus, uint8_t *frame, size_t size)
{
    size_t pos = SIM_ETH_HLEN;
    uint16_t ecat_hdr, ecat_len;
    int more;

    if (size < SIM_ETH_HLEN + SIM_ECAT_HDR_LEN) {
        return -EINVAL;
    }
    if (frame[12] != 0x88 || frame[13] != 0xA4) {
        return -EINVAL; /* not an EtherCAT frame */
    }

    ecat_hdr = sim_rd16(frame + pos);
    ecat_len = ecat_hdr & 0x07FF;
    if ((ecat_hdr >> 12) != 0x1) {
        return -EINVAL; /* not a datagram-type frame */
    }
    pos += SIM_ECAT_HDR_LEN;
    if (pos + ecat_len > size) {
        return -EINVAL;
    }

    do {
        uint8_t cmd, *addr, *data;
        uint16_t lenflags, dlen;

        if (pos + SIM_DGRAM_HDR_LEN + 2 > size) {
            return -EINVAL;
        }
        cmd = frame[pos];
        addr = frame + pos + 2;
        lenflags = sim_rd16(frame + pos + 6);
        dlen = lenflags & 0x07FF;
        more = lenflags & 0x8000;
        data = frame + pos + SIM_DGRAM_HDR_LEN;

        if (pos + SIM_DGRAM_HDR_LEN + dlen + 2 > size) {
            return -EINVAL;
        }

        sim_bus_process_datagram(bus, cmd, addr, data, dlen,
                data + dlen /* working counter */);

        pos += SIM_DGRAM_HDR_LEN + dlen + 2;
    } while (more);

    return 0;
}

/****************************************************************************/
/* Transport operations                                                     */
/****************************************************************************/

/* Bus-description file parser (registry path).
 *
 * A small line-based format naming the slaves to emulate. Keys apply to
 * the slave opened by the most recent "slave <pos>" line; numbers accept
 * decimal, 0x hex and 0-prefixed octal. Example:
 *
 *   # one CoE slave, 4 bytes out / 4 bytes in
 *   slave 0
 *     vendor   0x00000E17
 *     product  0x5134000A
 *     revision 1
 *     serial   8001
 *     mbox_out 0x1000 128
 *     mbox_in  0x1080 128
 *     sm2      0x1100 4
 *     sm3      0x1400 4
 *     dc       1
 *     od       0x2000 0 4 0xDEADBEEF   # index sub bytelen value (LE)
 */

#define SIM_MAX_OD_PRESETS 64

typedef struct {
    unsigned int pos;
    uint16_t index;
    uint8_t subindex;
    size_t size;
    uint8_t data[SIM_OD_DATA_SIZE];
} sim_od_preset_t;

static sim_bus_t *sim_bus_create_from_file(const char *path)
{
    FILE *f;
    char line[256];
    sim_slave_identity_t ids[SIM_MAX_SLAVES];
    sim_od_preset_t presets[SIM_MAX_OD_PRESETS];
    unsigned int nslaves = 0, npresets = 0;
    int cur = -1; /* current slave, -1 until the first "slave" line */
    sim_bus_t *bus;
    unsigned int i;

    f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "sim: cannot open bus-description file '%s'\n", path);
        return NULL;
    }

    memset(ids, 0, sizeof(ids));

    while (fgets(line, sizeof(line), f)) {
        char key[32];
        int off = 0;
        char *args;
        char *hash = strchr(line, '#');
        sim_slave_identity_t *id;
        int a, b;

        if (hash) {
            *hash = '\0'; /* strip trailing comment */
        }
        if (sscanf(line, "%31s%n", key, &off) < 1) {
            continue; /* blank line */
        }
        args = line + off;

        if (strcmp(key, "slave") == 0) {
            unsigned long pos = strtoul(args, NULL, 0);
            if (pos >= SIM_MAX_SLAVES) {
                fprintf(stderr, "sim: slave position %lu >= %d\n", pos,
                        SIM_MAX_SLAVES);
                goto err;
            }
            cur = (int) pos;
            if ((unsigned int) cur + 1 > nslaves) {
                nslaves = (unsigned int) cur + 1;
            }
            continue;
        }

        if (cur < 0) {
            fprintf(stderr, "sim: '%s' before any 'slave' line\n", key);
            goto err;
        }
        id = &ids[cur];

        if (strcmp(key, "vendor") == 0) {
            id->vendor_id = (uint32_t) strtoul(args, NULL, 0);
        } else if (strcmp(key, "product") == 0) {
            id->product_code = (uint32_t) strtoul(args, NULL, 0);
        } else if (strcmp(key, "revision") == 0) {
            id->revision_number = (uint32_t) strtoul(args, NULL, 0);
        } else if (strcmp(key, "serial") == 0) {
            id->serial_number = (uint32_t) strtoul(args, NULL, 0);
        } else if (strcmp(key, "alias") == 0) {
            id->alias = (uint16_t) strtoul(args, NULL, 0);
        } else if (strcmp(key, "dc") == 0) {
            id->dc_supported = (uint8_t) strtoul(args, NULL, 0);
        } else if (strcmp(key, "eoe") == 0) {
            id->eoe = (uint8_t) strtoul(args, NULL, 0);
        } else if (strcmp(key, "loopback") == 0) {
            id->loopback = (uint8_t) strtoul(args, NULL, 0);
        } else if (strcmp(key, "mbox_out") == 0) {
            if (sscanf(args, "%i %i", &a, &b) != 2) goto badline;
            id->mbox_out_phys = (uint16_t) a;
            id->mbox_out_len = (uint16_t) b;
        } else if (strcmp(key, "mbox_in") == 0) {
            if (sscanf(args, "%i %i", &a, &b) != 2) goto badline;
            id->mbox_in_phys = (uint16_t) a;
            id->mbox_in_len = (uint16_t) b;
        } else if (strcmp(key, "sm2") == 0) {
            if (sscanf(args, "%i %i", &a, &b) != 2) goto badline;
            id->sm2_phys = (uint16_t) a;
            id->sm2_len = (uint16_t) b;
        } else if (strcmp(key, "sm3") == 0) {
            if (sscanf(args, "%i %i", &a, &b) != 2) goto badline;
            id->sm3_phys = (uint16_t) a;
            id->sm3_len = (uint16_t) b;
        } else if (strcmp(key, "od") == 0) {
            int idx, sub, sz, k;
            unsigned long val;
            sim_od_preset_t *pr;
            if (sscanf(args, "%i %i %i %li", &idx, &sub, &sz, &val) != 4) {
                goto badline;
            }
            if (sz < 0 || (size_t) sz > SIM_OD_DATA_SIZE) {
                fprintf(stderr, "sim: od bytelen %d out of range\n", sz);
                goto err;
            }
            if (npresets >= SIM_MAX_OD_PRESETS) {
                fprintf(stderr, "sim: too many od presets (max %d)\n",
                        SIM_MAX_OD_PRESETS);
                goto err;
            }
            pr = &presets[npresets++];
            pr->pos = (unsigned int) cur;
            pr->index = (uint16_t) idx;
            pr->subindex = (uint8_t) sub;
            pr->size = (size_t) sz;
            memset(pr->data, 0, sizeof(pr->data));
            for (k = 0; k < sz && (size_t) k < sizeof(val); k++) {
                pr->data[k] = (uint8_t) (val >> (8 * k)); /* little-endian */
            }
        } else {
            fprintf(stderr, "sim: unknown key '%s' in '%s'\n", key, path);
            goto err;
        }
        continue;

badline:
        fprintf(stderr, "sim: malformed '%s' line in '%s'\n", key, path);
        goto err;
    }

    fclose(f);
    f = NULL;

    if (nslaves == 0) {
        fprintf(stderr, "sim: no slaves defined in '%s'\n", path);
        return NULL;
    }

    bus = sim_bus_create(nslaves, ids);
    if (!bus) {
        return NULL;
    }

    for (i = 0; i < npresets; i++) {
        if (sim_bus_od_set(bus, presets[i].pos, presets[i].index,
                    presets[i].subindex, presets[i].data,
                    presets[i].size) != 0) {
            fprintf(stderr, "sim: failed to preset OD 0x%04X:%u on slave %u"
                    " (needs a mailbox/CoE slave)\n", presets[i].index,
                    presets[i].subindex, presets[i].pos);
            sim_bus_destroy(bus);
            return NULL;
        }
    }

    return bus;

err:
    if (f) {
        fclose(f);
    }
    return NULL;
}

static int sim_open(ec_transport_t *transport, const char *interface)
{
    sim_bus_t *bus;

    /* Test path: sim_bus_create() already built the bus and injected it
     * (transport->priv set); ecrt_startup_master() still calls open(). */
    if (transport->priv) {
        return 0;
    }

    /* Registry path (transportType=sim, interface=<file>): build the
     * emulated bus from its description file. */
    if (!interface || !interface[0]) {
        fprintf(stderr, "sim: no bus-description file"
                " (set interface=<path>)\n");
        return -EINVAL;
    }

    bus = sim_bus_create_from_file(interface);
    if (!bus) {
        return -EINVAL;
    }

    bus->transport_owned = 1;
    transport->priv = bus;
    return 0;
}

static void sim_close(ec_transport_t *transport)
{
    sim_bus_t *bus = transport->priv;

    if (bus && bus->transport_owned) {
        transport->priv = NULL;
        sim_bus_destroy(bus);
    }
}

static uint8_t *sim_get_tx_buffer(ec_transport_t *transport) ECRT_RT_ATTR;
static uint8_t *sim_get_tx_buffer(ec_transport_t *transport)
{
    return transport->tx_buffer;
}

/* TRUSTED: the sim transport is an in-process test / dry-run bus, not a
 * hard-realtime production path (selectable only via transportType=sim).
 * Its cyclic send()/receive() take a bounded, allocation-free mutex
 * around a frame memcpy; the function-effects analysis cannot see that
 * the critical section neither allocates nor blocks unboundedly. */
static int sim_send(ec_transport_t *transport, size_t size) ECRT_RT_ATTR;
ECRT_RT_TRUSTED_BEGIN
static int sim_send(ec_transport_t *transport, size_t size)
{
    sim_bus_t *bus = transport->priv;

    if (size > sizeof(transport->tx_buffer)) {
        return -EINVAL;
    }

    pthread_mutex_lock(&bus->lock);

    if (!bus->link_up) { /* link down: frame is lost */
        pthread_mutex_unlock(&bus->lock);
        return 0;
    }

    bus->frame_count++;
    bus->drop_frame = 0;

    if (sim_bus_process_frame(bus, transport->tx_buffer, size) == 0
            && !bus->drop_frame) {
        unsigned int s;
        /* Loopback slaves: echo the output process data the master just
         * wrote (SM2) into the input process data (SM3), so it reappears on
         * the next receive() — a one-frame round-trip. Geometry is bounded
         * by the sim_bus_create() SIM_REG_SIZE check. */
        for (s = 0; s < bus->nslaves; s++) {
            sim_slave_t *sl = &bus->slaves[s];
            if (sl->loopback && sl->sm2_len && sl->sm3_len) {
                size_t n = sl->sm2_len < sl->sm3_len ? sl->sm2_len : sl->sm3_len;
                memcpy(sl->regs + sl->sm3_phys, sl->regs + sl->sm2_phys, n);
            }
        }
        /* Queue the processed frame for the next receive() (drop the
         * oldest frame on overflow, like a full RX ring would). */
        unsigned int next = (bus->rxq_head + 1) % SIM_RXQ_LEN;
        if (next == bus->rxq_tail) {
            bus->rxq_tail = (bus->rxq_tail + 1) % SIM_RXQ_LEN;
        }
        memcpy(bus->rxq[bus->rxq_head].data, transport->tx_buffer, size);
        bus->rxq[bus->rxq_head].len = size;
        bus->rxq[bus->rxq_head].due_ns = sim_now_ns() + bus->reply_delay_ns;
        bus->rxq_head = next;
    }

    pthread_mutex_unlock(&bus->lock);
    return 0;
}
ECRT_RT_TRUSTED_END

static int sim_receive(ec_transport_t *transport, uint8_t *buffer,
        size_t max_size) ECRT_RT_ATTR;
ECRT_RT_TRUSTED_BEGIN
static int sim_receive(ec_transport_t *transport, uint8_t *buffer,
        size_t max_size)
{
    sim_bus_t *bus = transport->priv;
    size_t len = 0;

    pthread_mutex_lock(&bus->lock);
    if (bus->rxq_tail != bus->rxq_head
            && (!bus->reply_delay_ns
                || sim_now_ns() >= bus->rxq[bus->rxq_tail].due_ns)) {
        len = bus->rxq[bus->rxq_tail].len;
        if (len <= max_size) {
            memcpy(buffer, bus->rxq[bus->rxq_tail].data, len);
        } else {
            len = 0; /* drop oversized frame */
        }
        bus->rxq_tail = (bus->rxq_tail + 1) % SIM_RXQ_LEN;
    }
    pthread_mutex_unlock(&bus->lock);

    return (int) len;
}
ECRT_RT_TRUSTED_END

static int sim_get_link_state(ec_transport_t *transport)
{
    sim_bus_t *bus = transport->priv;
    int up;

    /* For a file-driven (registry) bus, a companion "<interface>.link"
     * control file — if present — overrides the link state: content "0"
     * means link down, anything else means up. This lets an integration
     * test inject link loss / recovery into a running master. It is done
     * here (not in send/receive) because get_link_state is off the RT
     * cyclic path and only polled periodically, like raw_get_link_state's
     * ioctl. Injected buses (sim_bus_create) use sim_bus_set_link() and
     * skip this. */
    if (bus->transport_owned && transport->interface[0]) {
        char linkpath[80];
        if (snprintf(linkpath, sizeof(linkpath), "%s.link",
                    transport->interface) < (int) sizeof(linkpath)) {
            FILE *f = fopen(linkpath, "r");
            if (f) {
                int c = fgetc(f);
                fclose(f);
                pthread_mutex_lock(&bus->lock);
                bus->link_up = (c != '0');
                pthread_mutex_unlock(&bus->lock);
            }
        }
    }

    pthread_mutex_lock(&bus->lock);
    up = bus->link_up;
    bus->link_polls++;
    pthread_mutex_unlock(&bus->lock);
    return up;
}

unsigned long sim_bus_link_polls(sim_bus_t *bus)
{
    unsigned long n;

    pthread_mutex_lock(&bus->lock);
    n = bus->link_polls;
    pthread_mutex_unlock(&bus->lock);
    return n;
}

static int sim_get_mac(ec_transport_t *transport, uint8_t mac[6])
{
    static const uint8_t sim_mac[6] = { 0x02, 0x53, 0x49, 0x4D, 0x00, 0x01 };

    (void) transport;
    memcpy(mac, sim_mac, 6);
    return 0;
}

static int sim_get_fd(ec_transport_t *transport)
{
    (void) transport;
    return -1;
}

/* No IRQs to move: records the request (and how far the bus traffic had
 * got by then) for sim_bus_irq_cpu() / sim_bus_irq_pin_frames(), and
 * reports one IRQ pinned as a NIC with a single vector would. */
static int sim_set_cpu_affinity(ec_transport_t *transport, int cpu)
{
    sim_bus_t *bus = transport->priv;

    pthread_mutex_lock(&bus->lock);
    bus->irq_cpu = cpu;
    bus->irq_pin_frames = (long) bus->frame_count;
    pthread_mutex_unlock(&bus->lock);
    return 1;
}

const ec_transport_ops_t ec_transport_sim_ops = {
    .name = "sim",
    .open = sim_open,
    .close = sim_close,
    .get_tx_buffer = sim_get_tx_buffer,
    .send = sim_send,
    .receive = sim_receive,
    .get_link_state = sim_get_link_state,
    .get_mac = sim_get_mac,
    .get_fd = sim_get_fd,
    .set_cpu_affinity = sim_set_cpu_affinity,
};

/****************************************************************************/
/* Public API                                                               */
/****************************************************************************/

sim_bus_t *sim_bus_create(unsigned int nslaves,
        const sim_slave_identity_t *identities)
{
    sim_bus_t *bus;
    unsigned int i;

    if (!nslaves || nslaves > SIM_MAX_SLAVES || !identities) {
        return NULL;
    }

    bus = calloc(1, sizeof(*bus));
    if (!bus) {
        return NULL;
    }

    bus->nslaves = nslaves;
    bus->link_up = 1;
    bus->irq_cpu = -1;
    bus->irq_pin_frames = -1;
    pthread_mutex_init(&bus->lock, NULL);

    for (i = 0; i < nslaves; i++) {
        sim_slave_t *slave = &bus->slaves[i];

        /* The mailbox/SM emulation trusts these offsets, so reject
         * geometry that reaches outside the register array. */
        if ((size_t) identities[i].mbox_out_phys + identities[i].mbox_out_len
                    > SIM_REG_SIZE
                || (size_t) identities[i].mbox_in_phys
                    + identities[i].mbox_in_len > SIM_REG_SIZE
                || (size_t) identities[i].sm2_phys + identities[i].sm2_len
                    > SIM_REG_SIZE
                || (size_t) identities[i].sm3_phys + identities[i].sm3_len
                    > SIM_REG_SIZE) {
            fprintf(stderr, "sim: slave %u mailbox/SM geometry exceeds"
                    " SIM_REG_SIZE\n", i);
            pthread_mutex_destroy(&bus->lock);
            free(bus);
            return NULL;
        }

        sim_slave_init_regs(slave, i, nslaves, &identities[i]);
        sim_slave_init_eeprom(slave, &identities[i]);

        slave->mbox_out_phys = identities[i].mbox_out_phys;
        slave->mbox_out_len = identities[i].mbox_out_len;
        slave->mbox_in_phys = identities[i].mbox_in_phys;
        slave->mbox_in_len = identities[i].mbox_in_len;
        slave->sm2_phys = identities[i].sm2_phys;
        slave->sm2_len = identities[i].sm2_len;
        slave->sm3_phys = identities[i].sm3_phys;
        slave->sm3_len = identities[i].sm3_len;
        slave->loopback = identities[i].loopback;

        if (slave->mbox_out_len) {
            /* PDO assignment counts for SM0..SM3 (read by the master's
             * PDO-reading FSM during scan): no PDOs assigned via CoE. */
            unsigned int sm;
            for (sm = 0; sm < 4; sm++) {
                sim_od_entry_t *e =
                        sim_od_find(slave, (uint16_t) (0x1C10 + sm), 0, 1);
                if (e) {
                    e->data[0] = 0;
                    e->size = 1;
                }
            }
        }
    }

    bus->transport.ops = &ec_transport_sim_ops;
    bus->transport.priv = bus;
    snprintf(bus->transport.interface, sizeof(bus->transport.interface),
            "sim0");

    return bus;
}

void sim_bus_destroy(sim_bus_t *bus)
{
    if (bus) {
        pthread_mutex_destroy(&bus->lock);
        free(bus);
    }
}

ec_transport_t *sim_bus_transport(sim_bus_t *bus)
{
    return &bus->transport;
}

void sim_bus_set_link(sim_bus_t *bus, int up)
{
    pthread_mutex_lock(&bus->lock);
    bus->link_up = up;
    pthread_mutex_unlock(&bus->lock);
}

unsigned int sim_bus_slave_al_state(sim_bus_t *bus, unsigned int pos)
{
    unsigned int state = 0;

    if (pos < bus->nslaves) {
        pthread_mutex_lock(&bus->lock);
        state = sim_rd16(bus->slaves[pos].regs + 0x0130) & 0x000F;
        pthread_mutex_unlock(&bus->lock);
    }
    return state;
}

/* NOTE: returns an unlocked pointer. Safe only while no master thread
 * processes frames concurrently — in practice: after
 * ecrt_master_activate() returned (the idle thread is joined there and
 * the OP thread does not touch the transport) or before startup.
 * Touching regs during the idle phase would be a data race. */
uint8_t *sim_bus_slave_regs(sim_bus_t *bus, unsigned int pos)
{
    return pos < bus->nslaves ? bus->slaves[pos].regs : NULL;
}

unsigned long sim_bus_frame_count(sim_bus_t *bus)
{
    unsigned long n;

    pthread_mutex_lock(&bus->lock);
    n = bus->frame_count;
    pthread_mutex_unlock(&bus->lock);
    return n;
}

void sim_bus_lose_sii(sim_bus_t *bus, unsigned int pos, unsigned int scans)
{
    pthread_mutex_lock(&bus->lock);
    if (pos < bus->nslaves) {
        bus->slaves[pos].sii_lost_scans = scans;
    }
    pthread_mutex_unlock(&bus->lock);
}

void sim_bus_set_reply_delay(sim_bus_t *bus, unsigned int us)
{
    pthread_mutex_lock(&bus->lock);
    bus->reply_delay_ns = (uint64_t) us * 1000ULL;
    pthread_mutex_unlock(&bus->lock);
}

unsigned int sim_bus_scan_count(sim_bus_t *bus)
{
    unsigned int n;

    pthread_mutex_lock(&bus->lock);
    n = bus->scans;
    pthread_mutex_unlock(&bus->lock);
    return n;
}

int sim_bus_irq_cpu(sim_bus_t *bus)
{
    int cpu;

    pthread_mutex_lock(&bus->lock);
    cpu = bus->irq_cpu;
    pthread_mutex_unlock(&bus->lock);
    return cpu;
}

long sim_bus_irq_pin_frames(sim_bus_t *bus)
{
    long n;

    pthread_mutex_lock(&bus->lock);
    n = bus->irq_pin_frames;
    pthread_mutex_unlock(&bus->lock);
    return n;
}

unsigned long sim_bus_al_status_brd_count(sim_bus_t *bus)
{
    unsigned long n;

    pthread_mutex_lock(&bus->lock);
    n = bus->al_status_brds;
    pthread_mutex_unlock(&bus->lock);
    return n;
}

int sim_bus_mbox_preload(sim_bus_t *bus, unsigned int pos, uint8_t type,
        const void *payload, uint16_t len)
{
    sim_slave_t *slave;

    if (pos >= bus->nslaves) {
        return -1;
    }

    pthread_mutex_lock(&bus->lock);
    slave = &bus->slaves[pos];
    if (!slave->mbox_in_len) {
        pthread_mutex_unlock(&bus->lock);
        return -1;
    }
    sim_mbox_respond_type(slave, type, payload, len);
    pthread_mutex_unlock(&bus->lock);
    return 0;
}

int sim_bus_mbox_full(sim_bus_t *bus, unsigned int pos)
{
    int full;

    if (pos >= bus->nslaves) {
        return -1;
    }

    pthread_mutex_lock(&bus->lock);
    full = (bus->slaves[pos].regs[0x080D] & 0x08) ? 1 : 0;
    pthread_mutex_unlock(&bus->lock);
    return full;
}

int sim_bus_od_set(sim_bus_t *bus, unsigned int pos, uint16_t index,
        uint8_t subindex, const void *data, size_t size)
{
    sim_od_entry_t *entry;
    int ret = -1;

    if (pos >= bus->nslaves || size > SIM_OD_DATA_SIZE) {
        return -1;
    }

    pthread_mutex_lock(&bus->lock);
    entry = sim_od_find(&bus->slaves[pos], index, subindex, 1);
    if (entry) {
        memcpy(entry->data, data, size);
        entry->size = size;
        ret = 0;
    }
    pthread_mutex_unlock(&bus->lock);
    return ret;
}

unsigned long sim_bus_od_download_count(sim_bus_t *bus, unsigned int pos)
{
    unsigned long n = 0;

    if (pos < bus->nslaves) {
        pthread_mutex_lock(&bus->lock);
        n = bus->slaves[pos].od_downloads;
        pthread_mutex_unlock(&bus->lock);
    }
    return n;
}

const uint8_t *sim_bus_od_data(sim_bus_t *bus, unsigned int pos,
        uint16_t index, uint8_t subindex, size_t *size)
{
    sim_od_entry_t *entry;
    const uint8_t *data = NULL;

    if (pos >= bus->nslaves) {
        return NULL;
    }

    pthread_mutex_lock(&bus->lock);
    entry = sim_od_find(&bus->slaves[pos], index, subindex, 0);
    if (entry) {
        if (size) {
            *size = entry->size;
        }
        data = entry->data;
    }
    pthread_mutex_unlock(&bus->lock);
    return data;
}
