/*****************************************************************************
 *
 *  Integration test: the NIC IRQs are pinned before the first bus scan.
 *
 *  On a CPU shared with the rest of the system the replies of the scan can
 *  reach the master later than the datagram timeout, and slaves are left
 *  without their SII (a coating machine failed to start after every reboot
 *  until its IRQ was pinned by hand).  So ecrt_startup_master() pins the
 *  transports' IRQs to irq_cpu before the idle phase sends anything, and
 *  leaves them alone with irq_cpu = -1.
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

#include <unistd.h>

static const sim_slave_identity_t identities[2] = {
    { .vendor_id = 0x00000002, .product_code = 0x044C2C52,
      .revision_number = 0x00110000, .serial_number = 2001, .alias = 0 },
    { .vendor_id = 0x00000002, .product_code = 0x0C1E3052,
      .revision_number = 0x00100000, .serial_number = 2002, .alias = 0 },
};

/* Start a master on a fresh two-slave bus with irq_cpu, let the bus run a
 * little, and report what the transport was asked to do. */
static void run(int irq_cpu, int *pinned_cpu, long *pin_frames,
        unsigned long *frames)
{
    sim_bus_t *bus;
    ec_master_t *master;

    *pinned_cpu = -2;
    *pin_frames = -2;
    *frames = 0;

    bus = sim_bus_create(2, identities);
    TEST_CHECK(bus != NULL);
    if (!bus) {
        return;
    }
    TEST_CHECK_EQ(0, ecrt_lib_init(NULL, NULL));

    master = ecrt_startup_master(0, sim_bus_transport(bus), NULL, 0, -1,
            irq_cpu);
    TEST_CHECK(master != NULL);
    if (master) {
        usleep(100000); /* the idle phase keeps polling the bus */
        *pinned_cpu = sim_bus_irq_cpu(bus);
        *pin_frames = sim_bus_irq_pin_frames(bus);
        *frames = sim_bus_frame_count(bus);
        ecrt_release_master(master);
    }

    ecrt_lib_cleanup();
    sim_bus_destroy(bus);
}

int main(void)
{
    int cpu;
    long at;
    unsigned long frames;

    /* irq_cpu given: pinned to it, before the first frame of the scan. */
    run(1, &cpu, &at, &frames);
    TEST_CHECK_EQ(1, cpu);
    TEST_CHECK_EQ(0, at);
    TEST_CHECK(frames > 0); /* the scan did run after it */

    /* -1: the transport is never asked. */
    run(-1, &cpu, &at, &frames);
    TEST_CHECK_EQ(-1, cpu);
    TEST_CHECK_EQ(-1, at);
    TEST_CHECK(frames > 0);

    return test_done("test_sim_irq_pin");
}
