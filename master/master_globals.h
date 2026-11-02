/*****************************************************************************
 *
 *  Copyright (C) 2006-2021  Florian Pose, Ingenieurgemeinschaft IgH
 *
 *  This file is part of the IgH EtherCAT master.
 *
 *  The file is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU Lesser General Public License as published by the
 *  Free Software Foundation; version 2.1 of the License.
 *
 *  This file is distributed in the hope that it will be useful, but WITHOUT
 *  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 *  FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public
 *  License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public License
 *  along with this file. If not, see <http://www.gnu.org/licenses/>.
 *
 ****************************************************************************/

/** \file
 * Global definitions and macros.
 */

/****************************************************************************/

#ifndef __EC_MASTER_GLOBALS_H__
#define __EC_MASTER_GLOBALS_H__

#include "ec_ipc_types.h"
#include "../globals.h"

/*****************************************************************************
 * EtherCAT master
 ****************************************************************************/

/** Datagram timeout in microseconds. */
#define EC_IO_TIMEOUT 500

/** Datagram timeout in microseconds before activation (idle phase: bus
 * scan, slave configuration).  Nothing there is time-critical, and its
 * replies may be handled on a CPU shared with the rest of the system -- on
 * a coating machine they took 0.5-1.8 ms, and the scan failed with
 * EC_IO_TIMEOUT. */
#define EC_IDLE_IO_TIMEOUT 2000

/** Time to send a byte in nanoseconds.
 *
 * t_ns = 1 / (100 MBit/s / 8 bit/byte) = 80 ns/byte
 */
#define EC_BYTE_TRANSMISSION_TIME_NS 80

/** Number of state machine retries on datagram timeout. */
#define EC_FSM_RETRIES 3

/** Bus scans the master makes at startup before it accepts one that left
 * slaves unscanned: a slave without its SII cannot be configured, and a
 * reply that arrived too late in one scan usually arrives in time in the
 * next. */
#define EC_INITIAL_SCAN_ATTEMPTS 3

/** Seconds to wait before fetching SDO dictionary
    after slave entered PREOP state. */
#define EC_WAIT_SDO_DICT 3

/** Minimum size of a buffer used with ec_state_string(). */
#define EC_STATE_STRING_SIZE 32

/** Maximum SII size in words, to avoid infinite reading. */
#define EC_MAX_SII_SIZE 4096

/*****************************************************************************
 * EtherCAT protocol
 ****************************************************************************/

/** Size of an EtherCAT frame header. */
#define EC_FRAME_HEADER_SIZE 2

/** Size of an EtherCAT datagram header. */
#define EC_DATAGRAM_HEADER_SIZE 10

/** Size of an EtherCAT datagram footer. */
#define EC_DATAGRAM_FOOTER_SIZE 2

/** Size of the EtherCAT address field. */
#define EC_ADDR_LEN 4

/** Resulting maximum data size of a single datagram in a frame. */
#define EC_MAX_DATA_SIZE (ETH_DATA_LEN - EC_FRAME_HEADER_SIZE \
                          - EC_DATAGRAM_HEADER_SIZE - EC_DATAGRAM_FOOTER_SIZE)

/** Mailbox header size.  */
#define EC_MBOX_HEADER_SIZE 6

/** Word offset of first SII category. */
#define EC_FIRST_SII_CATEGORY_OFFSET 0x40

/** Qualifier for fields shared between the application (RT) thread and
 * the master threads without locking: the datagram state and the
 * injection/ring handover counters. In the userspace build these are
 * C11 atomics, which gives the plain assignments and comparisons in
 * the shared core sequentially consistent semantics and establishes
 * the happens-before edges for the datagram fields they guard. The
 * kernel build keeps plain fields and relies on its existing
 * barrier/locking conventions. */
#ifdef EC_USPACE_MASTER
#define EC_PAL_SHARED _Atomic
#else
#define EC_PAL_SHARED
#endif

/* The EC_RT_ATTR / EC_RT_TRUSTED_* realtime annotation macros live in
 * globals.h so the PAL headers can use them as well. */

/** Size of a sync manager configuration page. */
#define EC_SYNC_PAGE_SIZE 8

/** Maximum number of FMMUs per slave. */
#define EC_MAX_FMMUS 16

/** Size of an FMMU configuration page. */
#define EC_FMMU_PAGE_SIZE 16

/****************************************************************************/

/** Convenience macro for printing EtherCAT-specific information to syslog.
 *
 * This will print the message in \a fmt with a prefixed "EtherCAT: ".
 *
 * \param fmt format string (like in printf())
 * \param args arguments (optional)
 */
#define EC_INFO(fmt, args...) \
    ec_log(EC_LOG_INFO, "EtherCAT: " fmt, ##args)

/** Convenience macro for printing EtherCAT-specific errors to syslog.
 *
 * This will print the message in \a fmt with a prefixed "EtherCAT ERROR: ".
 *
 * \param fmt format string (like in printf())
 * \param args arguments (optional)
 */
#define EC_ERR(fmt, args...) \
    ec_log(EC_LOG_ERR, "EtherCAT ERROR: " fmt, ##args)

/** Convenience macro for printing EtherCAT-specific warnings to syslog.
 *
 * This will print the message in \a fmt with a prefixed "EtherCAT WARNING: ".
 *
 * \param fmt format string (like in printf())
 * \param args arguments (optional)
 */
#define EC_WARN(fmt, args...) \
    ec_log(EC_LOG_WARNING, "EtherCAT WARNING: " fmt, ##args)

/** Convenience macro for printing EtherCAT debug messages to syslog.
 *
 * This will print the message in \a fmt with a prefixed "EtherCAT DEBUG: ".
 *
 * \param fmt format string (like in printf())
 * \param args arguments (optional)
 */
#define EC_DBG(fmt, args...) \
    ec_log(EC_LOG_DEBUG, "EtherCAT DEBUG: " fmt, ##args)

/****************************************************************************/

/** Absolute value.
 */
#define EC_ABS(X) ((X) >= 0 ? (X) : -(X))

/****************************************************************************/

extern char *ec_master_version_str;

/****************************************************************************/

unsigned int ec_master_count(void);

ec_master_t *ecrt_request_master_err(unsigned int);

/****************************************************************************/

/** Code/Message pair.
 *
 * Some EtherCAT datagrams support reading a status code to display a certain
 * message. This type allows to map a code to a message string.
 */
typedef struct {
    uint32_t code; /**< Code. */
    const char *message; /**< Message belonging to \a code. */
} ec_code_msg_t;

/****************************************************************************/

/** Generic request state.
 *
 * \attention If ever changing this, please be sure to adjust the \a
 * state_table in master/sdo_request.c.
 */
typedef enum {
    EC_INT_REQUEST_INIT,
    EC_INT_REQUEST_QUEUED,
    EC_INT_REQUEST_BUSY,
    EC_INT_REQUEST_SUCCESS,
    EC_INT_REQUEST_FAILURE
} ec_internal_request_state_t;

/****************************************************************************/

extern const ec_request_state_t ec_request_state_translation_table[];

/****************************************************************************/

/** Origin type.
 */
typedef enum {
    EC_ORIG_INTERNAL, /**< Internal. */
    EC_ORIG_EXTERNAL /**< External. */
} ec_origin_t;

/****************************************************************************/

typedef struct ec_slave ec_slave_t; /**< \see ec_slave. */

/****************************************************************************/

/*****************************************************************************
 * Utils
 ****************************************************************************/

#define EC_MAX_MAC_STRING_SIZE (3 * ETH_ALEN)

int ec_mac_equal(const uint8_t *, const uint8_t *);
ssize_t ec_mac_print(const uint8_t *, char *);
int ec_mac_is_zero(const uint8_t *);
int ec_mac_is_broadcast(const uint8_t *);
int ec_mac_parse(uint8_t *, const char *, int);
void ec_print_data(const uint8_t *, size_t) EC_RT_ATTR;
void ec_print_data_diff(const uint8_t *, const uint8_t *, size_t);
size_t ec_state_string(uint8_t, char *, uint8_t);

#endif
