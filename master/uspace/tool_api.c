/*****************************************************************************
 *
 *  Copyright (C) 2006-2024  Florian Pose, Ingenieurgemeinschaft IgH
 *  Copyright (C) 2024-2026  LinuxCNC contributors
 *
 *  This file is part of the IgH EtherCAT Master.
 *
 *  The IgH EtherCAT Master is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License version 2, as
 *  published by the Free Software Foundation.
 *
 *  The IgH EtherCAT Master is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General
 *  Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with the IgH EtherCAT Master; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 ****************************************************************************/

/**
 * \file
 * EtherCAT master tool API implementation.
 *
 * Provides the ecrt_tool_*() functions declared in include/ecrt_tool.h.
 * These are in-process, blocking, non-RT-safe diagnostic/configuration
 * functions that access master internals under the appropriate locks.
 */

/****************************************************************************/

#include "master.h"
#include "slave.h"
#include "slave_config.h"
#include "domain.h"
#include "sdo.h"
#include "sdo_entry.h"
#include "pdo.h"
#include "pdo_list.h"
#include "pdo_entry.h"
#include "foe_request.h"
#include "reg_request.h"
#include "ec_ioctl_data.h"

#ifdef EC_EOE
#include "ethernet.h"
#endif

#include "../include/ecrt_tool.h"

#include <string.h>
#include <stdlib.h>
#include <errno.h>

/****************************************************************************/
/* Helpers                                                                     */
/****************************************************************************/

/** Copy a string into a fixed-size buffer, NUL-terminate. */
static void tool_strcpy(char *target, size_t target_size, const char *source)
{
    if (source) {
        strncpy(target, source, target_size - 1);
        target[target_size - 1] = '\0';
    } else {
        target[0] = '\0';
    }
}

/** Convenience: copy into EC_TOOL_STRING_SIZE buffer. */
#define TOOL_STRCPY(dst, src) \
    tool_strcpy((char *)(dst), EC_TOOL_STRING_SIZE, (src))

/****************************************************************************/
/* Module                                                                     */
/****************************************************************************/

int ecrt_tool_get_module(ec_tool_module_t *data)
{
    if (!data)
        return -EINVAL;
    data->ioctl_version_magic = EC_IOCTL_VERSION_MAGIC;
    data->master_count = (uint32_t)ec_master_registry_count();
    return 0;
}

/****************************************************************************/
/* Master                                                                     */
/****************************************************************************/

int ecrt_tool_get_master(ec_master_t *master, ec_tool_master_t *data)
{
    unsigned int dev_idx, j;

    if (!master || !data)
        return -EINVAL;

    memset(data, 0, sizeof(*data));

    if (ec_sem_down_interruptible(&master->master_sem))
        return -EINTR;

    data->slave_count  = master->slave_count;
    data->scan_index   = master->scan_index;
    data->config_count = ec_master_config_count(master);
    data->domain_count = ec_master_domain_count(master);
#ifdef EC_EOE
    data->eoe_handler_count = ec_master_eoe_handler_count(master);
#else
    data->eoe_handler_count = 0;
#endif
    data->phase     = (uint8_t)master->phase;
    data->active    = (uint8_t)master->active;
    data->scan_busy = master->scan_busy;

    ec_sem_up(&master->master_sem);

    if (ec_sem_down_interruptible(&master->device_sem))
        return -EINTR;

    for (dev_idx = EC_DEVICE_MAIN;
            dev_idx < ec_master_num_devices(master); dev_idx++) {
        ec_device_t *device = &master->devices[dev_idx];

        if (dev_idx >= EC_TOOL_MAX_NUM_DEVICES)
            break;

        memcpy(data->devices[dev_idx].address, master->macs[dev_idx],
                EC_TOOL_ETH_ALEN);
        data->devices[dev_idx].attached   = 1;
        data->devices[dev_idx].link_state = device->link_state ? 1 : 0;
        data->devices[dev_idx].tx_count   = device->tx_count;
        data->devices[dev_idx].rx_count   = device->rx_count;
        data->devices[dev_idx].tx_bytes   = device->tx_bytes;
        data->devices[dev_idx].rx_bytes   = device->rx_bytes;
        data->devices[dev_idx].tx_errors  = device->tx_errors;
        for (j = 0; j < EC_TOOL_RATE_COUNT; j++) {
            data->devices[dev_idx].tx_frame_rates[j] =
                device->tx_frame_rates[j];
            data->devices[dev_idx].rx_frame_rates[j] =
                device->rx_frame_rates[j];
            data->devices[dev_idx].tx_byte_rates[j] =
                device->tx_byte_rates[j];
            data->devices[dev_idx].rx_byte_rates[j] =
                device->rx_byte_rates[j];
        }
    }
    data->num_devices = ec_master_num_devices(master);

    data->tx_count = master->device_stats.tx_count;
    data->rx_count = master->device_stats.rx_count;
    data->tx_bytes = master->device_stats.tx_bytes;
    data->rx_bytes = master->device_stats.rx_bytes;
    for (j = 0; j < EC_TOOL_RATE_COUNT; j++) {
        data->tx_frame_rates[j] = master->device_stats.tx_frame_rates[j];
        data->rx_frame_rates[j] = master->device_stats.rx_frame_rates[j];
        data->tx_byte_rates[j]  = master->device_stats.tx_byte_rates[j];
        data->rx_byte_rates[j]  = master->device_stats.rx_byte_rates[j];
        data->loss_rates[j]     = master->device_stats.loss_rates[j];
    }

    ec_sem_up(&master->device_sem);

    data->app_time    = master->app_time;
    data->dc_ref_time = master->dc_ref_time;
    data->ref_clock   = master->dc_ref_clock
        ? master->dc_ref_clock->ring_position : 0xffff;

    return 0;
}

/****************************************************************************/
/* Slave                                                                      */
/****************************************************************************/

int ecrt_tool_get_slave(ec_master_t *master, ec_tool_slave_t *data)
{
    const ec_slave_t *slave;
    int i;

    if (!master || !data)
        return -EINVAL;

    if (ec_sem_down_interruptible(&master->master_sem))
        return -EINTR;

    slave = ec_master_find_slave_const(master, 0, data->position);
    if (!slave) {
        ec_sem_up(&master->master_sem);
        return -EINVAL;
    }

    data->device_index           = slave->device_index;
    data->vendor_id              = slave->sii.vendor_id;
    data->product_code           = slave->sii.product_code;
    data->revision_number        = slave->sii.revision_number;
    data->serial_number          = slave->sii.serial_number;
    data->alias                  = slave->effective_alias;
    data->boot_rx_mailbox_offset = slave->sii.boot_rx_mailbox_offset;
    data->boot_rx_mailbox_size   = slave->sii.boot_rx_mailbox_size;
    data->boot_tx_mailbox_offset = slave->sii.boot_tx_mailbox_offset;
    data->boot_tx_mailbox_size   = slave->sii.boot_tx_mailbox_size;
    data->std_rx_mailbox_offset  = slave->sii.std_rx_mailbox_offset;
    data->std_rx_mailbox_size    = slave->sii.std_rx_mailbox_size;
    data->std_tx_mailbox_offset  = slave->sii.std_tx_mailbox_offset;
    data->std_tx_mailbox_size    = slave->sii.std_tx_mailbox_size;
    data->mailbox_protocols      = slave->sii.mailbox_protocols;
    data->has_general_category   = slave->sii.has_general;
    data->coe_details.enable_sdo = slave->sii.coe_details.enable_sdo;
    data->coe_details.enable_sdo_info =
        slave->sii.coe_details.enable_sdo_info;
    data->coe_details.enable_pdo_assign =
        slave->sii.coe_details.enable_pdo_assign;
    data->coe_details.enable_pdo_configuration =
        slave->sii.coe_details.enable_pdo_configuration;
    data->coe_details.enable_upload_at_startup =
        slave->sii.coe_details.enable_upload_at_startup;
    data->coe_details.enable_sdo_complete_access =
        slave->sii.coe_details.enable_sdo_complete_access;
    data->general_flags.enable_safeop =
        slave->sii.general_flags.enable_safeop;
    data->general_flags.enable_not_lrw =
        slave->sii.general_flags.enable_not_lrw;
    data->current_on_ebus        = slave->sii.current_on_ebus;

    for (i = 0; i < EC_MAX_PORTS; i++) {
        data->ports[i].desc         = slave->ports[i].desc;
        data->ports[i].link.link_up = slave->ports[i].link.link_up;
        data->ports[i].link.loop_closed =
            slave->ports[i].link.loop_closed;
        data->ports[i].link.signal_detected =
            slave->ports[i].link.signal_detected;
        data->ports[i].receive_time = slave->ports[i].receive_time;
        data->ports[i].next_slave   = slave->ports[i].next_slave
            ? slave->ports[i].next_slave->ring_position : 0xffff;
        data->ports[i].delay_to_next_dc =
            slave->ports[i].delay_to_next_dc;
    }

    data->fmmu_bit           = slave->base_fmmu_bit_operation;
    data->dc_supported       = slave->base_dc_supported;
    data->dc_range           = (ec_tool_slave_dc_range_t)slave->base_dc_range;
    data->has_dc_system_time = slave->has_dc_system_time;
    data->transmission_delay = slave->transmission_delay;
    data->al_state           = slave->current_state;
    data->error_flag         = slave->error_flag;
    data->sync_count         = slave->sii.sync_count;
    data->sdo_count          = ec_slave_sdo_count(slave);
    data->sii_nwords         = slave->sii_nwords;
    TOOL_STRCPY(data->group, slave->sii.group);
    TOOL_STRCPY(data->image, slave->sii.image);
    TOOL_STRCPY(data->order, slave->sii.order);
    TOOL_STRCPY(data->name,  slave->sii.name);

    ec_sem_up(&master->master_sem);
    return 0;
}

/****************************************************************************/
/* Slave sync manager                                                         */
/****************************************************************************/

int ecrt_tool_get_slave_sync(ec_master_t *master,
        ec_tool_slave_sync_t *data)
{
    const ec_slave_t *slave;
    const ec_sync_t *sync;

    if (!master || !data)
        return -EINVAL;

    if (ec_sem_down_interruptible(&master->master_sem))
        return -EINTR;

    slave = ec_master_find_slave_const(master, 0, data->slave_position);
    if (!slave) {
        ec_sem_up(&master->master_sem);
        return -EINVAL;
    }

    if (data->sync_index >= slave->sii.sync_count) {
        ec_sem_up(&master->master_sem);
        return -EINVAL;
    }

    sync = &slave->sii.syncs[data->sync_index];
    data->physical_start_address = sync->physical_start_address;
    data->default_size           = sync->default_length;
    data->control_register       = sync->control_register;
    data->enable                 = sync->enable;
    data->pdo_count              = ec_pdo_list_count(&sync->pdos);

    ec_sem_up(&master->master_sem);
    return 0;
}

/****************************************************************************/
/* Slave sync manager PDO                                                     */
/****************************************************************************/

int ecrt_tool_get_slave_sync_pdo(ec_master_t *master,
        ec_tool_slave_sync_pdo_t *data)
{
    const ec_slave_t *slave;
    const ec_sync_t *sync;
    const ec_pdo_t *pdo;

    if (!master || !data)
        return -EINVAL;

    if (ec_sem_down_interruptible(&master->master_sem))
        return -EINTR;

    slave = ec_master_find_slave_const(master, 0, data->slave_position);
    if (!slave) {
        ec_sem_up(&master->master_sem);
        return -EINVAL;
    }

    if (data->sync_index >= slave->sii.sync_count) {
        ec_sem_up(&master->master_sem);
        return -EINVAL;
    }

    sync = &slave->sii.syncs[data->sync_index];
    pdo  = ec_pdo_list_find_pdo_by_pos_const(&sync->pdos, data->pdo_pos);
    if (!pdo) {
        ec_sem_up(&master->master_sem);
        return -EINVAL;
    }

    data->index       = pdo->index;
    data->entry_count = ec_pdo_entry_count(pdo);
    TOOL_STRCPY(data->name, pdo->name);

    ec_sem_up(&master->master_sem);
    return 0;
}

/****************************************************************************/
/* Slave sync manager PDO entry                                               */
/****************************************************************************/

int ecrt_tool_get_slave_sync_pdo_entry(ec_master_t *master,
        ec_tool_slave_sync_pdo_entry_t *data)
{
    const ec_slave_t *slave;
    const ec_sync_t *sync;
    const ec_pdo_t *pdo;
    const ec_pdo_entry_t *entry;

    if (!master || !data)
        return -EINVAL;

    if (ec_sem_down_interruptible(&master->master_sem))
        return -EINTR;

    slave = ec_master_find_slave_const(master, 0, data->slave_position);
    if (!slave) {
        ec_sem_up(&master->master_sem);
        return -EINVAL;
    }

    if (data->sync_index >= slave->sii.sync_count) {
        ec_sem_up(&master->master_sem);
        return -EINVAL;
    }

    sync = &slave->sii.syncs[data->sync_index];
    pdo  = ec_pdo_list_find_pdo_by_pos_const(&sync->pdos, data->pdo_pos);
    if (!pdo) {
        ec_sem_up(&master->master_sem);
        return -EINVAL;
    }

    entry = ec_pdo_find_entry_by_pos_const(pdo, data->entry_pos);
    if (!entry) {
        ec_sem_up(&master->master_sem);
        return -EINVAL;
    }

    data->index      = entry->index;
    data->subindex   = entry->subindex;
    data->bit_length = entry->bit_length;
    TOOL_STRCPY(data->name, entry->name);

    ec_sem_up(&master->master_sem);
    return 0;
}

/****************************************************************************/
/* Domain                                                                     */
/****************************************************************************/

int ecrt_tool_get_domain(ec_master_t *master, ec_tool_domain_t *data)
{
    const ec_domain_t *domain;
    unsigned int dev_idx;

    if (!master || !data)
        return -EINVAL;

    if (ec_sem_down_interruptible(&master->master_sem))
        return -EINTR;

    domain = ec_master_find_domain_const(master, data->index);
    if (!domain) {
        ec_sem_up(&master->master_sem);
        return -EINVAL;
    }

    data->data_size     = domain->data_size;
    data->logical_base_address = domain->logical_base_address;
    for (dev_idx = EC_DEVICE_MAIN;
            dev_idx < ec_master_num_devices(domain->master); dev_idx++) {
        if (dev_idx >= EC_TOOL_MAX_NUM_DEVICES)
            break;
        data->working_counter[dev_idx] = domain->working_counter[dev_idx];
    }
    data->expected_working_counter = domain->expected_working_counter;
    data->fmmu_count               = ec_domain_fmmu_count(domain);

    ec_sem_up(&master->master_sem);
    return 0;
}

/****************************************************************************/
/* Domain FMMU                                                                */
/****************************************************************************/

int ecrt_tool_get_domain_fmmu(ec_master_t *master,
        ec_tool_domain_fmmu_t *data)
{
    const ec_domain_t *domain;
    const ec_fmmu_config_t *fmmu;

    if (!master || !data)
        return -EINVAL;

    if (ec_sem_down_interruptible(&master->master_sem))
        return -EINTR;

    domain = ec_master_find_domain_const(master, data->domain_index);
    if (!domain) {
        ec_sem_up(&master->master_sem);
        return -EINVAL;
    }

    fmmu = ec_domain_find_fmmu(domain, data->fmmu_index);
    if (!fmmu) {
        ec_sem_up(&master->master_sem);
        return -EINVAL;
    }

    data->slave_config_alias    = fmmu->sc->alias;
    data->slave_config_position = fmmu->sc->position;
    data->sync_index            = fmmu->sync_index;
    data->dir                   = fmmu->dir;
    data->logical_address       = fmmu->logical_start_address;
    data->data_size             = fmmu->data_size;

    ec_sem_up(&master->master_sem);
    return 0;
}

/****************************************************************************/
/* Domain data                                                                */
/****************************************************************************/

int ecrt_tool_get_domain_data(ec_master_t *master,
        ec_tool_domain_data_t *data)
{
    const ec_domain_t *domain;

    if (!master || !data || !data->target)
        return -EINVAL;

    if (ec_sem_down_interruptible(&master->master_sem))
        return -EINTR;

    domain = ec_master_find_domain_const(master, data->domain_index);
    if (!domain) {
        ec_sem_up(&master->master_sem);
        return -EINVAL;
    }

    if (domain->data_size != data->data_size) {
        ec_sem_up(&master->master_sem);
        return -EFAULT;
    }

    memcpy(data->target, domain->data, data->data_size);

    ec_sem_up(&master->master_sem);
    return 0;
}

/****************************************************************************/
/* Debug level                                                                */
/****************************************************************************/

int ecrt_tool_set_debug(ec_master_t *master, unsigned int level)
{
    if (!master)
        return -EINVAL;
    return ec_master_debug_level(master, level);
}

/****************************************************************************/
/* Rescan                                                                     */
/****************************************************************************/

int ecrt_tool_rescan(ec_master_t *master)
{
    if (!master)
        return -EINVAL;
    master->fsm.rescan_required = 1;
    return 0;
}

/****************************************************************************/
/* Slave state                                                                */
/****************************************************************************/

int ecrt_tool_set_slave_state(ec_master_t *master,
        ec_tool_slave_state_t *data)
{
    ec_slave_t *slave;

    if (!master || !data)
        return -EINVAL;

    if (ec_sem_down_interruptible(&master->master_sem))
        return -EINTR;

    if (data->slave_position == EC_TOOL_SLAVE_POSITION_ALL) {
        for (slave = master->slaves;
                slave < master->slaves + master->slave_count; slave++) {
            ec_slave_request_state(slave, data->al_state);
        }
    } else {
        slave = ec_master_find_slave(master, 0, data->slave_position);
        if (!slave) {
            ec_sem_up(&master->master_sem);
            return -EINVAL;
        }
        ec_slave_request_state(slave, data->al_state);
    }

    ec_sem_up(&master->master_sem);
    return 0;
}

/****************************************************************************/
/* Slave SDO dictionary                                                       */
/****************************************************************************/

int ecrt_tool_get_slave_sdo(ec_master_t *master, ec_tool_slave_sdo_t *data)
{
    const ec_slave_t *slave;
    const ec_sdo_t *sdo;

    if (!master || !data)
        return -EINVAL;

    if (ec_sem_down_interruptible(&master->master_sem))
        return -EINTR;

    slave = ec_master_find_slave_const(master, 0, data->slave_position);
    if (!slave) {
        ec_sem_up(&master->master_sem);
        return -EINVAL;
    }

    sdo = ec_slave_get_sdo_by_pos_const(slave, data->sdo_position);
    if (!sdo) {
        ec_sem_up(&master->master_sem);
        return -EINVAL;
    }

    data->sdo_index    = sdo->index;
    data->max_subindex = sdo->max_subindex;
    TOOL_STRCPY(data->name, sdo->name);

    ec_sem_up(&master->master_sem);
    return 0;
}

/****************************************************************************/
/* Slave SDO entry                                                            */
/****************************************************************************/

int ecrt_tool_get_slave_sdo_entry(ec_master_t *master,
        ec_tool_slave_sdo_entry_t *data)
{
    const ec_slave_t *slave;
    const ec_sdo_t *sdo;
    const ec_sdo_entry_t *entry;

    if (!master || !data)
        return -EINVAL;

    if (ec_sem_down_interruptible(&master->master_sem))
        return -EINTR;

    slave = ec_master_find_slave_const(master, 0, data->slave_position);
    if (!slave) {
        ec_sem_up(&master->master_sem);
        return -EINVAL;
    }

    if (data->sdo_spec <= 0) {
        sdo = ec_slave_get_sdo_by_pos_const(slave,
                (uint16_t)(-data->sdo_spec));
    } else {
        sdo = ec_slave_get_sdo_const(slave, (uint16_t)data->sdo_spec);
    }
    if (!sdo) {
        ec_sem_up(&master->master_sem);
        return -EINVAL;
    }

    entry = ec_sdo_get_entry_const(sdo, data->sdo_entry_subindex);
    if (!entry) {
        ec_sem_up(&master->master_sem);
        return -EINVAL;
    }

    data->data_type  = entry->data_type;
    data->bit_length = entry->bit_length;
    data->read_access[EC_TOOL_SDO_ENTRY_ACCESS_PREOP] =
        entry->read_access[EC_SDO_ENTRY_ACCESS_PREOP];
    data->read_access[EC_TOOL_SDO_ENTRY_ACCESS_SAFEOP] =
        entry->read_access[EC_SDO_ENTRY_ACCESS_SAFEOP];
    data->read_access[EC_TOOL_SDO_ENTRY_ACCESS_OP] =
        entry->read_access[EC_SDO_ENTRY_ACCESS_OP];
    data->write_access[EC_TOOL_SDO_ENTRY_ACCESS_PREOP] =
        entry->write_access[EC_SDO_ENTRY_ACCESS_PREOP];
    data->write_access[EC_TOOL_SDO_ENTRY_ACCESS_SAFEOP] =
        entry->write_access[EC_SDO_ENTRY_ACCESS_SAFEOP];
    data->write_access[EC_TOOL_SDO_ENTRY_ACCESS_OP] =
        entry->write_access[EC_SDO_ENTRY_ACCESS_OP];
    TOOL_STRCPY(data->description, entry->description);

    ec_sem_up(&master->master_sem);
    return 0;
}

/****************************************************************************/
/* SDO upload                                                                 */
/****************************************************************************/

int ecrt_tool_sdo_upload(ec_master_t *master,
        ec_tool_slave_sdo_upload_t *data)
{
    size_t result_size = 0;
    int ret;

    if (!master || !data || !data->target || !data->target_size)
        return -EINVAL;

    ret = ecrt_master_sdo_upload(master, data->slave_position,
            data->sdo_index, data->sdo_entry_subindex,
            data->target, data->target_size,
            &result_size, &data->abort_code);
    data->data_size = (uint32_t)result_size;
    return ret;
}

/****************************************************************************/
/* SDO download                                                               */
/****************************************************************************/

int ecrt_tool_sdo_download(ec_master_t *master,
        ec_tool_slave_sdo_download_t *data)
{
    if (!master || !data || !data->data || !data->data_size)
        return -EINVAL;

    if (data->complete_access) {
        return ecrt_master_sdo_download_complete(master,
                data->slave_position, data->sdo_index,
                data->data, data->data_size,
                &data->abort_code);
    } else {
        return ecrt_master_sdo_download(master,
                data->slave_position, data->sdo_index,
                data->sdo_entry_subindex,
                data->data, data->data_size,
                &data->abort_code);
    }
}

/****************************************************************************/
/* SII read                                                                   */
/****************************************************************************/

int ecrt_tool_sii_read(ec_master_t *master, ec_tool_slave_sii_t *data)
{
    const ec_slave_t *slave;

    if (!master || !data || !data->words || !data->nwords)
        return -EINVAL;

    if (ec_sem_down_interruptible(&master->master_sem))
        return -EINTR;

    slave = ec_master_find_slave_const(master, 0, data->slave_position);
    if (!slave) {
        ec_sem_up(&master->master_sem);
        return -EINVAL;
    }

    if (data->offset + data->nwords > slave->sii_nwords) {
        ec_sem_up(&master->master_sem);
        return -EINVAL;
    }

    memcpy(data->words, slave->sii_words + data->offset, data->nwords * 2);

    ec_sem_up(&master->master_sem);
    return 0;
}

/****************************************************************************/
/* SII write                                                                  */
/****************************************************************************/

int ecrt_tool_sii_write(ec_master_t *master, ec_tool_slave_sii_t *data)
{
    ec_slave_t *slave;
    ec_sii_write_request_t request;
    uint16_t *words;

    if (!master || !data || !data->words || !data->nwords)
        return -EINVAL;

    /* Copy word data to heap so it remains valid during FSM processing. */
    words = malloc((size_t)data->nwords * 2);
    if (!words)
        return -ENOMEM;
    memcpy(words, data->words, (size_t)data->nwords * 2);

    if (ec_sem_down_interruptible(&master->master_sem)) {
        free(words);
        return -EINTR;
    }

    slave = ec_master_find_slave(master, 0, data->slave_position);
    if (!slave) {
        ec_sem_up(&master->master_sem);
        free(words);
        return -EINVAL;
    }

    /* Init SII write request. */
    INIT_LIST_HEAD(&request.list);
    request.slave  = slave;
    request.words  = words;
    request.offset = data->offset;
    request.nwords = data->nwords;
    request.state  = EC_INT_REQUEST_QUEUED;

    /* Schedule SII write request. */
    list_add_tail(&request.list, &master->sii_requests);
    ec_sem_up(&master->master_sem);

    /* Wait for processing through FSM. */
    if (ec_wq_wait_interruptible(master->request_queue,
                request.state != EC_INT_REQUEST_QUEUED)) {
        ec_sem_down(&master->master_sem);
        if (request.state == EC_INT_REQUEST_QUEUED) {
            list_del(&request.list);
            ec_sem_up(&master->master_sem);
            free(words);
            return -EINTR;
        }
        ec_sem_up(&master->master_sem);
    }

    /* Wait until master FSM has finished processing. */
    ec_wq_wait(master->request_queue,
            request.state != EC_INT_REQUEST_BUSY);

    free(words);
    return request.state == EC_INT_REQUEST_SUCCESS ? 0 : -EIO;
}

/****************************************************************************/
/* Register read                                                              */
/****************************************************************************/

int ecrt_tool_reg_read(ec_master_t *master, ec_tool_slave_reg_t *data)
{
    ec_slave_t *slave;
    ec_reg_request_t request;
    int ret;

    if (!master || !data || !data->data || !data->size)
        return -EINVAL;

    ret = ec_reg_request_init(&request, data->size);
    if (ret)
        return ret;

    ret = ecrt_reg_request_read(&request, data->address, data->size);
    if (ret) {
        ec_reg_request_clear(&request);
        return ret;
    }

    if (ec_sem_down_interruptible(&master->master_sem)) {
        ec_reg_request_clear(&request);
        return -EINTR;
    }

    slave = ec_master_find_slave(master, 0, data->slave_position);
    if (!slave) {
        ec_sem_up(&master->master_sem);
        ec_reg_request_clear(&request);
        return -EINVAL;
    }

    list_add_tail(&request.list, &slave->reg_requests);
    ec_sem_up(&master->master_sem);

    /* Wait for processing through FSM. */
    if (ec_wq_wait_interruptible(master->request_queue,
                request.state != EC_INT_REQUEST_QUEUED)) {
        ec_sem_down(&master->master_sem);
        if (request.state == EC_INT_REQUEST_QUEUED) {
            list_del(&request.list);
            ec_sem_up(&master->master_sem);
            ec_reg_request_clear(&request);
            return -EINTR;
        }
        ec_sem_up(&master->master_sem);
    }

    ec_wq_wait(master->request_queue, request.state != EC_INT_REQUEST_BUSY);

    if (request.state == EC_INT_REQUEST_SUCCESS) {
        memcpy(data->data, request.data, data->size);
        ec_reg_request_clear(&request);
        return 0;
    }

    ec_reg_request_clear(&request);
    return -EIO;
}

/****************************************************************************/
/* Register write                                                             */
/****************************************************************************/

int ecrt_tool_reg_write(ec_master_t *master, ec_tool_slave_reg_t *data)
{
    ec_slave_t *slave;
    ec_reg_request_t request;
    int ret;

    if (!master || !data || !data->data || !data->size)
        return -EINVAL;

    ret = ec_reg_request_init(&request, data->size);
    if (ret)
        return ret;

    memcpy(request.data, data->data, data->size);

    ret = ecrt_reg_request_write(&request, data->address, data->size);
    if (ret) {
        ec_reg_request_clear(&request);
        return ret;
    }

    if (ec_sem_down_interruptible(&master->master_sem)) {
        ec_reg_request_clear(&request);
        return -EINTR;
    }

    if (data->emergency) {
        request.ring_position = data->slave_position;
        list_add_tail(&request.list, &master->emerg_reg_requests);
    } else {
        slave = ec_master_find_slave(master, 0, data->slave_position);
        if (!slave) {
            ec_sem_up(&master->master_sem);
            ec_reg_request_clear(&request);
            return -EINVAL;
        }
        list_add_tail(&request.list, &slave->reg_requests);
    }
    ec_sem_up(&master->master_sem);

    /* Wait for processing through FSM. */
    if (ec_wq_wait_interruptible(master->request_queue,
                request.state != EC_INT_REQUEST_QUEUED)) {
        ec_sem_down(&master->master_sem);
        if (request.state == EC_INT_REQUEST_QUEUED) {
            list_del(&request.list);
            ec_sem_up(&master->master_sem);
            ec_reg_request_clear(&request);
            return -EINTR;
        }
        ec_sem_up(&master->master_sem);
    }

    ec_wq_wait(master->request_queue, request.state != EC_INT_REQUEST_BUSY);

    ret = request.state == EC_INT_REQUEST_SUCCESS ? 0 : -EIO;
    ec_reg_request_clear(&request);
    return ret;
}

/****************************************************************************/
/* FoE read                                                                   */
/****************************************************************************/

int ecrt_tool_foe_read(ec_master_t *master, ec_tool_slave_foe_t *data)
{
    ec_foe_request_t request;
    ec_slave_t *slave;
    int ret;

    if (!master || !data || !data->buffer || !data->buffer_size)
        return -EINVAL;

    ec_foe_request_init(&request, (uint8_t *) data->file_name);
    ret = ec_foe_request_alloc(&request, 10000); /* TODO: dynamic */
    if (ret) {
        ec_foe_request_clear(&request);
        return ret;
    }

    ec_foe_request_read(&request);

    if (ec_sem_down_interruptible(&master->master_sem)) {
        ec_foe_request_clear(&request);
        return -EINTR;
    }

    slave = ec_master_find_slave(master, 0, data->slave_position);
    if (!slave) {
        ec_sem_up(&master->master_sem);
        ec_foe_request_clear(&request);
        return -EINVAL;
    }

    list_add_tail(&request.list, &slave->foe_requests);
    ec_sem_up(&master->master_sem);

    /* Wait for processing through FSM. */
    if (ec_wq_wait_interruptible(master->request_queue,
                request.state != EC_INT_REQUEST_QUEUED)) {
        ec_sem_down(&master->master_sem);
        if (request.state == EC_INT_REQUEST_QUEUED) {
            list_del(&request.list);
            ec_sem_up(&master->master_sem);
            ec_foe_request_clear(&request);
            return -EINTR;
        }
        ec_sem_up(&master->master_sem);
    }

    ec_wq_wait(master->request_queue, request.state != EC_INT_REQUEST_BUSY);

    data->result     = request.result;
    data->error_code = request.error_code;

    if (request.state != EC_INT_REQUEST_SUCCESS) {
        data->data_size = 0;
        ec_foe_request_clear(&request);
        return -EIO;
    }

    data->data_size = (uint32_t)request.data_size;
    if (data->data_size > data->buffer_size)
        data->data_size = data->buffer_size;
    memcpy(data->buffer, request.buffer, data->data_size);

    ec_foe_request_clear(&request);
    return 0;
}

/****************************************************************************/
/* FoE write                                                                  */
/****************************************************************************/

int ecrt_tool_foe_write(ec_master_t *master, ec_tool_slave_foe_t *data)
{
    ec_foe_request_t request;
    ec_slave_t *slave;
    int ret;

    if (!master || !data || !data->buffer || !data->buffer_size)
        return -EINVAL;

    ec_foe_request_init(&request, (uint8_t *) data->file_name);
    ret = ec_foe_request_alloc(&request, data->buffer_size);
    if (ret) {
        ec_foe_request_clear(&request);
        return ret;
    }

    memcpy(request.buffer, data->buffer, data->buffer_size);
    request.data_size = data->buffer_size;
    ec_foe_request_write(&request);

    if (ec_sem_down_interruptible(&master->master_sem)) {
        ec_foe_request_clear(&request);
        return -EINTR;
    }

    slave = ec_master_find_slave(master, 0, data->slave_position);
    if (!slave) {
        ec_sem_up(&master->master_sem);
        ec_foe_request_clear(&request);
        return -EINVAL;
    }

    list_add_tail(&request.list, &slave->foe_requests);
    ec_sem_up(&master->master_sem);

    /* Wait for processing through FSM. */
    if (ec_wq_wait_interruptible(master->request_queue,
                request.state != EC_INT_REQUEST_QUEUED)) {
        ec_sem_down(&master->master_sem);
        if (request.state == EC_INT_REQUEST_QUEUED) {
            list_del(&request.list);
            ec_sem_up(&master->master_sem);
            ec_foe_request_clear(&request);
            return -EINTR;
        }
        ec_sem_up(&master->master_sem);
    }

    ec_wq_wait(master->request_queue, request.state != EC_INT_REQUEST_BUSY);

    data->result     = request.result;
    data->error_code = request.error_code;

    ret = request.state == EC_INT_REQUEST_SUCCESS ? 0 : -EIO;
    ec_foe_request_clear(&request);
    return ret;
}

/****************************************************************************/
/* SoE read                                                                   */
/****************************************************************************/

int ecrt_tool_soe_read(ec_master_t *master, ec_tool_slave_soe_read_t *data)
{
    size_t result_size = 0;
    int ret;

    if (!master || !data || !data->data || !data->mem_size)
        return -EINVAL;

    ret = ecrt_master_read_idn(master, data->slave_position,
            data->drive_no, data->idn, data->data, data->mem_size,
            &result_size, &data->error_code);
    data->data_size = (uint32_t)result_size;
    return ret;
}

/****************************************************************************/
/* SoE write                                                                  */
/****************************************************************************/

int ecrt_tool_soe_write(ec_master_t *master, ec_tool_slave_soe_write_t *data)
{
    if (!master || !data || !data->data || !data->data_size)
        return -EINVAL;

    return ecrt_master_write_idn(master, data->slave_position,
            data->drive_no, data->idn, data->data, data->data_size,
            &data->error_code);
}

/****************************************************************************/
/* Config                                                                     */
/****************************************************************************/

int ecrt_tool_get_config(ec_master_t *master, ec_tool_config_t *data)
{
    const ec_slave_config_t *sc;
    uint8_t i;

    if (!master || !data)
        return -EINVAL;

    if (ec_sem_down_interruptible(&master->master_sem))
        return -EINTR;

    sc = ec_master_get_config_const(master, data->config_index);
    if (!sc) {
        ec_sem_up(&master->master_sem);
        return -EINVAL;
    }

    data->alias        = sc->alias;
    data->position     = sc->position;
    data->vendor_id    = sc->vendor_id;
    data->product_code = sc->product_code;

    for (i = 0; i < EC_MAX_SYNC_MANAGERS; i++) {
        data->syncs[i].dir           = sc->sync_configs[i].dir;
        data->syncs[i].watchdog_mode = sc->sync_configs[i].watchdog_mode;
        data->syncs[i].pdo_count     =
            ec_pdo_list_count(&sc->sync_configs[i].pdos);
    }

    data->watchdog_divider   = sc->watchdog_divider;
    data->watchdog_intervals = sc->watchdog_intervals;
    data->sdo_count          = ec_slave_config_sdo_count(sc);
    data->idn_count          = ec_slave_config_idn_count(sc);
    data->flag_count         = ec_slave_config_flag_count(sc);
    data->slave_position     = sc->slave ? sc->slave->ring_position : -1;
    data->dc_assign_activate = sc->dc_assign_activate;
    for (i = 0; i < EC_TOOL_SYNC_SIGNAL_COUNT; i++) {
        data->dc_sync[i].cycle_time = sc->dc_sync[i].cycle_time;
        data->dc_sync[i].shift_time = sc->dc_sync[i].shift_time;
    }

    ec_sem_up(&master->master_sem);
    return 0;
}

/****************************************************************************/
/* Config PDO                                                                 */
/****************************************************************************/

int ecrt_tool_get_config_pdo(ec_master_t *master, ec_tool_config_pdo_t *data)
{
    const ec_slave_config_t *sc;
    const ec_pdo_t *pdo;

    if (!master || !data)
        return -EINVAL;

    if (data->sync_index >= EC_MAX_SYNC_MANAGERS)
        return -EINVAL;

    if (ec_sem_down_interruptible(&master->master_sem))
        return -EINTR;

    sc = ec_master_get_config_const(master, data->config_index);
    if (!sc) {
        ec_sem_up(&master->master_sem);
        return -EINVAL;
    }

    pdo = ec_pdo_list_find_pdo_by_pos_const(
            &sc->sync_configs[data->sync_index].pdos, data->pdo_pos);
    if (!pdo) {
        ec_sem_up(&master->master_sem);
        return -EINVAL;
    }

    data->index       = pdo->index;
    data->entry_count = ec_pdo_entry_count(pdo);
    TOOL_STRCPY(data->name, pdo->name);

    ec_sem_up(&master->master_sem);
    return 0;
}

/****************************************************************************/
/* Config PDO entry                                                           */
/****************************************************************************/

int ecrt_tool_get_config_pdo_entry(ec_master_t *master,
        ec_tool_config_pdo_entry_t *data)
{
    const ec_slave_config_t *sc;
    const ec_pdo_t *pdo;
    const ec_pdo_entry_t *entry;

    if (!master || !data)
        return -EINVAL;

    if (data->sync_index >= EC_MAX_SYNC_MANAGERS)
        return -EINVAL;

    if (ec_sem_down_interruptible(&master->master_sem))
        return -EINTR;

    sc = ec_master_get_config_const(master, data->config_index);
    if (!sc) {
        ec_sem_up(&master->master_sem);
        return -EINVAL;
    }

    pdo = ec_pdo_list_find_pdo_by_pos_const(
            &sc->sync_configs[data->sync_index].pdos, data->pdo_pos);
    if (!pdo) {
        ec_sem_up(&master->master_sem);
        return -EINVAL;
    }

    entry = ec_pdo_find_entry_by_pos_const(pdo, data->entry_pos);
    if (!entry) {
        ec_sem_up(&master->master_sem);
        return -EINVAL;
    }

    data->index      = entry->index;
    data->subindex   = entry->subindex;
    data->bit_length = entry->bit_length;
    TOOL_STRCPY(data->name, entry->name);

    ec_sem_up(&master->master_sem);
    return 0;
}

/****************************************************************************/
/* Config SDO                                                                 */
/****************************************************************************/

int ecrt_tool_get_config_sdo(ec_master_t *master, ec_tool_config_sdo_t *data)
{
    const ec_slave_config_t *sc;
    const ec_sdo_request_t *sdo_req;
    uint32_t copy_size;

    if (!master || !data)
        return -EINVAL;

    if (ec_sem_down_interruptible(&master->master_sem))
        return -EINTR;

    sc = ec_master_get_config_const(master, data->config_index);
    if (!sc) {
        ec_sem_up(&master->master_sem);
        return -EINVAL;
    }

    sdo_req = ec_slave_config_get_sdo_by_pos_const(sc, data->sdo_pos);
    if (!sdo_req) {
        ec_sem_up(&master->master_sem);
        return -EINVAL;
    }

    data->index    = sdo_req->index;
    data->subindex = sdo_req->subindex;
    data->size     = sdo_req->data_size;
    copy_size      = data->size < EC_TOOL_MAX_SDO_DATA_SIZE
                     ? data->size : EC_TOOL_MAX_SDO_DATA_SIZE;
    memcpy(data->data, sdo_req->data, copy_size);
    data->complete_access = sdo_req->complete_access;

    ec_sem_up(&master->master_sem);
    return 0;
}

/****************************************************************************/
/* Config IDN                                                                 */
/****************************************************************************/

int ecrt_tool_get_config_idn(ec_master_t *master, ec_tool_config_idn_t *data)
{
    const ec_slave_config_t *sc;
    const ec_soe_request_t *idn_req;
    uint32_t copy_size;

    if (!master || !data)
        return -EINVAL;

    if (ec_sem_down_interruptible(&master->master_sem))
        return -EINTR;

    sc = ec_master_get_config_const(master, data->config_index);
    if (!sc) {
        ec_sem_up(&master->master_sem);
        return -EINVAL;
    }

    idn_req = ec_slave_config_get_idn_by_pos_const(sc, data->idn_pos);
    if (!idn_req) {
        ec_sem_up(&master->master_sem);
        return -EINVAL;
    }

    data->drive_no = idn_req->drive_no;
    data->idn      = idn_req->idn;
    data->state    = idn_req->al_state;
    data->size     = idn_req->data_size;
    copy_size      = data->size < EC_TOOL_MAX_IDN_DATA_SIZE
                     ? data->size : EC_TOOL_MAX_IDN_DATA_SIZE;
    memcpy(data->data, idn_req->data, copy_size);

    ec_sem_up(&master->master_sem);
    return 0;
}

/****************************************************************************/
/* Config flag                                                                */
/****************************************************************************/

int ecrt_tool_get_config_flag(ec_master_t *master,
        ec_tool_config_flag_t *data)
{
    const ec_slave_config_t *sc;
    const ec_flag_t *flag;
    size_t key_len;

    if (!master || !data)
        return -EINVAL;

    if (ec_sem_down_interruptible(&master->master_sem))
        return -EINTR;

    sc = ec_master_get_config_const(master, data->config_index);
    if (!sc) {
        ec_sem_up(&master->master_sem);
        return -EINVAL;
    }

    flag = ec_slave_config_get_flag_by_pos_const(sc, data->flag_pos);
    if (!flag) {
        ec_sem_up(&master->master_sem);
        return -EINVAL;
    }

    key_len = strlen(flag->key);
    if (key_len >= EC_TOOL_MAX_FLAG_KEY_SIZE)
        key_len = EC_TOOL_MAX_FLAG_KEY_SIZE - 1;
    memcpy(data->key, flag->key, key_len);
    data->key[key_len] = '\0';
    data->value = flag->value;

    ec_sem_up(&master->master_sem);
    return 0;
}

/****************************************************************************/
/* EoE handler                                                                */
/****************************************************************************/

#ifdef EC_EOE

int ecrt_tool_get_eoe_handler(ec_master_t *master,
        ec_tool_eoe_handler_t *data)
{
    const ec_eoe_t *eoe;

    if (!master || !data)
        return -EINVAL;

    if (ec_sem_down_interruptible(&master->master_sem))
        return -EINTR;

    eoe = ec_master_get_eoe_handler_const(master, data->eoe_index);
    if (!eoe) {
        ec_sem_up(&master->master_sem);
        return -EINVAL;
    }

    data->slave_position = eoe->slave
        ? eoe->slave->ring_position : 0xffff;
    snprintf(data->name, EC_TOOL_DATAGRAM_NAME_SIZE, "%s", eoe->dev->name);
    data->open             = eoe->opened;
    data->rx_bytes         = eoe->stats.tx_bytes;
    data->rx_rate          = eoe->tx_rate;
    data->tx_bytes         = eoe->stats.rx_bytes;
    data->tx_rate          = eoe->rx_rate;
    data->tx_queued_frames = eoe->tx_queued_frames;
    data->tx_queue_size    = eoe->tx_queue_size;

    ec_sem_up(&master->master_sem);
    return 0;
}

/****************************************************************************/

int ecrt_tool_set_eoe_ip(ec_master_t *master,
        ec_tool_eoe_ip_t *data)
{
    ec_slave_t *slave;
    ec_eoe_request_t request;

    if (!master || !data)
        return -EINVAL;

    ec_eoe_request_init(&request);

    request.mac_address_included = data->mac_address_included;
    request.ip_address_included  = data->ip_address_included;
    request.subnet_mask_included = data->subnet_mask_included;
    request.gateway_included     = data->gateway_included;
    request.dns_included         = data->dns_included;
    request.name_included        = data->name_included;

    memcpy(request.mac_address, data->mac_address, EC_ETH_ALEN);
    memcpy(&request.ip_address, &data->ip_address, 4);
    memcpy(&request.subnet_mask, &data->subnet_mask, 4);
    memcpy(&request.gateway, &data->gateway, 4);
    memcpy(&request.dns, &data->dns, 4);
    memcpy(request.name, data->name, EC_MAX_HOSTNAME_SIZE);

    request.state = EC_INT_REQUEST_QUEUED;

    if (ec_sem_down_interruptible(&master->master_sem))
        return -EINTR;

    slave = ec_master_find_slave(master, 0, data->slave_position);
    if (!slave) {
        ec_sem_up(&master->master_sem);
        return -EINVAL;
    }

    list_add_tail(&request.list, &slave->eoe_requests);
    ec_sem_up(&master->master_sem);

    /* Wait for processing through FSM. */
    if (ec_wq_wait_interruptible(master->request_queue,
                request.state != EC_INT_REQUEST_QUEUED)) {
        ec_sem_down(&master->master_sem);
        if (request.state == EC_INT_REQUEST_QUEUED) {
            list_del(&request.list);
            ec_sem_up(&master->master_sem);
            return -EINTR;
        }
        ec_sem_up(&master->master_sem);
    }

    ec_wq_wait(master->request_queue, request.state != EC_INT_REQUEST_BUSY);

    data->result = request.result;
    return request.state == EC_INT_REQUEST_SUCCESS ? 0 : -EIO;
}

#endif /* EC_EOE */

/****************************************************************************/
