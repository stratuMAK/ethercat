/*****************************************************************************
 *
 *  Integration test: cyclic domain exchange against the simulated bus.
 *
 *  Configures a slave with explicit PDO mapping (one 16-bit output, one
 *  16-bit input entry), activates the master and runs the real cyclic
 *  path (receive/process/queue/send) until the slave reaches OP and the
 *  domain working counter is complete. Then verifies the process data
 *  round-trip in both directions through the FMMU emulation: an output
 *  value written by the application arrives in the slave's SM2 memory,
 *  and an input value poked into SM3 memory arrives in the domain.
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
#define PRODUCT_CODE 0x51340001

#define MAX_CYCLES 5000

static const sim_slave_identity_t identities[1] = {
    { .vendor_id = VENDOR_ID, .product_code = PRODUCT_CODE,
      .revision_number = 1, .serial_number = 1, .alias = 0,
      .sm2_phys = SM2_PHYS, .sm2_len = 8,
      .sm3_phys = SM3_PHYS, .sm3_len = 8 },
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

/** One application cycle against the sim (the sim answers a frame on
 * the receive following its send, so pace the loop briefly).
 *
 * Output data must be written between process and queue: receive()
 * copies the echoed datagram — which carries the PREVIOUS cycle's
 * output data — back into the domain memory, so values written before
 * receive() are overwritten, exactly as on a real bus. \a out_value
 * (if non-negative) is written to \a out_offset each cycle. */
static void cycle(ec_master_t *master, ec_domain_t *domain,
        uint8_t *pd, int out_offset, long out_value)
{
    ecrt_master_receive(master);
    ecrt_domain_process(domain);
    if (out_value >= 0) {
        EC_WRITE_U16(pd + out_offset, (uint16_t) out_value);
    }
    ecrt_domain_queue(domain);
    ecrt_master_send(master);
    usleep(1000);
}

int main(void)
{
    sim_bus_t *bus;
    ec_master_t *master;
    ec_domain_t *domain;
    ec_slave_config_t *sc;
    ec_slave_config_state_t sc_state;
    ec_domain_state_t dstate;
    uint8_t *pd, *slave_regs;
    int off_out, off_in;
    unsigned int cycles;

    bus = sim_bus_create(1, identities);
    TEST_CHECK(bus != NULL);
    if (!bus) {
        return test_done("test_sim_domain");
    }

    TEST_CHECK_EQ(0, ecrt_lib_init(NULL, NULL));
    master = ecrt_startup_master(0, sim_bus_transport(bus), NULL, 0, -1, -1);
    TEST_CHECK(master != NULL);
    if (!master) {
        goto out_cleanup;
    }

    domain = ecrt_master_create_domain(master);
    TEST_CHECK(domain != NULL);
    sc = ecrt_master_slave_config(master, 0, 0, VENDOR_ID, PRODUCT_CODE);
    TEST_CHECK(sc != NULL);
    if (!domain || !sc) {
        goto out_release;
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
        goto out_release;
    }

    /* Run the cyclic path until the slave is operational and the domain
     * working counter is complete (1 read + 2 write = 3 for LRW, or
     * separate LRD/LWR datagrams summing to 2 — accept EC_WC_COMPLETE). */
    for (cycles = 0; cycles < MAX_CYCLES; cycles++) {
        cycle(master, domain, pd, 0, -1);
        ecrt_slave_config_state(sc, &sc_state);
        ecrt_domain_state(domain, &dstate);
        if (sc_state.operational && dstate.wc_state == EC_WC_COMPLETE) {
            break;
        }
    }
    TEST_CHECK(cycles < MAX_CYCLES);
    TEST_CHECK_EQ(1, sc_state.online);
    TEST_CHECK_EQ(1, sc_state.operational);
    TEST_CHECK_EQ(0x08, sc_state.al_state); /* OP */
    TEST_CHECK_EQ(8, sim_bus_slave_al_state(bus, 0));
    TEST_CHECK_EQ(EC_WC_COMPLETE, dstate.wc_state);

    slave_regs = sim_bus_slave_regs(bus, 0);
    TEST_CHECK(slave_regs != NULL);

    /* Output direction: application -> slave SM2 memory. */
    for (cycles = 0; cycles < 5; cycles++) {
        cycle(master, domain, pd, off_out, 0xCAFE);
    }
    TEST_CHECK_EQ(0xCAFE, EC_READ_U16(slave_regs + SM2_PHYS));

    /* Input direction: slave SM3 memory -> application. */
    EC_WRITE_U16(slave_regs + SM3_PHYS, 0xBEEF);
    for (cycles = 0; cycles < 5; cycles++) {
        cycle(master, domain, pd, off_out, 0xCAFE);
    }
    TEST_CHECK_EQ(0xBEEF, EC_READ_U16(pd + off_in));
    /* The output value must survive the round-trip echo as well. */
    TEST_CHECK_EQ(0xCAFE, EC_READ_U16(pd + off_out));

    /* The domain must still be exchanging with a complete WC. */
    ecrt_domain_state(domain, &dstate);
    TEST_CHECK_EQ(EC_WC_COMPLETE, dstate.wc_state);

out_release:
    ecrt_release_master(master);
out_cleanup:
    ecrt_lib_cleanup();
    sim_bus_destroy(bus);

    return test_done("test_sim_domain");
}
