/*****************************************************************************
 *
 *  Integration test: multi-slave parallel configuration and one shared
 *  domain (Parallel Slave Configuration, commit ecf9dac8).
 *
 *  Six slaves on one bus: three plain process-data slaves, two
 *  CoE-capable slaves whose PDO assignment is written via SDO during
 *  configuration (their mailbox FSMs run concurrently), and one slave
 *  without process data. All five PD slaves share ONE domain, so the
 *  cyclic exchange is a logical datagram spanning five FMMU pairs
 *  (expected working counter 5 x 3 = 15).
 *
 *  Verifies: all slaves reach OP in parallel, the shared domain
 *  completes with WC 15, per-slave process data lands in the right
 *  slave in both directions (no cross-slave corruption in the logical
 *  address layout), CoE assignment objects are written on both mailbox
 *  slaves, and a deactivate/re-activate cycle brings everything back
 *  to OP without further SDO downloads (config matches).
 *
 *  This file is part of the IgH EtherCAT Master.
 *
 *  The IgH EtherCAT Master is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License version 2, as
 *  published by the Free Software Foundation.
 *
 ****************************************************************************/

#include <unistd.h>

#include "transport_sim.h"

#include "test.h"

#define SM2_PHYS 0x1100
#define SM3_PHYS 0x1400

#define VENDOR_ID 0x00000E17
#define PRODUCT_PD 0x51340006   /* plain process-data slave */
#define PRODUCT_COE 0x51340007  /* CoE + process-data slave */
#define PRODUCT_PLAIN 0x51340008 /* no process data */

#define NUM_SLAVES 6
#define NUM_PD 5 /* positions 0..4 carry process data */

#define MAX_CYCLES 20000

static const sim_slave_identity_t identities[NUM_SLAVES] = {
    { .vendor_id = VENDOR_ID, .product_code = PRODUCT_PD,
      .revision_number = 1, .serial_number = 6000,
      .sm2_phys = SM2_PHYS, .sm2_len = 8,
      .sm3_phys = SM3_PHYS, .sm3_len = 8 },
    { .vendor_id = VENDOR_ID, .product_code = PRODUCT_COE,
      .revision_number = 1, .serial_number = 6001,
      .sm2_phys = SM2_PHYS, .sm2_len = 8,
      .sm3_phys = SM3_PHYS, .sm3_len = 8,
      .mbox_out_phys = 0x1000, .mbox_out_len = 128,
      .mbox_in_phys = 0x1080, .mbox_in_len = 128 },
    { .vendor_id = VENDOR_ID, .product_code = PRODUCT_PD,
      .revision_number = 1, .serial_number = 6002,
      .sm2_phys = SM2_PHYS, .sm2_len = 8,
      .sm3_phys = SM3_PHYS, .sm3_len = 8 },
    { .vendor_id = VENDOR_ID, .product_code = PRODUCT_COE,
      .revision_number = 1, .serial_number = 6003,
      .sm2_phys = SM2_PHYS, .sm2_len = 8,
      .sm3_phys = SM3_PHYS, .sm3_len = 8,
      .mbox_out_phys = 0x1000, .mbox_out_len = 128,
      .mbox_in_phys = 0x1080, .mbox_in_len = 128 },
    { .vendor_id = VENDOR_ID, .product_code = PRODUCT_PD,
      .revision_number = 1, .serial_number = 6004,
      .sm2_phys = SM2_PHYS, .sm2_len = 8,
      .sm3_phys = SM3_PHYS, .sm3_len = 8 },
    { .vendor_id = VENDOR_ID, .product_code = PRODUCT_PLAIN,
      .revision_number = 1, .serial_number = 6005 },
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

/** Configure all PD slaves into one domain, activate and run to OP with
 * a complete working counter. Returns 1 on success. */
static int run_to_op(sim_bus_t *bus, ec_master_t *master,
        unsigned int *cycles_needed)
{
    ec_domain_t *domain;
    ec_slave_config_t *sc[NUM_PD];
    ec_slave_config_state_t sc_state;
    ec_domain_state_t dstate;
    uint8_t *pd;
    int off_out[NUM_PD], off_in[NUM_PD];
    unsigned int i, cycles, all_op, settled = 0;

    domain = ecrt_master_create_domain(master);
    TEST_CHECK(domain != NULL);
    if (!domain) {
        return 0;
    }

    for (i = 0; i < NUM_PD; i++) {
        sc[i] = ecrt_master_slave_config(master, 0, (uint16_t) i, VENDOR_ID,
                identities[i].product_code);
        TEST_CHECK(sc[i] != NULL);
        if (!sc[i]) {
            return 0;
        }
        TEST_CHECK_EQ(0, ecrt_slave_config_pdos(sc[i], EC_END, syncs));
        off_out[i] = ecrt_slave_config_reg_pdo_entry(sc[i], 0x7000, 0x01,
                domain, NULL);
        off_in[i] = ecrt_slave_config_reg_pdo_entry(sc[i], 0x6000, 0x01,
                domain, NULL);
        TEST_CHECK(off_out[i] >= 0);
        TEST_CHECK(off_in[i] >= 0);
    }

    TEST_CHECK_EQ(0, ecrt_master_activate(master));
    pd = ecrt_domain_data(domain);
    TEST_CHECK(pd != NULL);
    if (!pd) {
        return 0;
    }

    /* Feed distinct input values before starting. */
    for (i = 0; i < NUM_PD; i++) {
        EC_WRITE_U16(sim_bus_slave_regs(bus, i) + SM3_PHYS,
                (uint16_t) (0xB000 + i));
    }

    for (cycles = 0; cycles < MAX_CYCLES; cycles++) {
        ecrt_master_receive(master);
        ecrt_domain_process(domain);
        for (i = 0; i < NUM_PD; i++) {
            EC_WRITE_U16(pd + off_out[i], (uint16_t) (0xA000 + i));
        }
        ecrt_domain_queue(domain);
        ecrt_master_send(master);
        usleep(1000);

        all_op = 1;
        for (i = 0; i < NUM_PD; i++) {
            ecrt_slave_config_state(sc[i], &sc_state);
            all_op = all_op && sc_state.operational;
        }
        ecrt_domain_state(domain, &dstate);
        if (all_op && dstate.wc_state == EC_WC_COMPLETE) {
            if (++settled >= 3) { /* let the data propagate */
                break;
            }
        }
    }
    TEST_CHECK(cycles < MAX_CYCLES);
    if (cycles_needed) {
        *cycles_needed = cycles;
    }

    /* The shared logical exchange covers all five slaves: 5 x 3. */
    ecrt_domain_state(domain, &dstate);
    TEST_CHECK_EQ(EC_WC_COMPLETE, dstate.wc_state);
    TEST_CHECK_EQ(15, dstate.working_counter);

    /* No cross-slave corruption: each output landed in ITS slave's SM2
     * and each input arrived at ITS domain offset. */
    for (i = 0; i < NUM_PD; i++) {
        TEST_CHECK_EQ(0xA000 + i,
                EC_READ_U16(sim_bus_slave_regs(bus, i) + SM2_PHYS));
        TEST_CHECK_EQ(0xB000 + i, EC_READ_U16(pd + off_in[i]));
        TEST_CHECK_EQ(8, sim_bus_slave_al_state(bus, i));
    }

    return 1;
}

int main(void)
{
    sim_bus_t *bus;
    ec_master_t *master;
    ec_master_info_t info;
    unsigned int cycles1 = 0, cycles2 = 0;
    unsigned long downloads1a, downloads3a, downloads1b, downloads3b;
    uint8_t zero = 0;

    bus = sim_bus_create(NUM_SLAVES, identities);
    TEST_CHECK(bus != NULL);
    if (!bus) {
        return test_done("test_sim_multi");
    }

    /* CoE slaves power up with empty, writable mapping objects. */
    TEST_CHECK_EQ(0, sim_bus_od_set(bus, 1, 0x1600, 0, &zero, 1));
    TEST_CHECK_EQ(0, sim_bus_od_set(bus, 1, 0x1A00, 0, &zero, 1));
    TEST_CHECK_EQ(0, sim_bus_od_set(bus, 3, 0x1600, 0, &zero, 1));
    TEST_CHECK_EQ(0, sim_bus_od_set(bus, 3, 0x1A00, 0, &zero, 1));

    TEST_CHECK_EQ(0, ecrt_lib_init(NULL, NULL));
    master = ecrt_startup_master(0, sim_bus_transport(bus), NULL, 0, -1, -1);
    TEST_CHECK(master != NULL);
    if (!master) {
        goto out_cleanup;
    }

    TEST_CHECK_EQ(0, ecrt_master(master, &info));
    TEST_CHECK_EQ(NUM_SLAVES, info.slave_count);

    /* First activation: parallel configuration of five slaves, two of
     * them writing PDO mapping/assignment via their mailboxes. */
    if (!run_to_op(bus, master, &cycles1)) {
        goto out_release;
    }

    /* Both CoE slaves got their assignment written concurrently. */
    {
        size_t size;
        const uint8_t *od;

        od = sim_bus_od_data(bus, 1, 0x1C12, 0x01, &size);
        TEST_CHECK(od != NULL && size == 2 && (od[0] | (od[1] << 8)) == 0x1600);
        od = sim_bus_od_data(bus, 3, 0x1C13, 0x01, &size);
        TEST_CHECK(od != NULL && size == 2 && (od[0] | (od[1] << 8)) == 0x1A00);
    }
    downloads1a = sim_bus_od_download_count(bus, 1);
    downloads3a = sim_bus_od_download_count(bus, 3);
    TEST_CHECK(downloads1a > 0);
    TEST_CHECK(downloads3a > 0);

    /* Re-activation with identical configuration: everything must come
     * back to OP without any further SDO downloads. */
    TEST_CHECK_EQ(0, ecrt_master_deactivate(master));
    if (!run_to_op(bus, master, &cycles2)) {
        goto out_release;
    }
    downloads1b = sim_bus_od_download_count(bus, 1);
    downloads3b = sim_bus_od_download_count(bus, 3);
    TEST_CHECK_EQ(downloads1a, downloads1b);
    TEST_CHECK_EQ(downloads3a, downloads3b);

    fprintf(stderr, "test_sim_multi: first activation %u cycles,"
            " re-activation %u cycles\n", cycles1, cycles2);

out_release:
    ecrt_release_master(master);
out_cleanup:
    ecrt_lib_cleanup();
    sim_bus_destroy(bus);

    return test_done("test_sim_multi");
}
