/*****************************************************************************
 *
 *  Integration test: distributed clocks against the simulated bus.
 *
 *  Two DC-capable slaves (the first also carries process data). Verifies
 *  that the master selects a reference clock, measures transmission
 *  delays from the synthetic port receive times, programs the DC
 *  registers configured via ecrt_slave_config_dc() (AssignActivate
 *  0x0980, cycle time 0x09A0), and that the cyclic DC path works: the
 *  application time written via ecrt_master_sync_reference_clock_to()
 *  reaches the reference clock's system time register (0x0910) and is
 *  propagated to the other slave by the FRMW sync datagram;
 *  ecrt_master_reference_clock_time() returns it (minus the ref clock's
 *  transmission delay) and the sync monitor reports the system time
 *  difference register (0x092C).
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
#define PRODUCT_CODE 0x51340005

#define MAX_CYCLES 5000

/** EtherCAT epoch-ish application time base (2000-01-01 in ns). */
#define APP_TIME_BASE 0x0102030405060708ULL

static const sim_slave_identity_t identities[2] = {
    { .vendor_id = VENDOR_ID, .product_code = PRODUCT_CODE,
      .revision_number = 1, .serial_number = 5001, .alias = 0,
      .sm2_phys = SM2_PHYS, .sm2_len = 8,
      .sm3_phys = SM3_PHYS, .sm3_len = 8,
      .dc_supported = 1 },
    { .vendor_id = VENDOR_ID, .product_code = PRODUCT_CODE + 1,
      .revision_number = 1, .serial_number = 5002, .alias = 0,
      .dc_supported = 1 },
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

int main(void)
{
    sim_bus_t *bus;
    ec_master_t *master;
    ec_domain_t *domain;
    ec_slave_config_t *sc;
    ec_slave_config_state_t sc_state;
    ec_domain_state_t dstate;
    uint8_t *pd;
    uint8_t *ref_regs, *other_regs;
    int off_out, off_in;
    unsigned int cycles;
    int reached;
    uint64_t app_time = APP_TIME_BASE;
    uint32_t ref_time, sync_mon;

    bus = sim_bus_create(2, identities);
    TEST_CHECK(bus != NULL);
    if (!bus) {
        return test_done("test_sim_dc");
    }

    TEST_CHECK_EQ(0, ecrt_lib_init(NULL, NULL));
    master = ecrt_startup_master(0, sim_bus_transport(bus), NULL, 0, -1, -1);
    TEST_CHECK(master != NULL);
    if (!master) {
        goto out_cleanup;
    }

    domain = ecrt_master_create_domain(master);
    sc = ecrt_master_slave_config(master, 0, 0, VENDOR_ID, PRODUCT_CODE);
    TEST_CHECK(domain != NULL);
    TEST_CHECK(sc != NULL);
    if (!domain || !sc) {
        goto out_release;
    }

    TEST_CHECK_EQ(0, ecrt_slave_config_pdos(sc, EC_END, syncs));
    off_out = ecrt_slave_config_reg_pdo_entry(sc, 0x7000, 0x01, domain, NULL);
    off_in = ecrt_slave_config_reg_pdo_entry(sc, 0x6000, 0x01, domain, NULL);
    TEST_CHECK(off_out >= 0);
    TEST_CHECK(off_in >= 0);

    /* SYNC0 generation, 1 ms cycle. */
    TEST_CHECK_EQ(0, ecrt_slave_config_dc(sc, 0x0300, 1000000, 0, 0, 0));

    TEST_CHECK_EQ(0, ecrt_master_activate(master));
    pd = ecrt_domain_data(domain);
    TEST_CHECK(pd != NULL);
    if (!pd) {
        goto out_release;
    }

    /* Cyclic loop with the full DC call set. */
    reached = 0;
    for (cycles = 0; cycles < MAX_CYCLES; cycles++) {
        ecrt_master_receive(master);
        ecrt_domain_process(domain);
        sync_mon = ecrt_master_sync_monitor_process(master);

        EC_WRITE_U16(pd + off_out, 0x0DC0);
        app_time += 1000000;
        ecrt_master_application_time(master, app_time);
        ecrt_master_sync_reference_clock_to(master, app_time);
        ecrt_master_sync_slave_clocks(master);
        ecrt_master_sync_monitor_queue(master);

        ecrt_domain_queue(domain);
        ecrt_master_send(master);
        usleep(1000);

        ecrt_slave_config_state(sc, &sc_state);
        ecrt_domain_state(domain, &dstate);
        if (sc_state.operational && dstate.wc_state == EC_WC_COMPLETE) {
            if (++reached >= 5) { /* extra cycles: DC datagrams settle */
                break;
            }
        }
    }
    TEST_CHECK(reached >= 5);
    (void) sync_mon;

    /* The loop ends right after send(): receive the last frame so the
     * sync datagram is in RECEIVED state before querying it. */
    usleep(2000);
    ecrt_master_receive(master);
    ecrt_domain_process(domain);

    ref_regs = sim_bus_slave_regs(bus, 0);
    other_regs = sim_bus_slave_regs(bus, 1);
    TEST_CHECK(ref_regs != NULL && other_regs != NULL);

    /* DC configuration written during slave configuration. */
    TEST_CHECK_EQ(0x0300, EC_READ_U16(ref_regs + 0x0980));
    TEST_CHECK_EQ(1000000, EC_READ_U32(ref_regs + 0x09A0));

    /* The reference clock (first DC slave) received the application
     * time; the FRMW sync datagram propagated it to the other slave in
     * the same frame. */
    TEST_CHECK_EQ((uint32_t) app_time, EC_READ_U32(ref_regs + 0x0910));
    TEST_CHECK_EQ((uint32_t) app_time, EC_READ_U32(other_regs + 0x0910));

    /* The master measured transmission delays from the synthetic port
     * times and wrote them (together with the system time offset, via
     * the same 12-byte FPWR to 0x0920) to both DC slaves: the reference
     * clock has no delay, the second slave is one 100 ns hop away. */
    TEST_CHECK_EQ(0, EC_READ_U32(ref_regs + 0x0928)); /* ref: no delay */
    TEST_CHECK_EQ(100, EC_READ_U32(other_regs + 0x0928));

    /* Reference clock time as seen by the application. */
    TEST_CHECK_EQ(0, ecrt_master_reference_clock_time(master, &ref_time));
    TEST_CHECK((uint32_t) (app_time - ref_time) < 1000000);

    /* Sync monitoring: the BRD of 0x092C ORs the per-slave system time
     * difference registers. */
    EC_WRITE_U32(other_regs + 0x092C, 0x1234);
    for (cycles = 0; cycles < 5; cycles++) {
        ecrt_master_receive(master);
        ecrt_domain_process(domain);
        sync_mon = ecrt_master_sync_monitor_process(master);
        app_time += 1000000;
        ecrt_master_application_time(master, app_time);
        ecrt_master_sync_reference_clock_to(master, app_time);
        ecrt_master_sync_slave_clocks(master);
        ecrt_master_sync_monitor_queue(master);
        ecrt_domain_queue(domain);
        ecrt_master_send(master);
        usleep(1000);
    }
    TEST_CHECK_EQ(0x1234, sync_mon);

    /* Process data still flows alongside the DC datagrams. */
    TEST_CHECK_EQ(0x0DC0, EC_READ_U16(ref_regs + SM2_PHYS));

out_release:
    ecrt_release_master(master);
out_cleanup:
    ecrt_lib_cleanup();
    sim_bus_destroy(bus);

    return test_done("test_sim_dc");
}
