/*****************************************************************************
 *
 *  Integration test: a stale mailbox response does not corrupt the scan.
 *
 *  A master that exits mid-transfer leaves its last response sitting unread
 *  in the slave's send mailbox.  The next master to come up starts its scan
 *  by reading the PDO assignment over CoE — and used to pick up that stale
 *  response as the answer to its own request, which surfaces as
 *
 *      Received unknown response while uploading SDO 0x1C12:00.
 *      Failed to read number of assigned PDOs for SM2.
 *      Received upload response for wrong SDO (0x1C12:00, requested 0x1C13:00).
 *
 *  and leaves the scanned PDO assignment wrong for that slave.  The scan now
 *  drains the send mailbox before touching CoE.
 *
 *  The test preloads a leftover SDO-Information response — the same kind of
 *  frame an interrupted dictionary fetch leaves behind — then scans and
 *  verifies that the PDO assignment came back intact.
 *
 *  This file is part of the IgH EtherCAT Master.
 *
 *  The IgH EtherCAT Master is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License version 2, as
 *  published by the Free Software Foundation.
 *
 ****************************************************************************/

#include <string.h>

#include "transport_sim.h"

#include "ecrt_tool.h"

#include "test.h"

#define SM2_PHYS 0x1100
#define SM3_PHYS 0x1400

#define VENDOR_ID 0x00000E17
#define PRODUCT_CODE 0x5134000A

/** Sync manager 2 carries exactly this PDO, and the scan has to find it. */
#define SM2_INDEX 2
#define RX_PDO_INDEX 0x1600

static const sim_slave_identity_t identities[1] = {
    { .vendor_id = VENDOR_ID, .product_code = PRODUCT_CODE,
      .revision_number = 1, .serial_number = 9001, .alias = 0,
      .sm2_phys = SM2_PHYS, .sm2_len = 8,
      .sm3_phys = SM3_PHYS, .sm3_len = 8,
      .mbox_out_phys = 0x1000, .mbox_out_len = 128,
      .mbox_in_phys = 0x1080, .mbox_in_len = 128 },
};

/** A CoE SDO-Information "Entry Res" for 0x1602:0x72, i.e. exactly what an
 * interrupted dictionary fetch leaves queued in the mailbox. */
static const uint8_t stale_response[] = {
    0x00, 0x80,             /* CoE header: service 8 = SDO Information */
    0x06,                   /* opcode: entry description response */
    0x00,                   /* reserved */
    0x00, 0x00,             /* fragments left */
    0x02, 0x16,             /* object index 0x1602 */
    0x72,                   /* subindex */
    0x00,                   /* value info */
    0x07, 0x00,             /* data type */
    0x20, 0x00,             /* bit length */
    0x07, 0x00,             /* object access */
    'M', 'a', 'p', 'p', 'e', 'd', ' ', 'O', 'b', 'j', '.'
};

int main(void)
{
    sim_bus_t *bus;
    ec_master_t *master;
    ec_master_info_t info;
    ec_tool_slave_sync_t sync;
    ec_tool_slave_sync_pdo_t pdo;
    uint8_t buf[4];

    bus = sim_bus_create(1, identities);
    TEST_CHECK(bus != NULL);
    if (!bus) {
        return test_done("test_sim_scan_stale_mbox");
    }

    /* SM2 has one assigned PDO (0x1600) with one mapped entry. This is what
     * the scan reads over CoE, and what the stale response used to break. */
    EC_WRITE_U8(buf, 1);
    TEST_CHECK_EQ(0, sim_bus_od_set(bus, 0, 0x1C12, 0, buf, 1));
    EC_WRITE_U16(buf, RX_PDO_INDEX);
    TEST_CHECK_EQ(0, sim_bus_od_set(bus, 0, 0x1C12, 1, buf, 2));
    EC_WRITE_U8(buf, 0);
    TEST_CHECK_EQ(0, sim_bus_od_set(bus, 0, 0x1C13, 0, buf, 1));
    EC_WRITE_U8(buf, 1);
    TEST_CHECK_EQ(0, sim_bus_od_set(bus, 0, RX_PDO_INDEX, 0, buf, 1));
    EC_WRITE_U32(buf, 0x70000110); /* 0x7000:01, 16 bit */
    TEST_CHECK_EQ(0, sim_bus_od_set(bus, 0, RX_PDO_INDEX, 1, buf, 4));

    /* The previous master died here: the bus is still hot, so the slave is
     * past INIT (the scan therefore reads the mailbox configuration instead
     * of driving the slave to PREOP first), and its last response is still
     * queued unread. */
    {
        uint8_t *regs = sim_bus_slave_regs(bus, 0);
        TEST_CHECK(regs != NULL);
        if (regs) {
            EC_WRITE_U16(regs + 0x0130, 0x0002); /* AL status: PREOP */
            /* Mailbox sync managers, as the previous master left them:
             * SM0 (receive) at 0x0800, SM1 (send) at 0x0808. */
            EC_WRITE_U16(regs + 0x0800, identities[0].mbox_out_phys);
            EC_WRITE_U16(regs + 0x0802, identities[0].mbox_out_len);
            EC_WRITE_U16(regs + 0x0808, identities[0].mbox_in_phys);
            EC_WRITE_U16(regs + 0x080A, identities[0].mbox_in_len);
        }
    }
    TEST_CHECK_EQ(0, sim_bus_mbox_preload(bus, 0, 0x03 /* CoE */,
                stale_response, sizeof(stale_response)));
    TEST_CHECK_EQ(1, sim_bus_mbox_full(bus, 0));

    TEST_CHECK_EQ(0, ecrt_lib_init(NULL, NULL));

    /* Startup blocks until the initial scan completes. */
    master = ecrt_startup_master(0, sim_bus_transport(bus), NULL, 0, -1, -1);
    TEST_CHECK(master != NULL);
    if (!master) {
        goto out_cleanup;
    }

    TEST_CHECK_EQ(0, ecrt_master(master, &info));
    TEST_CHECK_EQ(1, info.slave_count);

    /* The leftover response was discarded, not handed to the scan. */
    TEST_CHECK_EQ(0, sim_bus_mbox_full(bus, 0));

    /* And the scan read the real PDO assignment: SM2 carries 0x1600. */
    memset(&sync, 0, sizeof(sync));
    sync.slave_position = 0;
    sync.sync_index = SM2_INDEX;
    TEST_CHECK_EQ(0, ecrt_tool_get_slave_sync(master, &sync));
    TEST_CHECK_EQ(1, sync.pdo_count);

    memset(&pdo, 0, sizeof(pdo));
    pdo.slave_position = 0;
    pdo.sync_index = SM2_INDEX;
    pdo.pdo_pos = 0;
    TEST_CHECK_EQ(0, ecrt_tool_get_slave_sync_pdo(master, &pdo));
    TEST_CHECK_EQ(RX_PDO_INDEX, pdo.index);

    ecrt_release_master(master);
out_cleanup:
    ecrt_lib_cleanup();
    sim_bus_destroy(bus);

    return test_done("test_sim_scan_stale_mbox");
}
