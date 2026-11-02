/*****************************************************************************
 *
 *  Integration test: link-down and rescan behavior on the simulated bus.
 *
 *  A) Startup with the link down: the initial scan must NOT be declared
 *     complete while the broadcast datagram errors (regression for
 *     commit 5dab28a0). A helper thread raises the link after 1.5 s;
 *     ecrt_startup_master() must then return the fully scanned bus.
 *  B) Idle-phase link loss: master state reports link down and no
 *     responding slaves; after the link returns, the bus is rescanned
 *     and slave identities are intact.
 *  C) Cable yank while activated: from OP with complete working
 *     counter, the link drops (WC goes to zero, slave loses its
 *     operational flag) and returns — the master must reconfigure the
 *     slave back to OP automatically with process data flowing again.
 *
 *  This file is part of the IgH EtherCAT Master.
 *
 *  The IgH EtherCAT Master is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License version 2, as
 *  published by the Free Software Foundation.
 *
 ****************************************************************************/

#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>

#include "transport_sim.h"

#include "test.h"

#define SM2_PHYS 0x1100
#define SM3_PHYS 0x1400

#define VENDOR_ID 0x00000E17
#define PRODUCT_CODE 0x51340004

#define MAX_CYCLES 15000

/** Poll \a cond every 50 ms until true or \a timeout_ms elapsed. */
#define WAIT_FOR(cond, timeout_ms) \
    ({ \
        unsigned int _t = 0; \
        while (!(cond) && _t < (unsigned int) (timeout_ms)) { \
            usleep(50000); \
            _t += 50; \
        } \
        !!(cond); \
    })

static const sim_slave_identity_t identities[2] = {
    { .vendor_id = VENDOR_ID, .product_code = PRODUCT_CODE,
      .revision_number = 1, .serial_number = 4001, .alias = 0,
      .sm2_phys = SM2_PHYS, .sm2_len = 8,
      .sm3_phys = SM3_PHYS, .sm3_len = 8 },
    { .vendor_id = VENDOR_ID, .product_code = PRODUCT_CODE + 1,
      .revision_number = 1, .serial_number = 4002, .alias = 0 },
};

static ec_pdo_entry_info_t out_entries[] = {
    { 0x7000, 0x01, 16 },
};

static ec_pdo_entry_info_t in_entries[] = {
    { 0x6000, 0x01, 16 },
};

static ec_pdo_info_t rx_pdos[] = {
    { 0x1600, 1, out_entries },
};

static ec_pdo_info_t tx_pdos[] = {
    { 0x1A00, 1, in_entries },
};

static ec_sync_info_t syncs[] = {
    { 2, EC_DIR_OUTPUT, 1, rx_pdos, EC_WD_ENABLE },
    { 3, EC_DIR_INPUT, 1, tx_pdos, EC_WD_DISABLE },
    { 0xff, EC_DIR_INVALID, 0, NULL, EC_WD_DEFAULT }
};

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000 + (uint64_t) ts.tv_nsec / 1000000;
}

static _Atomic uint64_t link_up_at_ms;

static void *link_up_later(void *arg)
{
    sim_bus_t *bus = arg;

    /* Wait until the master's startup loop demonstrably runs and polls
     * the (down) link a few times — event-based instead of a fixed
     * wall-clock sleep, which would race the 5 s scan-progress timeout
     * on a badly loaded CI machine. */
    while (sim_bus_link_polls(bus) < 3) {
        usleep(10000);
    }
    usleep(100000); /* let it observe the down link a little longer */
    link_up_at_ms = now_ms();
    sim_bus_set_link(bus, 1);
    return NULL;
}

int main(void)
{
    sim_bus_t *bus;
    ec_master_t *master;
    ec_master_info_t info;
    ec_master_state_t state;
    ec_slave_info_t slave;
    pthread_t tid;
    uint64_t start, end;

    bus = sim_bus_create(2, identities);
    TEST_CHECK(bus != NULL);
    if (!bus) {
        return test_done("test_sim_link");
    }

    TEST_CHECK_EQ(0, ecrt_lib_init(NULL, NULL));

    /* A) Startup with the link down. Without the 5dab28a0 fix the
     * errored broadcast declares the scan done immediately and startup
     * returns an empty bus; with it, startup waits for the link. */
    sim_bus_set_link(bus, 0);
    TEST_CHECK_EQ(0, pthread_create(&tid, NULL, link_up_later, bus));

    start = now_ms();
    master = ecrt_startup_master(0, sim_bus_transport(bus), NULL, 0, -1, -1);
    end = now_ms();
    TEST_CHECK(master != NULL);
    pthread_join(tid, NULL);
    if (!master) {
        goto out_cleanup;
    }

    /* The scan cannot have finished before the link came up. */
    TEST_CHECK(start <= link_up_at_ms && link_up_at_ms <= end);
    TEST_CHECK_EQ(0, ecrt_master(master, &info));
    TEST_CHECK_EQ(2, info.slave_count);

    /* B) Idle-phase link loss and recovery. */
    TEST_CHECK_EQ(0, ecrt_master_state(master, &state));
    TEST_CHECK_EQ(1, state.link_up);
    TEST_CHECK_EQ(2, state.slaves_responding);

    sim_bus_set_link(bus, 0);
    TEST_CHECK(WAIT_FOR((ecrt_master_state(master, &state) == 0
                    && !state.link_up), 5000));
    TEST_CHECK(WAIT_FOR((ecrt_master_state(master, &state) == 0
                    && state.slaves_responding == 0), 5000));

    sim_bus_set_link(bus, 1);
    TEST_CHECK(WAIT_FOR((ecrt_master_state(master, &state) == 0
                    && state.link_up && state.slaves_responding == 2), 10000));
    TEST_CHECK(WAIT_FOR((ecrt_master(master, &info) == 0
                    && !info.scan_busy), 10000));

    /* Identities must be intact after the rescan. */
    TEST_CHECK_EQ(0, ecrt_master_get_slave(master, 0, &slave));
    TEST_CHECK_EQ(4001, slave.serial_number);
    TEST_CHECK_EQ(0, ecrt_master_get_slave(master, 1, &slave));
    TEST_CHECK_EQ(4002, slave.serial_number);

    /* C) Cable yank while activated. */
    {
        ec_domain_t *domain = ecrt_master_create_domain(master);
        ec_slave_config_t *sc = ecrt_master_slave_config(master, 0, 0,
                VENDOR_ID, PRODUCT_CODE);
        ec_slave_config_state_t sc_state;
        ec_domain_state_t dstate;
        uint8_t *pd;
        int off_out, off_in;
        unsigned int cycles;
        int reached;

        TEST_CHECK(domain != NULL);
        TEST_CHECK(sc != NULL);
        if (!domain || !sc) {
            goto out_release;
        }
        TEST_CHECK_EQ(0, ecrt_slave_config_pdos(sc, EC_END, syncs));
        off_out = ecrt_slave_config_reg_pdo_entry(sc, 0x7000, 0x01, domain,
                NULL);
        off_in = ecrt_slave_config_reg_pdo_entry(sc, 0x6000, 0x01, domain,
                NULL);
        TEST_CHECK(off_out >= 0);
        TEST_CHECK(off_in >= 0);
        TEST_CHECK_EQ(0, ecrt_master_activate(master));
        pd = ecrt_domain_data(domain);
        TEST_CHECK(pd != NULL);
        if (!pd) {
            goto out_release;
        }

        /* Reach OP with complete working counter. */
        reached = 0;
        for (cycles = 0; cycles < MAX_CYCLES; cycles++) {
            ecrt_master_receive(master);
            ecrt_domain_process(domain);
            EC_WRITE_U16(pd + off_out, 0x1111);
            ecrt_domain_queue(domain);
            ecrt_master_send(master);
            usleep(1000);
            ecrt_slave_config_state(sc, &sc_state);
            ecrt_domain_state(domain, &dstate);
            if (sc_state.operational && dstate.wc_state == EC_WC_COMPLETE) {
                reached = 1;
                break;
            }
        }
        TEST_CHECK(reached);

        /* Yank the cable: the working counter must collapse to zero
         * while the application keeps cycling. */
        sim_bus_set_link(bus, 0);
        reached = 0;
        for (cycles = 0; cycles < MAX_CYCLES; cycles++) {
            ecrt_master_receive(master);
            ecrt_domain_process(domain);
            ecrt_domain_queue(domain);
            ecrt_master_send(master);
            usleep(1000);
            ecrt_domain_state(domain, &dstate);
            if (dstate.wc_state == EC_WC_ZERO) {
                reached = 1;
                break;
            }
        }
        TEST_CHECK(reached);

        /* Plug it back in: the master must rescan, reconfigure the
         * slave back to OP and resume the process data exchange. */
        sim_bus_set_link(bus, 1);
        reached = 0;
        for (cycles = 0; cycles < MAX_CYCLES; cycles++) {
            ecrt_master_receive(master);
            ecrt_domain_process(domain);
            EC_WRITE_U16(pd + off_out, 0x2222);
            ecrt_domain_queue(domain);
            ecrt_master_send(master);
            usleep(1000);
            ecrt_slave_config_state(sc, &sc_state);
            ecrt_domain_state(domain, &dstate);
            if (sc_state.operational && dstate.wc_state == EC_WC_COMPLETE) {
                if (++reached >= 3) { /* let the output propagate */
                    break;
                }
            }
        }
        TEST_CHECK(reached >= 3);
        TEST_CHECK_EQ(8, sim_bus_slave_al_state(bus, 0));
        TEST_CHECK_EQ(0x2222,
                EC_READ_U16(sim_bus_slave_regs(bus, 0) + SM2_PHYS));
    }

out_release:
    ecrt_release_master(master);
out_cleanup:
    ecrt_lib_cleanup();
    sim_bus_destroy(bus);

    return test_done("test_sim_link");
}
