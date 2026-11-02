/*****************************************************************************
 *
 *  Integration test: SDO dictionary fetches run in the background.
 *
 *  A dictionary fetch is a long series of mailbox transfers.  It used to
 *  run on the master FSM, which owns a single datagram — so for the whole
 *  fetch (tens of seconds on a real drive with a few thousand objects) the
 *  master issued no AL status broadcast, polled no slave state and started
 *  no slave configuration.  The visible symptom was an "all slaves
 *  operational" pin that stayed false for a minute after the bus was
 *  already fully in OP.
 *
 *  The fetch now runs on the slave's own FSM, on the external datagram
 *  ring.  This test brings a bus with two CoE slaves to OP, waits for the
 *  automatic dictionary fetch to start (EC_WAIT_SDO_DICT after PREOP) and
 *  verifies that the master's AL status broadcast keeps running at a
 *  comparable rate while the fetch is in progress — and that the
 *  dictionaries still arrive.
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

#include "test.h"

#define SM2_PHYS 0x1100
#define SM3_PHYS 0x1400

#define VENDOR_ID 0x00000E17
#define PRODUCT_PLAIN 0x51340008 /* no mailbox, no process data */
#define PRODUCT_COE 0x51340007   /* CoE + process-data slave */

#define NUM_SLAVES 4
#define IS_COE(i) ((i) % 2 == 1)

/** Extra objects preset on each CoE slave, on top of the four
 * auto-populated PDO assignment counters (0x1C10..0x1C13) and the two
 * mapping objects below — enough that the fetch spans many cycles. */
#define EXTRA_OBJECTS 20
#define MIN_SDO_COUNT (EXTRA_OBJECTS + 2)

#define MAX_CYCLES 30000
/** Sampling window for the broadcast rate, in cycles (1 ms each). */
#define SAMPLE_CYCLES 400

static sim_slave_identity_t identities[NUM_SLAVES];

static ec_pdo_entry_info_t out_entries[] = { { 0x7000, 0x01, 16 } };
static ec_pdo_entry_info_t in_entries[] = { { 0x6000, 0x01, 16 } };
static ec_pdo_info_t rx_pdos[] = { { 0x1600, 1, out_entries } };
static ec_pdo_info_t tx_pdos[] = { { 0x1A00, 1, in_entries } };

static ec_sync_info_t syncs[] = {
    { 2, EC_DIR_OUTPUT, 1, rx_pdos, EC_WD_ENABLE },
    { 3, EC_DIR_INPUT, 1, tx_pdos, EC_WD_DISABLE },
    { 0xff, EC_DIR_INVALID, 0, NULL, EC_WD_DEFAULT }
};

/** Run one bus cycle. */
static void cycle(ec_master_t *master, ec_domain_t *domain)
{
    ecrt_master_receive(master);
    ecrt_domain_process(domain);
    ecrt_domain_queue(domain);
    ecrt_master_send(master);
    usleep(1000);
}

/** Number of objects cached for slave \a pos, or 0. */
static unsigned int sdo_count(ec_master_t *master, uint16_t pos)
{
    ec_slave_info_t info;

    if (ecrt_master_get_slave(master, pos, &info) != 0) {
        return 0;
    }
    return info.sdo_count;
}

int main(void)
{
    sim_bus_t *bus;
    ec_master_t *master;
    ec_domain_t *domain;
    ec_slave_config_t *sc[NUM_SLAVES];
    ec_slave_config_state_t sc_state;
    ec_master_state_t ms;
    unsigned int i, j, cycles, all_op;
    unsigned long brd_idle, brd_busy;
    uint8_t buf[4];
    uint8_t zero = 0;

    for (i = 0; i < NUM_SLAVES; i++) {
        identities[i].vendor_id = VENDOR_ID;
        identities[i].product_code = IS_COE(i) ? PRODUCT_COE : PRODUCT_PLAIN;
        identities[i].revision_number = 1;
        identities[i].serial_number = 8000 + i;
        if (IS_COE(i)) {
            identities[i].sm2_phys = SM2_PHYS;
            identities[i].sm2_len = 8;
            identities[i].sm3_phys = SM3_PHYS;
            identities[i].sm3_len = 8;
            identities[i].mbox_out_phys = 0x1000;
            identities[i].mbox_out_len = 128;
            identities[i].mbox_in_phys = 0x1080;
            identities[i].mbox_in_len = 128;
        }
    }

    bus = sim_bus_create(NUM_SLAVES, identities);
    TEST_CHECK(bus != NULL);
    if (!bus) {
        return test_done("test_sim_dict_bg");
    }

    for (i = 0; i < NUM_SLAVES; i++) {
        if (!IS_COE(i)) {
            continue;
        }
        /* Empty, writable mapping objects, plus a dictionary worth
         * fetching. */
        TEST_CHECK_EQ(0, sim_bus_od_set(bus, i, 0x1600, 0, &zero, 1));
        TEST_CHECK_EQ(0, sim_bus_od_set(bus, i, 0x1A00, 0, &zero, 1));
        for (j = 0; j < EXTRA_OBJECTS; j++) {
            EC_WRITE_U32(buf, 0x2000 + j);
            TEST_CHECK_EQ(0, sim_bus_od_set(bus, i,
                        (uint16_t) (0x2000 + j), 0, buf, 4));
        }
    }

    TEST_CHECK_EQ(0, ecrt_lib_init(NULL, NULL));
    master = ecrt_startup_master(0, sim_bus_transport(bus), NULL, 0, -1, -1);
    TEST_CHECK(master != NULL);
    if (!master) {
        goto out_cleanup;
    }

    domain = ecrt_master_create_domain(master);
    TEST_CHECK(domain != NULL);
    if (!domain) {
        goto out_release;
    }

    for (i = 0; i < NUM_SLAVES; i++) {
        sc[i] = ecrt_master_slave_config(master, 0, (uint16_t) i, VENDOR_ID,
                identities[i].product_code);
        TEST_CHECK(sc[i] != NULL);
        if (!sc[i]) {
            goto out_release;
        }
        if (!IS_COE(i)) {
            continue; /* no mailbox, no process data */
        }
        TEST_CHECK_EQ(0, ecrt_slave_config_pdos(sc[i], EC_END, syncs));
        TEST_CHECK(ecrt_slave_config_reg_pdo_entry(sc[i], 0x7000, 0x01,
                    domain, NULL) >= 0);
        TEST_CHECK(ecrt_slave_config_reg_pdo_entry(sc[i], 0x6000, 0x01,
                    domain, NULL) >= 0);
    }

    TEST_CHECK_EQ(0, ecrt_master_activate(master));
    TEST_CHECK(ecrt_domain_data(domain) != NULL);

    /* Bring the bus up: every slave in OP and the master's own broadcast
     * snapshot agreeing (al_states == OP only). */
    for (cycles = 0; cycles < MAX_CYCLES; cycles++) {
        cycle(master, domain);

        all_op = 1;
        for (i = 0; i < NUM_SLAVES; i++) {
            ecrt_slave_config_state(sc[i], &sc_state);
            all_op = all_op && sc_state.operational;
        }
        ecrt_master_state(master, &ms);
        if (all_op && ms.al_states == 0x08) {
            break;
        }
    }
    TEST_CHECK(cycles < MAX_CYCLES);

    /* No dictionary is being fetched yet (EC_WAIT_SDO_DICT has not
     * elapsed): measure the undisturbed broadcast rate. */
    TEST_CHECK_EQ(0, sdo_count(master, 1));
    brd_idle = sim_bus_al_status_brd_count(bus);
    for (i = 0; i < SAMPLE_CYCLES; i++) {
        cycle(master, domain);
    }
    brd_idle = sim_bus_al_status_brd_count(bus) - brd_idle;
    TEST_CHECK(brd_idle > 0);

    /* Wait for the automatic fetch to start. */
    for (cycles = 0; cycles < MAX_CYCLES; cycles++) {
        cycle(master, domain);
        if (sdo_count(master, 1) > 0) {
            break;
        }
    }
    TEST_CHECK(cycles < MAX_CYCLES);

    /* While the fetch is in progress the master FSM must keep polling the
     * bus.  Before the fetch moved to the slave FSM this count was zero
     * for the entire transfer. */
    brd_busy = sim_bus_al_status_brd_count(bus);
    for (i = 0; i < SAMPLE_CYCLES; i++) {
        cycle(master, domain);
    }
    brd_busy = sim_bus_al_status_brd_count(bus) - brd_busy;

    fprintf(stderr, "test_sim_dict_bg: AL status broadcasts per %u cycles:"
            " idle %lu, during dictionary fetch %lu\n",
            SAMPLE_CYCLES, brd_idle, brd_busy);

    TEST_CHECK(brd_busy > 0);
    /* Generous: the fetch shares the ring with the master FSM, so some
     * slowdown is expected — a stall is not. */
    TEST_CHECK(brd_busy * 4 >= brd_idle);

    /* The bus state stayed visible and correct throughout. */
    ecrt_master_state(master, &ms);
    TEST_CHECK_EQ(0x08, ms.al_states);

    /* And the dictionaries still arrive, on both CoE slaves. */
    for (cycles = 0; cycles < MAX_CYCLES; cycles++) {
        cycle(master, domain);
        if (sdo_count(master, 1) >= MIN_SDO_COUNT
                && sdo_count(master, 3) >= MIN_SDO_COUNT) {
            break;
        }
    }
    TEST_CHECK(cycles < MAX_CYCLES);
    TEST_CHECK(sdo_count(master, 1) >= MIN_SDO_COUNT);
    TEST_CHECK(sdo_count(master, 3) >= MIN_SDO_COUNT);

out_release:
    ecrt_release_master(master);
out_cleanup:
    ecrt_lib_cleanup();
    sim_bus_destroy(bus);

    return test_done("test_sim_dict_bg");
}
