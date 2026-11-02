/*****************************************************************************
 *
 *  Integration test: SDO Information Service (dictionary fetch).
 *
 *  A CoE slave advertising enable_sdo_info: the master fetches the SDO
 *  dictionary automatically EC_WAIT_SDO_DICT seconds after the slave
 *  reaches PREOP (OD list, object description and entry description
 *  requests through the real mailbox machinery). The test waits for the
 *  fetch and verifies the cached dictionary through the tool API — the
 *  same surface the `ethercat sdos` command uses over IPC.
 *
 *  This file is part of the IgH EtherCAT Master.
 *
 *  The IgH EtherCAT Master is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License version 2, as
 *  published by the Free Software Foundation.
 *
 ****************************************************************************/

#include <string.h>
#include <unistd.h>

#include "transport_sim.h"

#include "ecrt_tool.h"

#include "test.h"

#define VENDOR_ID 0x00000E17
#define PRODUCT_CODE 0x51340009

/* 4 auto-populated PDO-assignment counters (0x1C10..0x1C13) plus the
 * two preset objects below. */
#define EXPECTED_SDO_COUNT 6

static const sim_slave_identity_t identities[1] = {
    { .vendor_id = VENDOR_ID, .product_code = PRODUCT_CODE,
      .revision_number = 1, .serial_number = 7001, .alias = 0,
      .mbox_out_phys = 0x1000, .mbox_out_len = 128,
      .mbox_in_phys = 0x1080, .mbox_in_len = 128 },
};

static const char test_string[] = "sdo info service test";

int main(void)
{
    sim_bus_t *bus;
    ec_master_t *master;
    ec_slave_info_t slave;
    ec_tool_slave_sdo_t sdo;
    ec_tool_slave_sdo_entry_t entry;
    uint8_t buf[8];
    unsigned int waited, i;
    int found;

    bus = sim_bus_create(1, identities);
    TEST_CHECK(bus != NULL);
    if (!bus) {
        return test_done("test_sim_sdo_info");
    }

    /* An array-ish object with two entries and a string object. */
    EC_WRITE_U32(buf, 0xDD1CDD1C);
    TEST_CHECK_EQ(0, sim_bus_od_set(bus, 0, 0x2000, 0, buf, 4));
    EC_WRITE_U16(buf, 0x2222);
    TEST_CHECK_EQ(0, sim_bus_od_set(bus, 0, 0x2000, 1, buf, 2));
    TEST_CHECK_EQ(0, sim_bus_od_set(bus, 0, 0x2001, 0, test_string,
                sizeof(test_string)));

    TEST_CHECK_EQ(0, ecrt_lib_init(NULL, NULL));
    master = ecrt_startup_master(0, sim_bus_transport(bus), NULL, 0, -1, -1);
    TEST_CHECK(master != NULL);
    if (!master) {
        goto out_cleanup;
    }

    /* The dictionary fetch starts EC_WAIT_SDO_DICT (3) seconds after
     * PREOP; poll the slave's SDO count until it appears. */
    for (waited = 0; waited < 15000; waited += 100) {
        TEST_CHECK_EQ(0, ecrt_master_get_slave(master, 0, &slave));
        if (slave.sdo_count == EXPECTED_SDO_COUNT) {
            break;
        }
        usleep(100000);
    }
    TEST_CHECK_EQ(EXPECTED_SDO_COUNT, slave.sdo_count);

    /* Objects appear in the dictionary right after the OD list
     * response, while the per-entry descriptions are still being
     * fetched (one mailbox round trip each). Wait until the LAST
     * entry (0x2001:00, fetched after all others) is cached. */
    for (waited = 0; waited < 15000; waited += 100) {
        memset(&entry, 0, sizeof(entry));
        entry.slave_position = 0;
        entry.sdo_spec = 0x2001;
        entry.sdo_entry_subindex = 0;
        if (ecrt_tool_get_slave_sdo_entry(master, &entry) == 0) {
            break;
        }
        usleep(100000);
    }

    /* Find object 0x2000 in the cached dictionary via the tool API. */
    found = -1;
    for (i = 0; i < slave.sdo_count; i++) {
        memset(&sdo, 0, sizeof(sdo));
        sdo.slave_position = 0;
        sdo.sdo_position = (uint16_t) i;
        TEST_CHECK_EQ(0, ecrt_tool_get_slave_sdo(master, &sdo));
        if (sdo.sdo_index == 0x2000) {
            found = (int) i;
            break;
        }
    }
    TEST_CHECK(found >= 0);
    if (found >= 0) {
        TEST_CHECK_EQ(1, sdo.max_subindex);
        TEST_CHECK_STREQ("SimObj2000", (const char *) sdo.name);
    }

    /* Entry descriptions of 0x2000. */
    memset(&entry, 0, sizeof(entry));
    entry.slave_position = 0;
    entry.sdo_spec = 0x2000;
    entry.sdo_entry_subindex = 0;
    TEST_CHECK_EQ(0, ecrt_tool_get_slave_sdo_entry(master, &entry));
    TEST_CHECK_EQ(0x0007, entry.data_type); /* UDINT */
    TEST_CHECK_EQ(32, entry.bit_length);
    TEST_CHECK_STREQ("Entry00", (const char *) entry.description);
    for (i = 0; i < EC_TOOL_SDO_ENTRY_ACCESS_COUNT; i++) {
        TEST_CHECK_EQ(1, entry.read_access[i]);
        TEST_CHECK_EQ(1, entry.write_access[i]);
    }

    memset(&entry, 0, sizeof(entry));
    entry.slave_position = 0;
    entry.sdo_spec = 0x2000;
    entry.sdo_entry_subindex = 1;
    TEST_CHECK_EQ(0, ecrt_tool_get_slave_sdo_entry(master, &entry));
    TEST_CHECK_EQ(0x0006, entry.data_type); /* UINT */
    TEST_CHECK_EQ(16, entry.bit_length);

    /* The string object. */
    memset(&entry, 0, sizeof(entry));
    entry.slave_position = 0;
    entry.sdo_spec = 0x2001;
    entry.sdo_entry_subindex = 0;
    TEST_CHECK_EQ(0, ecrt_tool_get_slave_sdo_entry(master, &entry));
    TEST_CHECK_EQ(0x0009, entry.data_type); /* VISIBLE_STRING */
    TEST_CHECK_EQ(sizeof(test_string) * 8, entry.bit_length);

    ecrt_release_master(master);
out_cleanup:
    ecrt_lib_cleanup();
    sim_bus_destroy(bus);

    return test_done("test_sim_sdo_info");
}
