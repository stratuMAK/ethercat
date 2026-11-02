/*****************************************************************************
 *
 *  Integration test: the bus scan survives replies slower than the cyclic
 *  datagram timeout.
 *
 *  Before activation the master's idle thread runs on a CPU shared with
 *  the rest of the system, and so may the NIC's interrupts: on a coating
 *  machine the scan's replies took 0.5-1.8 ms, past the 500 us datagram
 *  timeout, and slaves were left without their SII.  The scan is not
 *  time-critical, so the idle phase gives a datagram EC_IDLE_IO_TIMEOUT;
 *  the cyclic exchange keeps EC_IO_TIMEOUT.
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
      .revision_number = 0x00110000, .serial_number = 4001, .alias = 0 },
    { .vendor_id = 0x0000066F, .product_code = 0x60380007,
      .revision_number = 0x00010000, .serial_number = 4002, .alias = 0 },
    { .vendor_id = 0x00000002, .product_code = 0x0C1E3052,
      .revision_number = 0x00100000, .serial_number = 4003, .alias = 0 },
};

int main(void)
{
    sim_bus_t *bus;
    ec_master_t *master;
    unsigned int i;

    bus = sim_bus_create(3, identities);
    TEST_CHECK(bus != NULL);
    if (!bus) {
        return test_done("test_sim_scan_slow_replies");
    }
    /* Every reply 800 us late: each datagram of the scan would time out
     * with the cyclic 500 us. */
    sim_bus_set_reply_delay(bus, 800);
    TEST_CHECK_EQ(0, ecrt_lib_init(NULL, NULL));

    master = ecrt_startup_master(0, sim_bus_transport(bus), NULL, 0, -1, -1);
    TEST_CHECK(master != NULL);
    if (master) {
        ec_master_info_t info;

        TEST_CHECK_EQ(0, ecrt_master(master, &info));
        TEST_CHECK_EQ(3, info.slave_count);
        /* One scan, every slave identified: no reply was given up on. */
        TEST_CHECK_EQ(1, sim_bus_scan_count(bus));
        for (i = 0; i < 3; i++) {
            ec_slave_info_t slave;
            TEST_CHECK_EQ(0, ecrt_master_get_slave(master, i, &slave));
            TEST_CHECK_EQ(identities[i].serial_number, slave.serial_number);
        }
        ecrt_release_master(master);
    }

    ecrt_lib_cleanup();
    sim_bus_destroy(bus);

    return test_done("test_sim_scan_slow_replies");
}
