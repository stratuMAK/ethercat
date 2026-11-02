/*****************************************************************************
 *
 *  Integration test (T3): the ethercat command-line tool against the
 *  IPC server, end to end.
 *
 *  Starts an in-process master with the Unix-socket IPC server enabled
 *  on a simulated bus, then runs the real `ethercat` binary (path from
 *  the EC_TOOL_BIN environment variable, set by the Makefile) against
 *  the socket and asserts on its output: master status, slave listing,
 *  SDO upload/download round trip through the simulated mailbox, and
 *  the SDO dictionary listing served by the SDO Information Service.
 *
 *  Skipped (exit 77) if the tool binary is not available.
 *
 *  This file is part of the IgH EtherCAT Master.
 *
 *  The IgH EtherCAT Master is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License version 2, as
 *  published by the Free Software Foundation.
 *
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "transport_sim.h"

#include "ecrt_tool.h"

#include "test.h"

#define VENDOR_ID 0x00000E17
#define PRODUCT_CODE 0x5134000A

static const sim_slave_identity_t identities[1] = {
    { .vendor_id = VENDOR_ID, .product_code = PRODUCT_CODE,
      .revision_number = 1, .serial_number = 8001, .alias = 0,
      .mbox_out_phys = 0x1000, .mbox_out_len = 128,
      .mbox_in_phys = 0x1080, .mbox_in_len = 128 },
};

static const char *tool_bin;
static char sock_path[128];

/** Run the tool with \a args; capture combined output into \a out.
 * Returns the tool's exit status (or -1). */
static int run_tool(const char *args, char *out, size_t out_size)
{
    char cmd[512];
    FILE *p;
    size_t used = 0, n;
    int status;

    /* Quote the interpolated paths so a TMPDIR/builddir with spaces
     * fails loudly in the tool instead of confusing the shell. */
    snprintf(cmd, sizeof(cmd), "\"%s\" --socket \"%s\" %s 2>&1",
            tool_bin, sock_path, args);
    p = popen(cmd, "r");
    if (!p) {
        return -1;
    }
    while (used + 1 < out_size
            && (n = fread(out + used, 1, out_size - used - 1, p)) > 0) {
        used += n;
    }
    out[used] = '\0';
    status = pclose(p);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int main(void)
{
    sim_bus_t *bus;
    ec_master_t *master;
    ec_slave_info_t slave;
    ec_tool_slave_sdo_entry_t entry;
    char out[8192];
    uint8_t buf[8];
    unsigned int waited;
    size_t od_size;
    const uint8_t *od;

    /* Watchdog: if the tool or the IPC server wedges, fail the test
     * after two minutes instead of hanging a CI runner (the tool's
     * fread() on the pipe has no timeout of its own). */
    alarm(120);

    tool_bin = getenv("EC_TOOL_BIN");
    if (!tool_bin || access(tool_bin, X_OK)) {
        fprintf(stderr, "test_tool_ipc: ethercat tool not available"
                " (EC_TOOL_BIN=%s) — skipping\n",
                tool_bin ? tool_bin : "(unset)");
        return 77;
    }

    snprintf(sock_path, sizeof(sock_path), "%s/ectest-%d.sock",
            getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int) getpid());

    bus = sim_bus_create(1, identities);
    TEST_CHECK(bus != NULL);
    if (!bus) {
        return test_done("test_tool_ipc");
    }

    EC_WRITE_U32(buf, 0xDEADBEEF);
    TEST_CHECK_EQ(0, sim_bus_od_set(bus, 0, 0x2000, 0, buf, 4));

    TEST_CHECK_EQ(0, ecrt_lib_init(NULL, sock_path));
    master = ecrt_startup_master(0, sim_bus_transport(bus), NULL, 0, -1, -1);
    TEST_CHECK(master != NULL);
    if (!master) {
        goto out_cleanup;
    }

    /* Wait until the idle phase brought the slave to PREOP (mailbox
     * usable, state visible in the slave listing). */
    for (waited = 0; waited < 10000; waited += 100) {
        TEST_CHECK_EQ(0, ecrt_master_get_slave(master, 0, &slave));
        if (slave.al_state == 0x02) {
            break;
        }
        usleep(100000);
    }
    TEST_CHECK_EQ(0x02, slave.al_state);

    /* Master status. */
    TEST_CHECK_EQ(0, run_tool("master", out, sizeof(out)));
    TEST_CHECK(strstr(out, "Phase:") != NULL);
    TEST_CHECK(strstr(out, "Slaves: 1") != NULL);

    /* Slave listing: poll — the displayed state can lag the API state
     * for a moment around scan/state transitions. */
    for (waited = 0; waited < 5000; waited += 100) {
        TEST_CHECK_EQ(0, run_tool("slaves", out, sizeof(out)));
        if (strstr(out, "PREOP")) {
            break;
        }
        usleep(100000);
    }
    TEST_CHECK(strstr(out, "PREOP") != NULL);
    if (!strstr(out, "PREOP")) {
        fprintf(stderr, "SLAVES-OUT: [%s]\n", out);
    }

    /* SDO upload of the preset object through the simulated mailbox. */
    TEST_CHECK_EQ(0, run_tool("upload -p0 --type uint32 0x2000 0x00",
                out, sizeof(out)));
    TEST_CHECK(strstr(out, "3735928559") != NULL); /* 0xDEADBEEF */

    /* SDO download; verify in the sim's dictionary and read back. */
    TEST_CHECK_EQ(0, run_tool("download -p0 --type uint32 0x2000 0x00"
                " 0x11223344", out, sizeof(out)));
    od = sim_bus_od_data(bus, 0, 0x2000, 0, &od_size);
    TEST_CHECK(od != NULL && od_size == 4);
    if (od) {
        TEST_CHECK_EQ(0x11223344u, EC_READ_U32(od));
    }
    TEST_CHECK_EQ(0, run_tool("upload -p0 --type uint32 0x2000 0x00",
                out, sizeof(out)));
    TEST_CHECK(strstr(out, "287454020") != NULL); /* 0x11223344 */

    /* SDO dictionary listing (SDO Information Service): wait for the
     * automatic fetch to complete (last entry cached), then list. */
    for (waited = 0; waited < 15000; waited += 100) {
        memset(&entry, 0, sizeof(entry));
        entry.slave_position = 0;
        entry.sdo_spec = 0x2000;
        entry.sdo_entry_subindex = 0;
        if (ecrt_tool_get_slave_sdo_entry(master, &entry) == 0) {
            break;
        }
        usleep(100000);
    }
    TEST_CHECK_EQ(0, run_tool("sdos -p0", out, sizeof(out)));
    TEST_CHECK(strstr(out, "SimObj2000") != NULL);
    TEST_CHECK(strstr(out, "Entry00") != NULL);

    /* Unknown object: the tool must fail with the abort message. */
    TEST_CHECK(run_tool("upload -p0 --type uint32 0x5555 0x00",
                out, sizeof(out)) != 0);

    ecrt_release_master(master);
out_cleanup:
    ecrt_lib_cleanup();
    sim_bus_destroy(bus);
    unlink(sock_path);

    return test_done("test_tool_ipc");
}
