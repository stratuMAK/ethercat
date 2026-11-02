/*****************************************************************************
 *
 *  Integration test: CoE SDO transfers against the simulated bus.
 *
 *  Starts a master on a bus with one mailbox-capable (CoE) slave and
 *  exercises the blocking SDO API through the real mailbox machinery
 *  (mailbox write, SM1 status polling via FPRD 0x808, mailbox fetch):
 *  expedited upload/download, normal (>4 byte) upload/download, and
 *  the abort path for a nonexistent object.
 *
 *  This file is part of the IgH EtherCAT Master.
 *
 *  The IgH EtherCAT Master is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License version 2, as
 *  published by the Free Software Foundation.
 *
 ****************************************************************************/

#include <string.h>

#include "transport_sim.h"

#include "test.h"

#define VENDOR_ID 0x00000E17
#define PRODUCT_CODE 0x51340002

static const sim_slave_identity_t identities[1] = {
    { .vendor_id = VENDOR_ID, .product_code = PRODUCT_CODE,
      .revision_number = 1, .serial_number = 2, .alias = 0,
      .mbox_out_phys = 0x1000, .mbox_out_len = 128,
      .mbox_in_phys = 0x1080, .mbox_in_len = 128 },
};

static const char test_string[] = "hello EtherCAT mailbox";

int main(void)
{
    sim_bus_t *bus;
    ec_master_t *master;
    uint8_t buf[64];
    size_t result_size, od_size;
    uint32_t abort_code;
    const uint8_t *od;
    int ret;

    bus = sim_bus_create(1, identities);
    TEST_CHECK(bus != NULL);
    if (!bus) {
        return test_done("test_sim_coe");
    }

    /* Preset objects: a 32-bit value and a string (normal transfer). */
    EC_WRITE_U32(buf, 0x12345678);
    TEST_CHECK_EQ(0, sim_bus_od_set(bus, 0, 0x2000, 0, buf, 4));
    TEST_CHECK_EQ(0, sim_bus_od_set(bus, 0, 0x2001, 0, test_string,
                sizeof(test_string)));

    TEST_CHECK_EQ(0, ecrt_lib_init(NULL, NULL));
    master = ecrt_startup_master(0, sim_bus_transport(bus), NULL, 0, -1, -1);
    TEST_CHECK(master != NULL);
    if (!master) {
        goto out_cleanup;
    }

    /* Expedited upload. */
    memset(buf, 0, sizeof(buf));
    abort_code = 0;
    ret = ecrt_master_sdo_upload(master, 0, 0x2000, 0, buf, sizeof(buf),
            &result_size, &abort_code);
    TEST_CHECK_EQ(0, ret);
    TEST_CHECK_EQ(4, result_size);
    TEST_CHECK_EQ(0x12345678u, EC_READ_U32(buf));

    /* Expedited download; verify arrival in the object dictionary. */
    EC_WRITE_U32(buf, 0xCAFEBABE);
    TEST_CHECK_EQ(0, ecrt_master_sdo_download(master, 0, 0x2000, 0, buf, 4,
                &abort_code));
    od = sim_bus_od_data(bus, 0, 0x2000, 0, &od_size);
    TEST_CHECK(od != NULL);
    if (od) {
        TEST_CHECK_EQ(4, od_size);
        TEST_CHECK_EQ(0xCAFEBABEu, EC_READ_U32(od));
    }

    /* ...and read it back. */
    ret = ecrt_master_sdo_upload(master, 0, 0x2000, 0, buf, sizeof(buf),
            &result_size, &abort_code);
    TEST_CHECK_EQ(0, ret);
    TEST_CHECK_EQ(0xCAFEBABEu, EC_READ_U32(buf));

    /* Normal (>4 byte) upload. */
    memset(buf, 0, sizeof(buf));
    ret = ecrt_master_sdo_upload(master, 0, 0x2001, 0, buf, sizeof(buf),
            &result_size, &abort_code);
    TEST_CHECK_EQ(0, ret);
    TEST_CHECK_EQ(sizeof(test_string), result_size);
    TEST_CHECK_EQ(0, memcmp(buf, test_string, sizeof(test_string)));

    /* Normal (>4 byte) download into a new object. */
    memcpy(buf, "0123456789AB", 12);
    TEST_CHECK_EQ(0, ecrt_master_sdo_download(master, 0, 0x2002, 0, buf, 12,
                &abort_code));
    od = sim_bus_od_data(bus, 0, 0x2002, 0, &od_size);
    TEST_CHECK(od != NULL);
    if (od) {
        TEST_CHECK_EQ(12, od_size);
        TEST_CHECK_EQ(0, memcmp(od, "0123456789AB", 12));
    }

    /* Upload of a nonexistent object must fail with an abort code. */
    abort_code = 0;
    ret = ecrt_master_sdo_upload(master, 0, 0x5555, 0, buf, sizeof(buf),
            &result_size, &abort_code);
    TEST_CHECK(ret != 0);
    TEST_CHECK_EQ(0x06020000u, abort_code); /* object does not exist */

    ecrt_release_master(master);
out_cleanup:
    ecrt_lib_cleanup();
    sim_bus_destroy(bus);

    return test_done("test_sim_coe");
}
