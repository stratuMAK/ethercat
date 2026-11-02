/*****************************************************************************
 *
 *  Integration test: a startup scan that left slaves unscanned is repeated.
 *
 *  A slave whose SII replies are lost (on a coating machine: delivered
 *  after the datagram timeout) is left without identity and PDO
 *  information, and the application's configuration fails on it.  So
 *  ecrt_startup_master() does not return on such a scan: the master scans
 *  the bus again, a bounded number of times, and returns with the bus as
 *  the last attempt left it -- the failed slaves named in the log.
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
      .revision_number = 0x00110000, .serial_number = 3001, .alias = 0 },
    { .vendor_id = 0x0000066F, .product_code = 0x60380007,
      .revision_number = 0x00010000, .serial_number = 3002, .alias = 0 },
    { .vendor_id = 0x00000002, .product_code = 0x0C1E3052,
      .revision_number = 0x00100000, .serial_number = 3003, .alias = 0 },
};

/* Start a master with slave 1's SII replies lost during the first
 * lost_scans scans; report how many scans it made before returning and
 * whether slave 1 was identified. */
static void run(unsigned int lost_scans, unsigned int *scans, int *identified)
{
    sim_bus_t *bus;
    ec_master_t *master;

    *scans = 0;
    *identified = -1;

    bus = sim_bus_create(3, identities);
    TEST_CHECK(bus != NULL);
    if (!bus) {
        return;
    }
    sim_bus_lose_sii(bus, 1, lost_scans);
    TEST_CHECK_EQ(0, ecrt_lib_init(NULL, NULL));

    master = ecrt_startup_master(0, sim_bus_transport(bus), NULL, 0, -1, -1);
    TEST_CHECK(master != NULL);
    if (master) {
        ec_master_info_t info;
        ec_slave_info_t slave;

        *scans = sim_bus_scan_count(bus);
        TEST_CHECK_EQ(0, ecrt_master(master, &info));
        TEST_CHECK_EQ(3, info.slave_count);
        TEST_CHECK_EQ(0, info.scan_busy);
        TEST_CHECK_EQ(0, ecrt_master_get_slave(master, 1, &slave));
        *identified = slave.vendor_id == identities[1].vendor_id
            && slave.serial_number == identities[1].serial_number;
        ecrt_release_master(master);
    }

    ecrt_lib_cleanup();
    sim_bus_destroy(bus);
}

int main(void)
{
    unsigned int scans;
    int identified;

    /* A clean bus: one scan. */
    run(0, &scans, &identified);
    TEST_CHECK_EQ(1, scans);
    TEST_CHECK_EQ(1, identified);

    /* Slave 1 lost in the first scan: scanned again, and it is there. */
    run(1, &scans, &identified);
    TEST_CHECK_EQ(2, scans);
    TEST_CHECK_EQ(1, identified);

    /* Lost in every scan: three attempts, then startup returns with the
     * slave unscanned -- bounded, not a hang. */
    run(100, &scans, &identified);
    TEST_CHECK_EQ(3, scans);
    TEST_CHECK_EQ(0, identified);

    return test_done("test_sim_scan_retry");
}
