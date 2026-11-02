/*****************************************************************************
 *
 *  Integration test: CoE-based PDO mapping/assignment against the sim.
 *
 *  A slave with both a CoE mailbox and process-data sync managers whose
 *  SII advertises enable_pdo_assign/enable_pdo_configuration: during
 *  PREOP->SAFEOP the master writes the PDO mapping (0x1600/0x1A00) and
 *  assignment (0x1C12/0x1C13) objects via SDO. The test verifies the
 *  written object values in the sim's dictionary, that the slave still
 *  reaches OP with working process data, and — regression for commit
 *  f381059c — that re-activating with an unchanged configuration issues
 *  NO further SDO downloads (mapping/assignment already match).
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
#define PRODUCT_CODE 0x51340003

#define MAX_CYCLES 5000

static const sim_slave_identity_t identities[1] = {
    { .vendor_id = VENDOR_ID, .product_code = PRODUCT_CODE,
      .revision_number = 1, .serial_number = 3, .alias = 0,
      .sm2_phys = SM2_PHYS, .sm2_len = 8,
      .sm3_phys = SM3_PHYS, .sm3_len = 8,
      .mbox_out_phys = 0x1000, .mbox_out_len = 128,
      .mbox_in_phys = 0x1080, .mbox_in_len = 128 },
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

/** Check a dictionary object against an expected little-endian value. */
static void check_od_u(sim_bus_t *bus, uint16_t index, uint8_t subindex,
        size_t size, uint32_t expected)
{
    size_t od_size = 0;
    const uint8_t *od = sim_bus_od_data(bus, 0, index, subindex, &od_size);

    TEST_CHECK(od != NULL);
    if (od) {
        uint32_t value = 0;
        unsigned int i;

        TEST_CHECK_EQ(size, od_size);
        for (i = 0; i < od_size && i < 4; i++) {
            value |= (uint32_t) od[i] << (8 * i);
        }
        TEST_CHECK_EQ(expected, value);
    }
}

/** Configure, activate and run to OP + complete WC.
 * Returns 1 on success. */
static int run_to_op(sim_bus_t *bus, ec_master_t *master)
{
    ec_domain_t *domain;
    ec_slave_config_t *sc;
    ec_slave_config_state_t sc_state;
    ec_domain_state_t dstate;
    uint8_t *pd;
    int off_out, off_in;
    unsigned int cycles, ok_cycles = 0;

    domain = ecrt_master_create_domain(master);
    sc = ecrt_master_slave_config(master, 0, 0, VENDOR_ID, PRODUCT_CODE);
    TEST_CHECK(domain != NULL);
    TEST_CHECK(sc != NULL);
    if (!domain || !sc) {
        return 0;
    }

    TEST_CHECK_EQ(0, ecrt_slave_config_pdos(sc, EC_END, syncs));
    off_out = ecrt_slave_config_reg_pdo_entry(sc, 0x7000, 0x01, domain, NULL);
    off_in = ecrt_slave_config_reg_pdo_entry(sc, 0x6000, 0x01, domain, NULL);
    TEST_CHECK(off_out >= 0);
    TEST_CHECK(off_in >= 0);

    TEST_CHECK_EQ(0, ecrt_master_activate(master));
    pd = ecrt_domain_data(domain);
    TEST_CHECK(pd != NULL);
    if (!pd) {
        return 0;
    }

    for (cycles = 0; cycles < MAX_CYCLES; cycles++) {
        ecrt_master_receive(master);
        ecrt_domain_process(domain);
        EC_WRITE_U16(pd + off_out, 0x55AA);
        ecrt_domain_queue(domain);
        ecrt_master_send(master);
        usleep(1000);

        ecrt_slave_config_state(sc, &sc_state);
        ecrt_domain_state(domain, &dstate);
        if (sc_state.operational && dstate.wc_state == EC_WC_COMPLETE) {
            /* run a few extra cycles so the output propagates */
            if (++ok_cycles >= 3) {
                break;
            }
        }
    }
    TEST_CHECK(cycles < MAX_CYCLES);
    TEST_CHECK_EQ(8, sim_bus_slave_al_state(bus, 0));
    TEST_CHECK_EQ(0x55AA,
            EC_READ_U16(sim_bus_slave_regs(bus, 0) + SM2_PHYS));

    return cycles < MAX_CYCLES;
}

int main(void)
{
    sim_bus_t *bus;
    ec_master_t *master;
    unsigned long downloads_first, downloads_second;
    uint8_t zero = 0;

    bus = sim_bus_create(1, identities);
    TEST_CHECK(bus != NULL);
    if (!bus) {
        return test_done("test_sim_pdo_assign");
    }

    /* The slave powers up with empty, writable mapping objects. */
    TEST_CHECK_EQ(0, sim_bus_od_set(bus, 0, 0x1600, 0, &zero, 1));
    TEST_CHECK_EQ(0, sim_bus_od_set(bus, 0, 0x1A00, 0, &zero, 1));

    TEST_CHECK_EQ(0, ecrt_lib_init(NULL, NULL));
    master = ecrt_startup_master(0, sim_bus_transport(bus), NULL, 0, -1, -1);
    TEST_CHECK(master != NULL);
    if (!master) {
        goto out_cleanup;
    }

    /* First activation: mapping and assignment differ from the slave's
     * (empty) state, so the master must write them via SDO. */
    if (!run_to_op(bus, master)) {
        goto out_release;
    }

    check_od_u(bus, 0x1600, 0x00, 1, 1); /* one mapped entry */
    check_od_u(bus, 0x1600, 0x01, 4, 0x70000110); /* 0x7000:01, 16 bit */
    check_od_u(bus, 0x1A00, 0x00, 1, 1);
    check_od_u(bus, 0x1A00, 0x01, 4, 0x60000110); /* 0x6000:01, 16 bit */
    check_od_u(bus, 0x1C12, 0x00, 1, 1); /* one assigned PDO */
    check_od_u(bus, 0x1C12, 0x01, 2, 0x1600);
    check_od_u(bus, 0x1C13, 0x00, 1, 1);
    check_od_u(bus, 0x1C13, 0x01, 2, 0x1A00);

    downloads_first = sim_bus_od_download_count(bus, 0);
    TEST_CHECK(downloads_first > 0);

    /* Re-activation with identical configuration: mapping/assignment
     * now match the slave, so no SDO downloads may happen (f381059c). */
    TEST_CHECK_EQ(0, ecrt_master_deactivate(master));
    if (!run_to_op(bus, master)) {
        goto out_release;
    }

    downloads_second = sim_bus_od_download_count(bus, 0);
    TEST_CHECK_EQ(downloads_first, downloads_second);

out_release:
    ecrt_release_master(master);
out_cleanup:
    ecrt_lib_cleanup();
    sim_bus_destroy(bus);

    return test_done("test_sim_pdo_assign");
}
