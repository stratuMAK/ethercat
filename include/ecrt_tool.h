/*****************************************************************************
 *
 *  Copyright (C) 2006-2024  Florian Pose, Ingenieurgemeinschaft IgH
 *  Copyright (C) 2024-2026  LinuxCNC contributors
 *
 *  This file is part of the IgH EtherCAT master userspace library.
 *
 *  The IgH EtherCAT master userspace library is free software; you can
 *  redistribute it and/or modify it under the terms of the GNU Lesser General
 *  Public License as published by the Free Software Foundation; version 2.1
 *  of the License.
 *
 *  This file is distributed in the hope that it will be useful, but WITHOUT
 *  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 *  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 *  License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public License
 *  along with this file. If not, see <http://www.gnu.org/licenses/>.
 *
 ****************************************************************************/

/* WARNING: This is an unstable internal API. Struct layouts and function
 * signatures may change between minor releases. This header is intended
 * for the ethercat CLI tool which is always version-matched with the
 * library. Third-party consumers should not rely on ABI stability. */

/**
 * \file
 * EtherCAT master tool/diagnostic API.
 *
 * Public interface for diagnostic and configuration tool operations on an
 * EtherCAT master.  These functions provide the same capabilities as the
 * \c ethercat command-line tool but are callable in-process without requiring
 * a socket connection.
 *
 * All functions are blocking and NOT realtime-safe.
 *
 * \defgroup ToolAPI EtherCAT Tool API
 * @{
 */

/****************************************************************************/

#ifndef __ECRT_TOOL_H__
#define __ECRT_TOOL_H__

#ifdef EC_USPACE_MASTER

#include <stdint.h>
#include "ecrt.h"

/****************************************************************************/
/* Constants                                                                 */
/****************************************************************************/

/** Maximum number of EtherCAT devices (main + backup). */
#ifndef EC_TOOL_MAX_NUM_DEVICES
#define EC_TOOL_MAX_NUM_DEVICES 2
#endif

/** Number of statistic rate intervals. */
#ifndef EC_TOOL_RATE_COUNT
#define EC_TOOL_RATE_COUNT 3
#endif

/** String size for tool data structures. */
#define EC_TOOL_STRING_SIZE 64

/** Maximum size for displayed SDO data. */
#define EC_TOOL_MAX_SDO_DATA_SIZE 1024

/** Maximum size for displayed IDN data. */
#define EC_TOOL_MAX_IDN_DATA_SIZE 1024

/** Maximum size for feature flag keys. */
#define EC_TOOL_MAX_FLAG_KEY_SIZE 128

/** Size of the datagram/EoE device name string. */
#define EC_TOOL_DATAGRAM_NAME_SIZE 20

/** Maximum hostname size (EoE set IP). */
#define EC_TOOL_MAX_HOSTNAME_SIZE 32

/** Ethernet address length. */
#define EC_TOOL_ETH_ALEN 6

/** Number of DC sync signals. */
#define EC_TOOL_SYNC_SIGNAL_COUNT 2

/****************************************************************************/
/* Supporting types (from ec_ipc_types.h, merged here for public use)       */
/****************************************************************************/

/** Slave information interface CANopen over EtherCAT details flags. */
typedef struct {
    uint8_t enable_sdo : 1;
    uint8_t enable_sdo_info : 1;
    uint8_t enable_pdo_assign : 1;
    uint8_t enable_pdo_configuration : 1;
    uint8_t enable_upload_at_startup : 1;
    uint8_t enable_sdo_complete_access : 1;
} ec_tool_sii_coe_details_t;

/** Slave information interface general flags. */
typedef struct {
    uint8_t enable_safeop : 1;
    uint8_t enable_not_lrw : 1;
} ec_tool_sii_general_flags_t;

/** EtherCAT slave distributed clocks range. */
typedef enum {
    EC_TOOL_DC_32, /**< 32 bit. */
    EC_TOOL_DC_64  /**< 64 bit. */
} ec_tool_slave_dc_range_t;

/** Sync signal configuration. */
typedef struct {
    uint32_t cycle_time; /**< Cycle time [ns]. */
    int32_t shift_time;  /**< Shift time [ns]. */
} ec_tool_sync_signal_t;

/** SDO entry access state indices. */
enum {
    EC_TOOL_SDO_ENTRY_ACCESS_PREOP,
    EC_TOOL_SDO_ENTRY_ACCESS_SAFEOP,
    EC_TOOL_SDO_ENTRY_ACCESS_OP,
    EC_TOOL_SDO_ENTRY_ACCESS_COUNT
};

/****************************************************************************/
/* Tool data structures                                                      */
/****************************************************************************/

/** Module (version/count) information. */
typedef struct {
    uint32_t ioctl_version_magic;
    uint32_t master_count;
} ec_tool_module_t;

/** Per-device statistics. */
typedef struct {
    uint8_t address[EC_TOOL_ETH_ALEN];
    uint8_t attached;
    uint8_t link_state;
    uint64_t tx_count;
    uint64_t rx_count;
    uint64_t tx_bytes;
    uint64_t rx_bytes;
    uint64_t tx_errors;
    int32_t tx_frame_rates[EC_TOOL_RATE_COUNT];
    int32_t rx_frame_rates[EC_TOOL_RATE_COUNT];
    int32_t tx_byte_rates[EC_TOOL_RATE_COUNT];
    int32_t rx_byte_rates[EC_TOOL_RATE_COUNT];
} ec_tool_device_t;

/** Master status information. */
typedef struct {
    uint32_t slave_count;
    uint32_t scan_index;
    uint32_t config_count;
    uint32_t domain_count;
    uint32_t eoe_handler_count;
    uint8_t phase;
    uint8_t active;
    uint8_t scan_busy;
    ec_tool_device_t devices[EC_TOOL_MAX_NUM_DEVICES];
    uint32_t num_devices;
    uint64_t tx_count;
    uint64_t rx_count;
    uint64_t tx_bytes;
    uint64_t rx_bytes;
    int32_t tx_frame_rates[EC_TOOL_RATE_COUNT];
    int32_t rx_frame_rates[EC_TOOL_RATE_COUNT];
    int32_t tx_byte_rates[EC_TOOL_RATE_COUNT];
    int32_t rx_byte_rates[EC_TOOL_RATE_COUNT];
    int32_t loss_rates[EC_TOOL_RATE_COUNT];
    uint64_t app_time;
    uint64_t dc_ref_time;
    uint16_t ref_clock;
} ec_tool_master_t;

/** Slave information. */
typedef struct {
    // input
    uint16_t position;

    // outputs
    uint32_t device_index;
    uint32_t vendor_id;
    uint32_t product_code;
    uint32_t revision_number;
    uint32_t serial_number;
    uint16_t alias;
    uint16_t boot_rx_mailbox_offset;
    uint16_t boot_rx_mailbox_size;
    uint16_t boot_tx_mailbox_offset;
    uint16_t boot_tx_mailbox_size;
    uint16_t std_rx_mailbox_offset;
    uint16_t std_rx_mailbox_size;
    uint16_t std_tx_mailbox_offset;
    uint16_t std_tx_mailbox_size;
    uint16_t mailbox_protocols;
    uint8_t has_general_category;
    ec_tool_sii_coe_details_t coe_details;
    ec_tool_sii_general_flags_t general_flags;
    int16_t current_on_ebus;
    struct {
        ec_slave_port_desc_t desc;
        ec_slave_port_link_t link;
        uint32_t receive_time;
        uint16_t next_slave;
        uint32_t delay_to_next_dc;
    } ports[EC_MAX_PORTS];
    uint8_t fmmu_bit;
    uint8_t dc_supported;
    ec_tool_slave_dc_range_t dc_range;
    uint8_t has_dc_system_time;
    uint32_t transmission_delay;
    uint8_t al_state;
    uint8_t error_flag;
    uint8_t sync_count;
    uint16_t sdo_count;
    uint32_t sii_nwords;
    char group[EC_TOOL_STRING_SIZE];
    char image[EC_TOOL_STRING_SIZE];
    char order[EC_TOOL_STRING_SIZE];
    char name[EC_TOOL_STRING_SIZE];
} ec_tool_slave_t;

/** Slave sync manager information. */
typedef struct {
    // inputs
    uint16_t slave_position;
    uint32_t sync_index;

    // outputs
    uint16_t physical_start_address;
    uint16_t default_size;
    uint8_t control_register;
    uint8_t enable;
    uint8_t pdo_count;
} ec_tool_slave_sync_t;

/** Slave sync manager PDO information. */
typedef struct {
    // inputs
    uint16_t slave_position;
    uint32_t sync_index;
    uint32_t pdo_pos;

    // outputs
    uint16_t index;
    uint8_t entry_count;
    int8_t name[EC_TOOL_STRING_SIZE];
} ec_tool_slave_sync_pdo_t;

/** Slave sync manager PDO entry information. */
typedef struct {
    // inputs
    uint16_t slave_position;
    uint32_t sync_index;
    uint32_t pdo_pos;
    uint32_t entry_pos;

    // outputs
    uint16_t index;
    uint8_t subindex;
    uint8_t bit_length;
    int8_t name[EC_TOOL_STRING_SIZE];
} ec_tool_slave_sync_pdo_entry_t;

/** Domain information. */
typedef struct {
    // inputs
    uint32_t index;

    // outputs
    uint32_t data_size;
    uint32_t logical_base_address;
    uint16_t working_counter[EC_TOOL_MAX_NUM_DEVICES];
    uint16_t expected_working_counter;
    uint32_t fmmu_count;
} ec_tool_domain_t;

/** Domain FMMU information. */
typedef struct {
    // inputs
    uint32_t domain_index;
    uint32_t fmmu_index;

    // outputs
    uint16_t slave_config_alias;
    uint16_t slave_config_position;
    uint8_t sync_index;
    ec_direction_t dir;
    uint32_t logical_address;
    uint32_t data_size;
} ec_tool_domain_fmmu_t;

/** Domain data request. */
typedef struct {
    // inputs
    uint32_t domain_index;
    uint32_t data_size;
    uint8_t *target; /**< Caller-provided buffer for domain data. */
} ec_tool_domain_data_t;

/** Slave position wildcard: request the state change for every slave on the
 * bus in one call, under a single acquisition of the master lock. */
#define EC_TOOL_SLAVE_POSITION_ALL 0xffff

/** Slave state change request. */
typedef struct {
    // inputs
    uint16_t slave_position; /**< Ring position, or EC_TOOL_SLAVE_POSITION_ALL. */
    uint8_t al_state;
} ec_tool_slave_state_t;

/** Slave SDO dictionary entry information. */
typedef struct {
    // inputs
    uint16_t slave_position;
    uint16_t sdo_position;

    // outputs
    uint16_t sdo_index;
    uint8_t max_subindex;
    int8_t name[EC_TOOL_STRING_SIZE];
} ec_tool_slave_sdo_t;

/** Slave SDO entry information. */
typedef struct {
    // inputs
    uint16_t slave_position;
    int32_t sdo_spec; /**< positive: SDO index, negative: list position */
    uint8_t sdo_entry_subindex;

    // outputs
    uint16_t data_type;
    uint16_t bit_length;
    uint8_t read_access[EC_TOOL_SDO_ENTRY_ACCESS_COUNT];
    uint8_t write_access[EC_TOOL_SDO_ENTRY_ACCESS_COUNT];
    int8_t description[EC_TOOL_STRING_SIZE];
} ec_tool_slave_sdo_entry_t;

/** SDO upload (read) request/response. */
typedef struct {
    // inputs
    uint16_t slave_position;
    uint16_t sdo_index;
    uint8_t sdo_entry_subindex;
    uint32_t target_size;
    uint8_t *target; /**< Caller-provided buffer. */

    // outputs
    uint32_t data_size;
    uint32_t abort_code;
} ec_tool_slave_sdo_upload_t;

/** SDO download (write) request. */
typedef struct {
    // inputs
    uint16_t slave_position;
    uint16_t sdo_index;
    uint8_t sdo_entry_subindex;
    uint8_t complete_access;
    uint32_t data_size;
    uint8_t *data; /**< Caller-provided data buffer. */

    // outputs
    uint32_t abort_code;
} ec_tool_slave_sdo_download_t;

/** SII (EEPROM) read/write request. */
typedef struct {
    // inputs
    uint16_t slave_position;
    uint16_t offset;
    uint32_t nwords;
    uint16_t *words; /**< Caller-provided buffer. */
} ec_tool_slave_sii_t;

/** Register read/write request. */
typedef struct {
    // inputs
    uint16_t slave_position;
    uint8_t emergency;
    uint16_t address;
    uint32_t size;
    uint8_t *data; /**< Caller-provided buffer. */
} ec_tool_slave_reg_t;

/** FoE (File-over-EtherCAT) read/write request. */
typedef struct {
    // inputs
    uint16_t slave_position;
    uint16_t offset;
    uint32_t buffer_size;
    uint8_t *buffer; /**< Caller-provided buffer. */

    // outputs
    uint32_t data_size;
    uint32_t result;
    uint32_t error_code;
    char file_name[32];
} ec_tool_slave_foe_t;

/** SoE (Servo-over-EtherCAT) read request. */
typedef struct {
    // inputs
    uint16_t slave_position;
    uint8_t drive_no;
    uint16_t idn;
    uint32_t mem_size;
    uint8_t *data; /**< Caller-provided buffer. */

    // outputs
    uint32_t data_size;
    uint16_t error_code;
} ec_tool_slave_soe_read_t;

/** SoE (Servo-over-EtherCAT) write request. */
typedef struct {
    // inputs
    uint16_t slave_position;
    uint8_t drive_no;
    uint16_t idn;
    uint32_t data_size;
    uint8_t *data; /**< Caller-provided data buffer. */

    // outputs
    uint16_t error_code;
} ec_tool_slave_soe_write_t;

/** Slave configuration information. */
typedef struct {
    // inputs
    uint32_t config_index;

    // outputs
    uint16_t alias;
    uint16_t position;
    uint32_t vendor_id;
    uint32_t product_code;
    struct {
        ec_direction_t dir;
        ec_watchdog_mode_t watchdog_mode;
        uint32_t pdo_count;
        uint8_t config_this;
    } syncs[EC_MAX_SYNC_MANAGERS];
    uint16_t watchdog_divider;
    uint16_t watchdog_intervals;
    uint32_t sdo_count;
    uint32_t idn_count;
    uint32_t flag_count;
    int32_t slave_position;
    uint16_t dc_assign_activate;
    ec_tool_sync_signal_t dc_sync[EC_TOOL_SYNC_SIGNAL_COUNT];
} ec_tool_config_t;

/** Slave configuration PDO information. */
typedef struct {
    // inputs
    uint32_t config_index;
    uint8_t sync_index;
    uint16_t pdo_pos;

    // outputs
    uint16_t index;
    uint8_t entry_count;
    int8_t name[EC_TOOL_STRING_SIZE];
} ec_tool_config_pdo_t;

/** Slave configuration PDO entry information. */
typedef struct {
    // inputs
    uint32_t config_index;
    uint8_t sync_index;
    uint16_t pdo_pos;
    uint8_t entry_pos;

    // outputs
    uint16_t index;
    uint8_t subindex;
    uint8_t bit_length;
    int8_t name[EC_TOOL_STRING_SIZE];
} ec_tool_config_pdo_entry_t;

/** Slave configuration SDO information. */
typedef struct {
    // inputs
    uint32_t config_index;
    uint32_t sdo_pos;

    // outputs
    uint16_t index;
    uint8_t subindex;
    uint32_t size;
    uint8_t data[EC_TOOL_MAX_SDO_DATA_SIZE];
    uint8_t complete_access;
} ec_tool_config_sdo_t;

/** Slave configuration IDN information. */
typedef struct {
    // inputs
    uint32_t config_index;
    uint32_t idn_pos;

    // outputs
    uint8_t drive_no;
    uint16_t idn;
    ec_al_state_t state;
    uint32_t size;
    uint8_t data[EC_TOOL_MAX_IDN_DATA_SIZE];
} ec_tool_config_idn_t;

/** Slave configuration feature flag information. */
typedef struct {
    // inputs
    uint32_t config_index;
    uint32_t flag_pos;

    // outputs
    char key[EC_TOOL_MAX_FLAG_KEY_SIZE];
    int32_t value;
} ec_tool_config_flag_t;

/** EoE handler information. */
typedef struct {
    // input
    uint16_t eoe_index;

    // outputs
    char name[EC_TOOL_DATAGRAM_NAME_SIZE];
    uint16_t slave_position;
    uint8_t open;
    uint32_t rx_bytes;
    uint32_t rx_rate;
    uint32_t tx_bytes;
    uint32_t tx_rate;
    uint32_t tx_queued_frames;
    uint32_t tx_queue_size;
} ec_tool_eoe_handler_t;

/** EoE set IP parameter request. */
typedef struct {
    // inputs
    uint16_t slave_position;
    uint8_t mac_address_included;
    uint8_t ip_address_included;
    uint8_t subnet_mask_included;
    uint8_t gateway_included;
    uint8_t dns_included;
    uint8_t name_included;
    unsigned char mac_address[EC_TOOL_ETH_ALEN];
    uint32_t ip_address;
    uint32_t subnet_mask;
    uint32_t gateway;
    uint32_t dns;
    char name[EC_TOOL_MAX_HOSTNAME_SIZE];

    // outputs
    uint16_t result;
} ec_tool_eoe_ip_t;

/****************************************************************************/
/* Tool API functions                                                        */
/****************************************************************************/

#ifdef __cplusplus
extern "C" {
#endif

/** Get module information (version magic, master count).
 * \return 0 on success, negative errno on failure. */
EC_PUBLIC_API int ecrt_tool_get_module(ec_tool_module_t *data);

/** Get master status information.
 * \return 0 on success, negative errno on failure. */
EC_PUBLIC_API int ecrt_tool_get_master(ec_master_t *master,
        ec_tool_master_t *data);

/** Get slave information.
 * \a data->position must be set to the slave's ring position on input.
 * \return 0 on success, negative errno on failure. */
EC_PUBLIC_API int ecrt_tool_get_slave(ec_master_t *master,
        ec_tool_slave_t *data);

/** Get slave sync manager information.
 * \a data->slave_position and \a data->sync_index must be set on input.
 * \return 0 on success, negative errno on failure. */
EC_PUBLIC_API int ecrt_tool_get_slave_sync(ec_master_t *master,
        ec_tool_slave_sync_t *data);

/** Get slave sync manager PDO information.
 * \return 0 on success, negative errno on failure. */
EC_PUBLIC_API int ecrt_tool_get_slave_sync_pdo(ec_master_t *master,
        ec_tool_slave_sync_pdo_t *data);

/** Get slave sync manager PDO entry information.
 * \return 0 on success, negative errno on failure. */
EC_PUBLIC_API int ecrt_tool_get_slave_sync_pdo_entry(ec_master_t *master,
        ec_tool_slave_sync_pdo_entry_t *data);

/** Get domain information.
 * \a data->index must be set on input.
 * \return 0 on success, negative errno on failure. */
EC_PUBLIC_API int ecrt_tool_get_domain(ec_master_t *master,
        ec_tool_domain_t *data);

/** Get domain FMMU information.
 * \return 0 on success, negative errno on failure. */
EC_PUBLIC_API int ecrt_tool_get_domain_fmmu(ec_master_t *master,
        ec_tool_domain_fmmu_t *data);

/** Get domain process data.
 * \a data->domain_index and \a data->data_size must be set; \a data->target
 * must point to a buffer of at least \a data_size bytes.
 * \return 0 on success, negative errno on failure. */
EC_PUBLIC_API int ecrt_tool_get_domain_data(ec_master_t *master,
        ec_tool_domain_data_t *data);

/** Set master debug level.
 * \return 0 on success, negative errno on failure. */
EC_PUBLIC_API int ecrt_tool_set_debug(ec_master_t *master,
        unsigned int level);

/** Trigger a bus rescan.
 * \return 0 on success, negative errno on failure. */
EC_PUBLIC_API int ecrt_tool_rescan(ec_master_t *master);

/** Request a slave state change.
 * With \a data->slave_position set to EC_TOOL_SLAVE_POSITION_ALL the request
 * is recorded for every slave on the bus atomically with respect to the
 * master FSM (one lock hold).
 * \return 0 on success, negative errno on failure. */
EC_PUBLIC_API int ecrt_tool_set_slave_state(ec_master_t *master,
        ec_tool_slave_state_t *data);

/** Get slave SDO dictionary entry information.
 * \return 0 on success, negative errno on failure. */
EC_PUBLIC_API int ecrt_tool_get_slave_sdo(ec_master_t *master,
        ec_tool_slave_sdo_t *data);

/** Get slave SDO entry metadata.
 * \return 0 on success, negative errno on failure. */
EC_PUBLIC_API int ecrt_tool_get_slave_sdo_entry(ec_master_t *master,
        ec_tool_slave_sdo_entry_t *data);

/** Upload (read) an SDO value from a slave.
 * \a data->target must point to a buffer of at least \a target_size bytes.
 * \return 0 on success, negative errno on failure. */
EC_PUBLIC_API int ecrt_tool_sdo_upload(ec_master_t *master,
        ec_tool_slave_sdo_upload_t *data);

/** Download (write) an SDO value to a slave.
 * \a data->data must point to a buffer of \a data_size bytes.
 * \return 0 on success, negative errno on failure. */
EC_PUBLIC_API int ecrt_tool_sdo_download(ec_master_t *master,
        ec_tool_slave_sdo_download_t *data);

/** Read SII (EEPROM) words from a slave.
 * \a data->words must point to a buffer of at least \a nwords * 2 bytes.
 * \return 0 on success, negative errno on failure. */
EC_PUBLIC_API int ecrt_tool_sii_read(ec_master_t *master,
        ec_tool_slave_sii_t *data);

/** Write SII (EEPROM) words to a slave.  Blocking — waits for FSM.
 * \a data->words must point to a buffer of \a nwords * 2 bytes.
 * \return 0 on success, negative errno on failure. */
EC_PUBLIC_API int ecrt_tool_sii_write(ec_master_t *master,
        ec_tool_slave_sii_t *data);

/** Read slave registers.  Blocking — waits for FSM.
 * \a data->data must point to a buffer of at least \a size bytes.
 * \return 0 on success, negative errno on failure. */
EC_PUBLIC_API int ecrt_tool_reg_read(ec_master_t *master,
        ec_tool_slave_reg_t *data);

/** Write slave registers.  Blocking — waits for FSM.
 * \a data->data must point to a buffer of \a size bytes.
 * \return 0 on success, negative errno on failure. */
EC_PUBLIC_API int ecrt_tool_reg_write(ec_master_t *master,
        ec_tool_slave_reg_t *data);

/** Read a file from a slave via FoE.  Blocking — waits for FSM.
 * \a data->buffer must point to a buffer of at least \a buffer_size bytes.
 * \return 0 on success, negative errno on failure. */
EC_PUBLIC_API int ecrt_tool_foe_read(ec_master_t *master,
        ec_tool_slave_foe_t *data);

/** Write a file to a slave via FoE.  Blocking — waits for FSM.
 * \a data->buffer must point to a buffer of \a buffer_size bytes.
 * \return 0 on success, negative errno on failure. */
EC_PUBLIC_API int ecrt_tool_foe_write(ec_master_t *master,
        ec_tool_slave_foe_t *data);

/** Read SoE IDN from a slave.  Blocking — waits for FSM.
 * \a data->data must point to a buffer of at least \a mem_size bytes.
 * \return 0 on success, negative errno on failure. */
EC_PUBLIC_API int ecrt_tool_soe_read(ec_master_t *master,
        ec_tool_slave_soe_read_t *data);

/** Write SoE IDN to a slave.  Blocking — waits for FSM.
 * \a data->data must point to a buffer of \a data_size bytes.
 * \return 0 on success, negative errno on failure. */
EC_PUBLIC_API int ecrt_tool_soe_write(ec_master_t *master,
        ec_tool_slave_soe_write_t *data);

/** Get slave configuration information.
 * \a data->config_index must be set on input.
 * \return 0 on success, negative errno on failure. */
EC_PUBLIC_API int ecrt_tool_get_config(ec_master_t *master,
        ec_tool_config_t *data);

/** Get slave configuration PDO information.
 * \return 0 on success, negative errno on failure. */
EC_PUBLIC_API int ecrt_tool_get_config_pdo(ec_master_t *master,
        ec_tool_config_pdo_t *data);

/** Get slave configuration PDO entry information.
 * \return 0 on success, negative errno on failure. */
EC_PUBLIC_API int ecrt_tool_get_config_pdo_entry(ec_master_t *master,
        ec_tool_config_pdo_entry_t *data);

/** Get slave configuration SDO information.
 * \return 0 on success, negative errno on failure. */
EC_PUBLIC_API int ecrt_tool_get_config_sdo(ec_master_t *master,
        ec_tool_config_sdo_t *data);

/** Get slave configuration IDN information.
 * \return 0 on success, negative errno on failure. */
EC_PUBLIC_API int ecrt_tool_get_config_idn(ec_master_t *master,
        ec_tool_config_idn_t *data);

/** Get slave configuration feature flag information.
 * \return 0 on success, negative errno on failure. */
EC_PUBLIC_API int ecrt_tool_get_config_flag(ec_master_t *master,
        ec_tool_config_flag_t *data);

#ifdef EC_EOE
/** Get EoE handler information.
 * \return 0 on success, negative errno on failure. */
EC_PUBLIC_API int ecrt_tool_get_eoe_handler(ec_master_t *master,
        ec_tool_eoe_handler_t *data);

/** Set EoE IP parameters on a slave.
 * Queues an EoE Set IP Parameter request and waits for completion.
 * \return 0 on success, negative errno on failure. */
EC_PUBLIC_API int ecrt_tool_set_eoe_ip(ec_master_t *master,
        ec_tool_eoe_ip_t *data);
#endif

#ifdef __cplusplus
}
#endif

#endif /* EC_USPACE_MASTER */

/** @} */

#endif /* __ECRT_TOOL_H__ */
