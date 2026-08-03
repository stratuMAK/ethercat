/*****************************************************************************
 *
 *  RT-correctness test: zero page faults in the cyclic path.
 *
 *  Runs the real cyclic path (receive/process/queue/send with domain
 *  data) against the simulated bus with the process memory locked, and
 *  asserts that after a warm-up phase the application (RT) thread takes
 *  ZERO minor and major page faults across the measured cycles.
 *
 *  This is deliberately NOT a latency test: page faults in the cyclic
 *  path are an environment-independent correctness property — if a
 *  per-cycle allocation creeps into the path, its fresh pages fault on
 *  first touch and this fails deterministically on any machine, without
 *  an RT kernel or tuned hardware. Latency itself is validated on the
 *  real system (see docs/testing/rt-system-test.md).
 *
 *  mlockall() is attempted (after raising RLIMIT_MEMLOCK to the hard
 *  limit) but not required: with the default 8 MiB limit the library's
 *  thread stacks alone exceed it — a deployment note in its own right,
 *  see docs/testing/rt-system-test.md. Without the lock the assertion still holds
 *  for the allocation-regression class; the warm-up faults in every
 *  page the cyclic path touches.
 *
 *  This file is part of the IgH EtherCAT Master.
 *
 *  The IgH EtherCAT Master is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License version 2, as
 *  published by the Free Software Foundation.
 *
 ****************************************************************************/

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>

#include "transport_sim.h"

#include "test.h"

#define SM2_PHYS 0x1100
#define SM3_PHYS 0x1400

#define VENDOR_ID 0x00000E17
#define PRODUCT_CODE 0x5134000B

#define MAX_CYCLES 5000
#define WARMUP_CYCLES 2000
#define MEASURED_CYCLES 1000

/* Sanitizer runtimes (ASan shadow memory, TSan state) demand-page
 * their own mappings during the measured window, so the zero-fault
 * assertion is meaningless under them — skip. */
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define EC_TEST_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define EC_TEST_SANITIZED 1
#endif
#endif

static const sim_slave_identity_t identities[1] = {
    { .vendor_id = VENDOR_ID, .product_code = PRODUCT_CODE,
      .revision_number = 1, .serial_number = 9001, .alias = 0,
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

/** Read this thread's minor/major fault counters (fields 10/12 of
 * /proc/thread-self/stat, counted after the parenthesized comm). */
static int read_faults(unsigned long *minflt, unsigned long *majflt)
{
    char buf[512];
    ssize_t n;
    int fd;
    char *p;

    fd = open("/proc/thread-self/stat", O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) {
        return -1;
    }
    buf[n] = '\0';

    p = strrchr(buf, ')'); /* comm may contain spaces */
    if (!p) {
        return -1;
    }
    /* p points at ")"; fields state=3, ..., minflt=10, ..., majflt=12 */
    if (sscanf(p + 1, " %*c %*d %*d %*d %*d %*d %*u %lu %*u %lu",
                minflt, majflt) != 2) {
        return -1;
    }
    return 0;
}

/** Prefault this thread's stack beyond anything the cyclic path uses. */
static void stack_prefault(void)
{
    unsigned char dummy[64 * 1024];

    memset(dummy, 0, sizeof(dummy));
}

static void cycle(ec_master_t *master, ec_domain_t *domain,
        uint8_t *pd, int off_out, uint16_t value)
{
    ecrt_master_receive(master);
    ecrt_domain_process(domain);
    EC_WRITE_U16(pd + off_out, value);
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
    uint8_t *pd;
    int off_out, off_in;
    unsigned int cycles;
    unsigned long minflt_a, majflt_a, minflt_b, majflt_b;

#ifdef EC_TEST_SANITIZED
    fprintf(stderr, "test_rt_pagefault: sanitizer build — fault"
            " accounting is not meaningful; skipping\n");
    return 77;
#endif

    bus = sim_bus_create(1, identities);
    TEST_CHECK(bus != NULL);
    if (!bus) {
        return test_done("test_rt_pagefault");
    }

    TEST_CHECK_EQ(0, ecrt_lib_init(NULL, NULL));
    master = ecrt_startup_master(0, sim_bus_transport(bus), NULL, 0, -1);
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
    TEST_CHECK_EQ(0, ecrt_master_activate(master));
    pd = ecrt_domain_data(domain);
    TEST_CHECK(pd != NULL);
    if (!pd) {
        goto out_release;
    }

    /* Lock memory if the limits allow it (with all library threads
     * already created, MCL_CURRENT covers their stacks — which is also
     * why this commonly fails under the default 8 MiB RLIMIT_MEMLOCK). */
    {
        struct rlimit rl;

        if (getrlimit(RLIMIT_MEMLOCK, &rl) == 0
                && rl.rlim_cur < rl.rlim_max) {
            rl.rlim_cur = rl.rlim_max;
            setrlimit(RLIMIT_MEMLOCK, &rl);
        }
        if (mlockall(MCL_CURRENT | MCL_FUTURE)) {
            fprintf(stderr, "test_rt_pagefault: running without mlockall"
                    " (%s; RLIMIT_MEMLOCK too low)\n", strerror(errno));
        } else {
            fprintf(stderr, "test_rt_pagefault: memory locked\n");
        }
    }

    /* Reach OP, then keep cycling through the warm-up phase so all
     * lazily-touched pages (frame paths, statistics, FSM settling)
     * are faulted in. */
    for (cycles = 0; cycles < MAX_CYCLES; cycles++) {
        cycle(master, domain, pd, off_out, 0x0001);
        ecrt_slave_config_state(sc, &sc_state);
        ecrt_domain_state(domain, &dstate);
        if (sc_state.operational && dstate.wc_state == EC_WC_COMPLETE) {
            break;
        }
    }
    TEST_CHECK(cycles < MAX_CYCLES);

    for (cycles = 0; cycles < WARMUP_CYCLES; cycles++) {
        cycle(master, domain, pd, off_out, (uint16_t) cycles);
    }

    stack_prefault();

    /* Measured phase: the cyclic path itself must take no faults.
     * Without mlockall the environment can inject an occasional stray
     * minor fault, so measure up to five windows and require at least
     * one with a zero delta: ambient strays are absent from most
     * windows, while a genuine fault source in the cyclic path faults
     * in EVERY window. Major faults are never acceptable. */
    {
        unsigned long best_minflt = (unsigned long) -1;
        unsigned long total_majflt = 0;
        unsigned int window;

        for (window = 0; window < 5; window++) {
            TEST_CHECK_EQ(0, read_faults(&minflt_a, &majflt_a));
            for (cycles = 0; cycles < MEASURED_CYCLES; cycles++) {
                cycle(master, domain, pd, off_out, (uint16_t) cycles);
            }
            TEST_CHECK_EQ(0, read_faults(&minflt_b, &majflt_b));

            total_majflt += majflt_b - majflt_a;
            if (minflt_b - minflt_a < best_minflt) {
                best_minflt = minflt_b - minflt_a;
            }
            if (best_minflt == 0) {
                break;
            }
        }
        if (best_minflt != 0) {
            fprintf(stderr, "test_rt_pagefault: %lu minor fault(s) in"
                    " every window — cyclic path touches new pages!\n",
                    best_minflt);
        }
        TEST_CHECK_EQ(0, best_minflt);
        TEST_CHECK_EQ(0, total_majflt);
    }

    /* The exchange must still have been live during the measurement. */
    ecrt_domain_state(domain, &dstate);
    TEST_CHECK_EQ(EC_WC_COMPLETE, dstate.wc_state);
    TEST_CHECK_EQ((uint16_t) (MEASURED_CYCLES - 1),
            EC_READ_U16(sim_bus_slave_regs(bus, 0) + SM2_PHYS));
    (void) off_in;

out_release:
    ecrt_release_master(master);
out_cleanup:
    ecrt_lib_cleanup();
    sim_bus_destroy(bus);

    return test_done("test_rt_pagefault");
}
