/*****************************************************************************
 *
 *  Integration test: full bus scan against the simulated transport.
 *
 *  Starts a real userspace master (idle thread, FSMs, SII state machine)
 *  on a sim_bus with three slaves and verifies that the scan completes
 *  and reports the identities configured in the simulated SII EEPROMs.
 *  Exercises ecrt_lib_init/startup/release/cleanup teardown as well.
 *
 *  This file is part of the IgH EtherCAT Master.
 *
 *  The IgH EtherCAT Master is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License version 2, as
 *  published by the Free Software Foundation.
 *
 ****************************************************************************/

#include "transport_sim.h"

#include "test.h"

static const sim_slave_identity_t identities[3] = {
    { .vendor_id = 0x00000002, .product_code = 0x044C2C52,
      .revision_number = 0x00110000, .serial_number = 1001, .alias = 0 },
    { .vendor_id = 0x0000066F, .product_code = 0x60380007,
      .revision_number = 0x00010000, .serial_number = 1002, .alias = 42 },
    { .vendor_id = 0x00000002, .product_code = 0x0C1E3052,
      .revision_number = 0x00100000, .serial_number = 1003, .alias = 0 },
};

int main(void)
{
    sim_bus_t *bus;
    ec_master_t *master;
    ec_master_info_t info;
    ec_master_state_t state;
    unsigned int i;

    bus = sim_bus_create(3, identities);
    TEST_CHECK(bus != NULL);
    if (!bus) {
        return test_done("test_sim_scan");
    }

    TEST_CHECK_EQ(0, ecrt_lib_init(NULL, NULL /* no IPC server */));

    /* Startup blocks until the initial scan completes (or its watchdog
     * expires) — a NULL result means the scan never finished. */
    master = ecrt_startup_master(0, sim_bus_transport(bus), NULL, 0, -1, -1);
    TEST_CHECK(master != NULL);

    if (master) {
        TEST_CHECK(sim_bus_frame_count(bus) > 0);

        TEST_CHECK_EQ(0, ecrt_master(master, &info));
        TEST_CHECK_EQ(3, info.slave_count);
        TEST_CHECK_EQ(0, info.scan_busy);

        TEST_CHECK_EQ(0, ecrt_master_state(master, &state));
        TEST_CHECK_EQ(3, state.slaves_responding);
        TEST_CHECK_EQ(1, state.link_up);

        for (i = 0; i < 3; i++) {
            ec_slave_info_t slave;
            TEST_CHECK_EQ(0, ecrt_master_get_slave(master, i, &slave));
            TEST_CHECK_EQ(i, slave.position);
            TEST_CHECK_EQ(identities[i].vendor_id, slave.vendor_id);
            TEST_CHECK_EQ(identities[i].product_code, slave.product_code);
            TEST_CHECK_EQ(identities[i].revision_number,
                    slave.revision_number);
            TEST_CHECK_EQ(identities[i].serial_number, slave.serial_number);
            TEST_CHECK_EQ(identities[i].alias, slave.alias);
        }

        ecrt_release_master(master);
    }

    ecrt_lib_cleanup();
    sim_bus_destroy(bus);

    return test_done("test_sim_scan");
}
