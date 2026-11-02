/*****************************************************************************
 *
 *  Integration test: Ethernet over EtherCAT against the simulated bus.
 *
 *  A slave advertising EoE: the master creates a TAP netdevice
 *  (eoe0s0) during scan and moves Ethernet frames between the TAP and
 *  the slave's mailbox from the EoE thread (started at activation,
 *  requires ecrt_master_callbacks()). The sim echoes every received
 *  EoE frame back, so the test can verify the complete datapath: an
 *  Ethernet frame sent into the TAP interface travels
 *  TAP -> EoE thread -> mailbox fragments -> sim -> echo -> mailbox ->
 *  EoE thread -> TAP, and arrives back on a packet socket.
 *
 *  Creating a TAP device needs CAP_NET_ADMIN: without it, the test
 *  re-executes itself inside an unprivileged user+network namespace
 *  (unshare -rn), where TAP creation is permitted; if that is not
 *  available either, it skips (exit 77).
 *
 *  This file is part of the IgH EtherCAT Master.
 *
 *  The IgH EtherCAT Master is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License version 2, as
 *  published by the Free Software Foundation.
 *
 ****************************************************************************/

#include <net/if.h>
#include <netinet/in.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "transport_sim.h"

#include "ecrt_tool.h"

#include "test.h"

#define VENDOR_ID 0x00000E17
#define PRODUCT_CODE 0x5134000C

#define TAP_NAME "eoe0s0"
#define TEST_ETHERTYPE 0x88B5 /* IEEE local experimental */

#define MAX_CYCLES 15000

static const sim_slave_identity_t identities[1] = {
    { .vendor_id = VENDOR_ID, .product_code = PRODUCT_CODE,
      .revision_number = 1, .serial_number = 10001, .alias = 0,
      .sm2_phys = 0x1100, .sm2_len = 8,
      .sm3_phys = 0x1400, .sm3_len = 8,
      .mbox_out_phys = 0x1000, .mbox_out_len = 128,
      .mbox_in_phys = 0x1080, .mbox_in_len = 128,
      .eoe = 1 },
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

/* The EoE thread calls these to serialize its bus access against the
 * cyclic loop (see ecrt_master_callbacks()). */
static pthread_mutex_t io_lock = PTHREAD_MUTEX_INITIALIZER;

static void send_cb(void *master)
{
    pthread_mutex_lock(&io_lock);
    ecrt_master_send_ext((ec_master_t *) master);
    pthread_mutex_unlock(&io_lock);
}

static void receive_cb(void *master)
{
    pthread_mutex_lock(&io_lock);
    ecrt_master_receive((ec_master_t *) master);
    pthread_mutex_unlock(&io_lock);
}

static void cycle(ec_master_t *master, ec_domain_t *domain)
{
    pthread_mutex_lock(&io_lock);
    ecrt_master_receive(master);
    ecrt_domain_process(domain);
    ecrt_domain_queue(domain);
    ecrt_master_send(master);
    pthread_mutex_unlock(&io_lock);
    usleep(1000);
}

/** Bring the TAP interface up. */
static int ifup(const char *name)
{
    struct ifreq ifr;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    int ret = -1;

    if (fd < 0) {
        return -1;
    }
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", name);
    if (ioctl(fd, SIOCGIFFLAGS, &ifr) == 0) {
        ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
        ret = ioctl(fd, SIOCSIFFLAGS, &ifr);
    }
    close(fd);
    return ret;
}

int main(int argc, char **argv)
{
    sim_bus_t *bus;
    ec_master_t *master;
    ec_domain_t *domain;
    ec_slave_config_t *sc;
    ec_slave_config_state_t sc_state;
    ec_domain_state_t dstate;
    ec_tool_slave_sdo_entry_t entry;
    uint8_t *pd;
    int off_out, off_in, sock, reached;
    unsigned int cycles, ifindex, waited;
    uint8_t frame[60], rbuf[1600];
    struct sockaddr_ll sll;

    (void) argc;

    /* Without privileges for TAP creation, re-exec in an unprivileged
     * user+network namespace — but only where that actually works
     * (recent Ubuntu blocks unprivileged user namespaces via AppArmor:
     * there, unshare itself exits nonzero, which must not fail the
     * test). Probe first; without a working namespace, run directly and
     * skip when the TAP cannot be created. */
    if (!getenv("EC_TEST_EOE_WRAPPED") && geteuid() != 0
            && system("unshare -r -n true >/dev/null 2>&1") == 0) {
        setenv("EC_TEST_EOE_WRAPPED", "1", 1);
        execlp("unshare", "unshare", "-r", "-n", argv[0], (char *) NULL);
        /* exec failed: fall through. */
    }

    bus = sim_bus_create(1, identities);
    TEST_CHECK(bus != NULL);
    if (!bus) {
        return test_done("test_sim_eoe");
    }

    TEST_CHECK_EQ(0, ecrt_lib_init(NULL, NULL));
    master = ecrt_startup_master(0, sim_bus_transport(bus), NULL, 0, -1, -1);
    TEST_CHECK(master != NULL);
    if (!master) {
        goto out_cleanup;
    }

    /* The master creates the TAP device during scan; without
     * CAP_NET_ADMIN this fails and EoE is unavailable — skip. */
    ifindex = if_nametoindex(TAP_NAME);
    if (!ifindex) {
        fprintf(stderr, "test_sim_eoe: no %s interface (TAP creation"
                " needs CAP_NET_ADMIN; unshare -rn unavailable?)"
                " — skipping\n", TAP_NAME);
        ecrt_release_master(master);
        ecrt_lib_cleanup();
        sim_bus_destroy(bus);
        return 77;
    }

    /* Wait for the automatic SDO dictionary fetch to finish so its
     * mailbox traffic does not interleave with the EoE test. */
    for (waited = 0; waited < 15000; waited += 100) {
        memset(&entry, 0, sizeof(entry));
        entry.slave_position = 0;
        entry.sdo_spec = 0x1C13;
        entry.sdo_entry_subindex = 0;
        if (ecrt_tool_get_slave_sdo_entry(master, &entry) == 0) {
            break;
        }
        usleep(100000);
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

    /* EoE needs the callbacks before activation. */
    ecrt_master_callbacks(master, send_cb, receive_cb, master);

    TEST_CHECK_EQ(0, ecrt_master_activate(master));
    pd = ecrt_domain_data(domain);
    TEST_CHECK(pd != NULL);
    if (!pd) {
        goto out_release;
    }

    reached = 0;
    for (cycles = 0; cycles < MAX_CYCLES; cycles++) {
        cycle(master, domain);
        ecrt_slave_config_state(sc, &sc_state);
        ecrt_domain_state(domain, &dstate);
        if (sc_state.operational && dstate.wc_state == EC_WC_COMPLETE) {
            reached = 1;
            break;
        }
    }
    TEST_CHECK(reached);

    /* Open a packet socket on the TAP interface and send a test frame
     * into it; the master's EoE thread forwards it to the (echoing)
     * slave and writes the echo back to the TAP. */
    TEST_CHECK_EQ(0, ifup(TAP_NAME));
    sock = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    TEST_CHECK(sock >= 0);
    memset(&sll, 0, sizeof(sll));
    sll.sll_family = AF_PACKET;
    sll.sll_ifindex = (int) ifindex;
    sll.sll_protocol = htons(ETH_P_ALL);
    TEST_CHECK_EQ(0, bind(sock, (struct sockaddr *) &sll, sizeof(sll)));

    memset(frame, 0, sizeof(frame));
    memset(frame, 0xFF, 6); /* broadcast */
    frame[6] = 0x02; frame[11] = 0x01; /* locally administered source */
    frame[12] = TEST_ETHERTYPE >> 8;
    frame[13] = TEST_ETHERTYPE & 0xFF;
    memcpy(frame + 14, "EoE-echo-test-payload", 21);

    TEST_CHECK_EQ((int) sizeof(frame),
            (int) sendto(sock, frame, sizeof(frame), 0,
                (struct sockaddr *) &sll, sizeof(sll)));

    /* Keep cycling; wait for the echoed frame (ignore our own outgoing
     * copy and unrelated traffic like IPv6 solicitations). */
    reached = 0;
    for (cycles = 0; cycles < MAX_CYCLES && !reached; cycles++) {
        struct sockaddr_ll from;
        socklen_t fromlen = sizeof(from);
        ssize_t n;

        cycle(master, domain);
        while ((n = recvfrom(sock, rbuf, sizeof(rbuf), MSG_DONTWAIT,
                        (struct sockaddr *) &from, &fromlen)) > 0) {
            if (from.sll_pkttype != PACKET_OUTGOING && n >= 35
                    && rbuf[12] == (TEST_ETHERTYPE >> 8)
                    && rbuf[13] == (TEST_ETHERTYPE & 0xFF)
                    && !memcmp(rbuf + 14, "EoE-echo-test-payload", 21)) {
                reached = 1;
                break;
            }
            fromlen = sizeof(from);
        }
    }
    TEST_CHECK(reached);

    close(sock);

out_release:
    ecrt_release_master(master);
out_cleanup:
    ecrt_lib_cleanup();
    sim_bus_destroy(bus);

    return test_done("test_sim_eoe");
}
