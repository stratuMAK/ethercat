/*****************************************************************************
 *
 *  Copyright (C) 2006-2023  Florian Pose, Ingenieurgemeinschaft IgH
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

/** \file
 * EtherCAT master state machine.
 */

/****************************************************************************/

#include "master_globals.h"
#include "master.h"
#include "mailbox.h"
#include "slave_config.h"
#ifdef EC_EOE
#include "ethernet.h"
#endif

#include "fsm_master.h"
#include "fsm_foe.h"

/****************************************************************************/

/** Time difference [ns] to tolerate without setting a new system time offset.
 */
#define EC_SYSTEM_TIME_TOLERANCE_NS 1000000

/****************************************************************************/

// prototypes for private methods
void ec_fsm_master_restart(ec_fsm_master_t *);
int ec_fsm_master_action_process_sii(ec_fsm_master_t *);
int ec_fsm_master_action_process_int_request(ec_fsm_master_t *);
void ec_fsm_master_action_idle(ec_fsm_master_t *);
void ec_fsm_master_action_next_slave_state(ec_fsm_master_t *);
uint64_t ec_fsm_master_dc_offset32(ec_fsm_master_t *, uint64_t, uint64_t, unsigned long);
uint64_t ec_fsm_master_dc_offset64(ec_fsm_master_t *, uint64_t, uint64_t, unsigned long);

/****************************************************************************/

void ec_fsm_master_state_start(ec_fsm_master_t *);
void ec_fsm_master_state_broadcast(ec_fsm_master_t *);
void ec_fsm_master_state_read_state(ec_fsm_master_t *);
void ec_fsm_master_state_acknowledge(ec_fsm_master_t *);
void ec_fsm_master_enter_configure_slaves(ec_fsm_master_t *);
void ec_fsm_master_state_configure_slaves(ec_fsm_master_t *);
void ec_fsm_master_state_clear_addresses(ec_fsm_master_t *);
void ec_fsm_master_state_dc_measure_delays(ec_fsm_master_t *);
void ec_fsm_master_state_scan_slave(ec_fsm_master_t *);
void ec_fsm_master_state_dc_read_offset(ec_fsm_master_t *);
void ec_fsm_master_state_dc_write_offset(ec_fsm_master_t *);
void ec_fsm_master_state_assign_sii(ec_fsm_master_t *);
void ec_fsm_master_state_write_sii(ec_fsm_master_t *);
void ec_fsm_master_state_sdo_request(ec_fsm_master_t *);
void ec_fsm_master_state_soe_request(ec_fsm_master_t *);

void ec_fsm_master_enter_clear_addresses(ec_fsm_master_t *);
void ec_fsm_master_enter_write_system_times(ec_fsm_master_t *);

/****************************************************************************/

/** Constructor.
 */
int ec_fsm_master_init(
        ec_fsm_master_t *fsm, /**< Master state machine. */
        ec_master_t *master, /**< EtherCAT master. */
        ec_datagram_t *datagram /**< Datagram object to use. */
        )
{
    int ret;
    unsigned int i;

    fsm->master = master;
    fsm->datagram = datagram;

    // inits the member variables state, idle, dev_idx, link_state,
    // slaves_responding, slave_states and rescan_required
    ec_fsm_master_reset(fsm);

    fsm->retries = 0;
    fsm->scan_time = 0;
    fsm->slave = NULL;
    fsm->sii_request = NULL;
    fsm->sii_index = 0;
    fsm->sdo_request = NULL;
    fsm->soe_request = NULL;

    // init sub-state-machines
    ec_fsm_coe_init(&fsm->fsm_coe);
    ec_fsm_soe_init(&fsm->fsm_soe);
    ec_fsm_pdo_init(&fsm->fsm_pdo, &fsm->fsm_coe);
#ifdef EC_EOE
    ec_fsm_eoe_init(&fsm->fsm_eoe);
#endif
    ec_fsm_change_init(&fsm->fsm_change);
    fsm->fsm_change.datagram = fsm->datagram;

    // init slave config FSM pool with per-slot datagrams
    for (i = 0; i < EC_FSM_SLAVE_CONFIG_POOL_SIZE; i++) {
        ec_fsm_slave_config_init(&fsm->config_slots[i].fsm);
        ec_datagram_init(&fsm->config_slots[i].datagram);
        snprintf(fsm->config_slots[i].datagram.name,
                EC_DATAGRAM_NAME_SIZE, "config-slot-%u", i);
        ret = ec_datagram_prealloc(&fsm->config_slots[i].datagram,
                EC_MAX_DATA_SIZE);
        if (ret < 0) {
            EC_MASTER_ERR(master,
                    "Failed to allocate config slot %u datagram.\n", i);
            goto out_clear_slots;
        }
        fsm->config_slots[i].in_use = 0;
    }

    // Slave scan permanently owns slot 0's FSM. The config pool allocator
    // starts at slot 1, so slot 0 is structurally reserved and never shared.
    ec_fsm_slave_scan_init(&fsm->fsm_slave_scan, fsm->datagram,
            &fsm->config_slots[0].fsm, &fsm->fsm_pdo);
    ec_fsm_sii_init(&fsm->fsm_sii, fsm->datagram);

    return 0;

out_clear_slots:
    while (i > 0) {
        i--;
        ec_datagram_clear(&fsm->config_slots[i].datagram);
        ec_fsm_slave_config_clear(&fsm->config_slots[i].fsm);
    }
    ec_fsm_change_clear(&fsm->fsm_change);
#ifdef EC_EOE
    ec_fsm_eoe_clear(&fsm->fsm_eoe);
#endif
    ec_fsm_pdo_clear(&fsm->fsm_pdo);
    ec_fsm_soe_clear(&fsm->fsm_soe);
    ec_fsm_coe_clear(&fsm->fsm_coe);
    return ret;
}

/****************************************************************************/

/** Destructor.
 */
void ec_fsm_master_clear(
        ec_fsm_master_t *fsm /**< Master state machine. */
        )
{
    // clear sub-state machines
    ec_fsm_coe_clear(&fsm->fsm_coe);
    ec_fsm_soe_clear(&fsm->fsm_soe);
    ec_fsm_pdo_clear(&fsm->fsm_pdo);
#ifdef EC_EOE
    ec_fsm_eoe_clear(&fsm->fsm_eoe);
#endif
    ec_fsm_change_clear(&fsm->fsm_change);

    // clear slave config FSM pool
    {
        unsigned int i;
        for (i = 0; i < EC_FSM_SLAVE_CONFIG_POOL_SIZE; i++) {
            ec_fsm_slave_config_clear(&fsm->config_slots[i].fsm);
            ec_datagram_clear(&fsm->config_slots[i].datagram);
        }
    }

    ec_fsm_slave_scan_clear(&fsm->fsm_slave_scan);
    ec_fsm_sii_clear(&fsm->fsm_sii);
}

/****************************************************************************/

/** Check if a slave is already assigned to an active config slot.
 *
 * \return 1 if slave is in a slot, 0 otherwise.
 */
static int ec_fsm_master_slave_in_slot(
        ec_fsm_master_t *fsm, /**< Master state machine. */
        ec_slave_t *slave /**< Slave to check. */
        )
{
    unsigned int i;

    for (i = 1; i < EC_FSM_SLAVE_CONFIG_POOL_SIZE; i++) {
        if (fsm->config_slots[i].in_use
                && fsm->config_slots[i].fsm.slave == slave) {
            return 1;
        }
    }
    return 0;
}

/****************************************************************************/

/** Try to fill a free config slot with the next unconfigured slave.
 *
 * Scans all slaves for one that needs configuration and is not already
 * assigned to a slot. If found, allocates the slot and starts the FSM.
 *
 * \return 1 if a slave was started, 0 if no slave needs config.
 */
static int ec_fsm_master_fill_config_slot(
        ec_fsm_master_t *fsm, /**< Master state machine. */
        ec_fsm_slave_config_slot_t *slot /**< Free slot to fill. */
        )
{
    ec_master_t *master = fsm->master;
    ec_slave_t *slave;

    for (slave = master->slaves;
            slave < master->slaves + master->slave_count;
            slave++) {
        if ((slave->current_state != slave->requested_state
                    || slave->force_config) && !slave->error_flag
                && !ec_fsm_master_slave_in_slot(fsm, slave)) {

            /* The slave's own FSM may be in the middle of a mailbox transfer
             * (an application request or a background dictionary fetch).
             * Configuration uses the same mailbox, so defer this slave until
             * its FSM is idle again; the other slaves are unaffected. */
            if (ec_fsm_slave_is_busy(&slave->fsm)) {
                EC_SLAVE_DBG(slave, 1, "Deferring configuration:"
                        " slave FSM is busy.\n");
                continue;
            }

            if (master->debug_level) {
                char old_state[EC_STATE_STRING_SIZE],
                     new_state[EC_STATE_STRING_SIZE];
                ec_state_string(slave->current_state, old_state, 0);
                ec_state_string(slave->requested_state, new_state, 0);
                EC_SLAVE_DBG(slave, 1, "Changing state from %s to %s%s.\n",
                        old_state, new_state,
                        slave->force_config ? " (forced)" : "");
            }

            slot->in_use = 1;
            ec_fsm_slave_config_start(&slot->fsm, slave);
            slot->datagram.device_index = slave->device_index;
            ec_fsm_slave_config_exec(&slot->fsm, &slot->datagram);
            return 1;
        }
    }
    return 0;
}

/****************************************************************************/

/** Queue the FSM datagram(s) for sending, respecting frame size limits.
 *
 * For non-config states, the master fsm_datagram is always queued (small).
 * For config states, the slot datagram is checked against max_queue_size.
 * If it doesn't fit, it is not queued and will be retried next cycle.
 *
 * \return 1 if a datagram was queued, 0 otherwise.
 */
int ec_fsm_master_queue_datagram(
        ec_fsm_master_t *fsm /**< Master state machine. */
        )
{
    ec_master_t *master = fsm->master;

    if (fsm->state == ec_fsm_master_state_configure_slaves) {
        ec_datagram_t *queued;
        ec_fsm_slave_config_slot_t *slot;
        size_t queue_size = 0;
        unsigned int i;
        int queued_any = 0;

        // sum current queue usage
        list_for_each_entry(queued, &master->datagram_queue, queue) {
            if (queued->state == EC_DATAGRAM_QUEUED) {
                queue_size += queued->data_size;
            }
        }

        // Queue each active slot's datagram if it fits.
        // Returns 0 if nothing was queued (all datagrams already in flight
        // or none fit); this is normal during parallel config.
        for (i = 1; i < EC_FSM_SLAVE_CONFIG_POOL_SIZE; i++) {
            slot = &fsm->config_slots[i];
            if (!slot->in_use) {
                continue;
            }
            // only queue if in INIT state (needs sending)
            if (slot->datagram.state != EC_DATAGRAM_INIT) {
                continue;
            }
            if (queue_size + slot->datagram.data_size
                    > master->max_queue_size) {
                // doesn't fit — skip, will retry next cycle
                continue;
            }
            ec_master_queue_datagram(master, &slot->datagram);
            queue_size += slot->datagram.data_size;
            queued_any = 1;
        }

        return queued_any;
    }

    // non-config: always queue the master FSM datagram
    ec_master_queue_datagram(master, fsm->datagram);
    return 1;
}

/****************************************************************************/

/** Reset state machine.
 */
void ec_fsm_master_reset(
        ec_fsm_master_t *fsm /**< Master state machine. */
        )
{
    ec_device_index_t dev_idx;

    fsm->state = ec_fsm_master_state_start;
    fsm->idle = 0;
    fsm->dev_idx = EC_DEVICE_MAIN;

    for (dev_idx = EC_DEVICE_MAIN;
            dev_idx < ec_master_num_devices(fsm->master); dev_idx++) {
        fsm->link_state[dev_idx] = 0;
        fsm->slaves_responding[dev_idx] = 0;
        fsm->slave_states[dev_idx] = EC_SLAVE_STATE_UNKNOWN;
    }

    fsm->rescan_required = 0;
}

/****************************************************************************/

/** Executes the current state of the state machine.
 *
 * If the state machine's datagram is not sent or received yet, the execution
 * of the state machine is delayed to the next cycle.
 *
 * \return true, if the state machine was executed
 */
int ec_fsm_master_exec(
        ec_fsm_master_t *fsm /**< Master state machine. */
        )
{
    // In parallel config mode, the state function itself checks per-slot
    // datagram states. Just check the master datagram for non-config states.
    if (fsm->state != ec_fsm_master_state_configure_slaves) {
        ec_datagram_t *datagram = fsm->datagram;

        if (datagram->state == EC_DATAGRAM_SENT
            || datagram->state == EC_DATAGRAM_QUEUED) {
            // datagram was not sent or received yet.
            return 0;
        }
    }

    fsm->state(fsm);
    return 1;
}

/****************************************************************************/

/**
 * \return true, if the state machine is in an idle phase
 */
int ec_fsm_master_idle(
        const ec_fsm_master_t *fsm /**< Master state machine. */
        )
{
    return fsm->idle;
}

/****************************************************************************/

/** Restarts the master state machine.
 */
void ec_fsm_master_restart(
        ec_fsm_master_t *fsm /**< Master state machine. */
        )
{
    fsm->dev_idx = EC_DEVICE_MAIN;
    fsm->state = ec_fsm_master_state_start;
    fsm->state(fsm); // execute immediately
}

/*****************************************************************************
 * Master state machine
 ****************************************************************************/

/** Master state: START.
 *
 * Starts with getting slave count and slave states.
 */
void ec_fsm_master_state_start(
        ec_fsm_master_t *fsm /**< Master state machine. */
        )
{
    ec_master_t *master = fsm->master;

    fsm->idle = 1;

    // check for emergency requests
    if (!list_empty(&master->emerg_reg_requests)) {
        ec_reg_request_t *request;

        // get first request
        request = list_entry(master->emerg_reg_requests.next,
                ec_reg_request_t, list);
        list_del_init(&request->list); // dequeue
        request->state = EC_INT_REQUEST_BUSY;

        if (request->transfer_size > fsm->datagram->mem_size) {
            EC_MASTER_ERR(master, "Emergency request data too large!\n");
            request->state = EC_INT_REQUEST_FAILURE;
            ec_wq_wake_all(&master->request_queue);
            fsm->state(fsm); // continue
            return;
        }

        if (request->dir != EC_DIR_OUTPUT) {
            EC_MASTER_ERR(master, "Emergency requests must be"
                    " write requests!\n");
            request->state = EC_INT_REQUEST_FAILURE;
            ec_wq_wake_all(&master->request_queue);
            fsm->state(fsm); // continue
            return;
        }

        EC_MASTER_DBG(master, 1, "Writing emergency register request...\n");
        ec_datagram_apwr(fsm->datagram, request->ring_position,
                request->address, request->transfer_size);
        memcpy(fsm->datagram->data, request->data, request->transfer_size);
        fsm->datagram->device_index = EC_DEVICE_MAIN;
        request->state = EC_INT_REQUEST_SUCCESS;
        ec_wq_wake_all(&master->request_queue);
        return;
    }

    ec_datagram_brd(fsm->datagram, 0x0130, 2);
    ec_datagram_zero(fsm->datagram);
    fsm->datagram->device_index = fsm->dev_idx;
    fsm->retries = EC_FSM_RETRIES;
    fsm->state = ec_fsm_master_state_broadcast;
}

/****************************************************************************/

/** Master state: BROADCAST.
 *
 * Processes the broadcast read slave count and slaves states.
 */
void ec_fsm_master_state_broadcast(
        ec_fsm_master_t *fsm /**< Master state machine. */
        )
{
    ec_datagram_t *datagram = fsm->datagram;
    unsigned int i, size;
    ec_slave_t *slave;
    ec_master_t *master = fsm->master;

    if (datagram->state == EC_DATAGRAM_TIMED_OUT && fsm->retries--) {
        return;
    }

    // bus topology change?
    if (datagram->working_counter != fsm->slaves_responding[fsm->dev_idx]) {
        fsm->rescan_required = 1;
        fsm->slaves_responding[fsm->dev_idx] = datagram->working_counter;
        EC_MASTER_INFO(master, "%u slave(s) responding on %s device. "
                "Re-scanning on next possibility.\n",
                fsm->slaves_responding[fsm->dev_idx],
                ec_device_names[fsm->dev_idx != 0]);
    }

    if (fsm->link_state[fsm->dev_idx] &&
            !master->devices[fsm->dev_idx].link_state) {
        ec_device_index_t dev_idx;

        EC_MASTER_DBG(master, 1, "Master state machine detected "
                "link down on %s device. Clearing slave list.\n",
                ec_device_names[fsm->dev_idx != 0]);

#ifdef EC_EOE
        ec_master_eoe_stop(master);
        ec_master_clear_eoe_handlers(master);
#endif
        ec_master_clear_slaves(master);

        for (dev_idx = EC_DEVICE_MAIN;
                dev_idx < ec_master_num_devices(master); dev_idx++) {
            fsm->slave_states[dev_idx] = 0x00;
            fsm->slaves_responding[dev_idx] = 0; /* Reset to trigger rescan on
                                                    next link up. */
        }
    }
    fsm->link_state[fsm->dev_idx] = master->devices[fsm->dev_idx].link_state;

    if (datagram->state == EC_DATAGRAM_RECEIVED &&
            fsm->slaves_responding[fsm->dev_idx]) {
        uint8_t states = EC_READ_U8(datagram->data);
        if (states != fsm->slave_states[fsm->dev_idx]) {
            // slave states changed
            char state_str[EC_STATE_STRING_SIZE];
            fsm->slave_states[fsm->dev_idx] = states;
            ec_state_string(states, state_str, 1);
            EC_MASTER_INFO(master, "Slave states on %s device: %s.\n",
                    ec_device_names[fsm->dev_idx != 0], state_str);
        }
    } else {
        fsm->slave_states[fsm->dev_idx] = 0x00;
    }

    fsm->dev_idx++;
    if (fsm->dev_idx < ec_master_num_devices(master)) {
        // check number of responding slaves on next device
        fsm->state = ec_fsm_master_state_start;
        fsm->state(fsm); // execute immediately
        return;
    }

    if (fsm->rescan_required) {
        ec_sem_down(&master->scan_sem);
        if (!master->allow_scan) {
            ec_sem_up(&master->scan_sem);
        } else {
            unsigned int count = 0, next_dev_slave, ring_position;
            ec_device_index_t dev_idx;

            master->scan_busy = 1;
            master->scan_index = 0;
            ec_sem_up(&master->scan_sem);

            EC_MASTER_INFO(master, "Re-scanning now.\n");

            // clear all slaves and scan the bus
            fsm->rescan_required = 0;
            fsm->idle = 0;
            fsm->scan_time = ec_current_time();
            fsm->scan_failures = 0;
            fsm->scan_failed[0] = '\0';
            master->scan_progress++;
            if (!master->initial_scan_done) {
                master->initial_scan_attempts++;
            }

#ifdef EC_EOE
            ec_master_eoe_stop(master);
            ec_master_clear_eoe_handlers(master);
#endif
            ec_master_clear_slaves(master);

            for (dev_idx = EC_DEVICE_MAIN;
                    dev_idx < ec_master_num_devices(master); dev_idx++) {
                count += fsm->slaves_responding[dev_idx];
            }

            if (!count) {
                // no slaves present -> finish state machine.
                master->scan_busy = 0;
                master->initial_scan_done = 1;
                ec_wq_wake_interruptible(&master->scan_queue);
                ec_fsm_master_restart(fsm);
                return;
            }

            size = sizeof(ec_slave_t) * count;
            if (!(master->slaves =
                        (ec_slave_t *) ec_alloc(size))) {
                EC_MASTER_ERR(master, "Failed to allocate %u bytes"
                        " of slave memory!\n", size);
                master->scan_busy = 0;
                master->initial_scan_done = 1;
                ec_wq_wake_interruptible(&master->scan_queue);
                ec_fsm_master_restart(fsm);
                return;
            }

            // init slaves
            dev_idx = EC_DEVICE_MAIN;
            next_dev_slave = fsm->slaves_responding[dev_idx];
            ring_position = 0;
            for (i = 0; i < count; i++, ring_position++) {
                slave = master->slaves + i;
                while (i >= next_dev_slave) {
                    dev_idx++;
                    next_dev_slave += fsm->slaves_responding[dev_idx];
                    ring_position = 0;
                }

                ec_slave_init(slave, master, dev_idx, ring_position, i + 1);

                // do not force reconfiguration in operation phase to avoid
                // unnecesssary process data interruptions
                if (master->phase != EC_OPERATION) {
                    slave->force_config = 1;
                }
            }
            master->slave_count = count;
            master->fsm_slave = master->slaves;

            /* start with first device with slaves responding; at least one
             * has responding slaves, otherwise count would be zero. */
            fsm->dev_idx = EC_DEVICE_MAIN;
            while (!fsm->slaves_responding[fsm->dev_idx]) {
                fsm->dev_idx++;
            }

            ec_fsm_master_enter_clear_addresses(fsm);
            return;
        }
    }

    if (master->slave_count) {

        // application applied configurations
        if (master->config_changed) {
            master->config_changed = 0;

            EC_MASTER_DBG(master, 1, "Configuration changed.\n");

            fsm->slave = master->slaves; // begin with first slave
            ec_fsm_master_enter_write_system_times(fsm);

        } else {
            // fetch state from first slave
            fsm->slave = master->slaves;
            ec_datagram_fprd(fsm->datagram, fsm->slave->station_address,
                    0x0130, 2);
            ec_datagram_zero(datagram);
            fsm->datagram->device_index = fsm->slave->device_index;
            fsm->retries = EC_FSM_RETRIES;
            fsm->state = ec_fsm_master_state_read_state;
        }
    } else {
        /* No slaves found.  Only declare the initial scan complete if the
         * broadcast datagram was actually received (link was up).  If the
         * datagram was errored (link down) we haven't actually scanned yet
         * — keep retrying until the link comes up. */
        if (datagram->state == EC_DATAGRAM_RECEIVED) {
            if (!master->initial_scan_done) {
                master->initial_scan_done = 1;
                ec_wq_wake_interruptible(&master->scan_queue);
            }
        }
        ec_fsm_master_restart(fsm);
    }
}

/****************************************************************************/

/** Check for pending SII write requests and process one.
 *
 * \return non-zero, if an SII write request is processed.
 */
int ec_fsm_master_action_process_sii(
        ec_fsm_master_t *fsm /**< Master state machine. */
        )
{
    ec_master_t *master = fsm->master;
    ec_sii_write_request_t *request;
    ec_slave_config_t *config;
    ec_flag_t *flag;
    int assign_to_pdi;

    // search the first request to be processed
    while (1) {
        if (list_empty(&master->sii_requests))
            break;

        // get first request
        request = list_entry(master->sii_requests.next,
                ec_sii_write_request_t, list);
        list_del_init(&request->list); // dequeue
        request->state = EC_INT_REQUEST_BUSY;

        assign_to_pdi = 0;
        config = request->slave->config;
        if (config) {
            flag = ec_slave_config_find_flag(config, "AssignToPdi");
            if (flag) {
                assign_to_pdi = flag->value;
            }
        }

        if (assign_to_pdi) {
            fsm->sii_request = request;
            EC_SLAVE_DBG(request->slave, 1,
                    "Assigning SII back to EtherCAT.\n");
            ec_datagram_fpwr(fsm->datagram, request->slave->station_address,
                    0x0500, 0x01);
            EC_WRITE_U8(fsm->datagram->data, 0x00); // EtherCAT
            fsm->retries = EC_FSM_RETRIES;
            fsm->state = ec_fsm_master_state_assign_sii;
            return 1;
        }

        // found pending SII write operation. execute it!
        EC_SLAVE_DBG(request->slave, 1, "Writing SII data...\n");
        fsm->sii_request = request;
        fsm->sii_index = 0;
        ec_fsm_sii_write(&fsm->fsm_sii, request->slave, request->offset,
                request->words, EC_FSM_SII_USE_CONFIGURED_ADDRESS);
        fsm->state = ec_fsm_master_state_write_sii;
        fsm->state(fsm); // execute immediately
        return 1;
    }

    return 0;
}

/****************************************************************************/

/** Check for pending internal SDO/SoE requests and process one.
 *
 * \return non-zero, if an SDO request is processed.
 */
int ec_fsm_master_action_process_int_request(
        ec_fsm_master_t *fsm /**< Master state machine. */
        )
{
    ec_master_t *master = fsm->master;
    ec_slave_t *slave;
    ec_sdo_request_t *sdo_req;
    ec_soe_request_t *soe_req;

    // search for internal requests to be processed
    for (slave = master->slaves;
            slave < master->slaves + master->slave_count;
            slave++) {

        if (!slave->config) {
            continue;
        }

        list_for_each_entry(sdo_req, &slave->config->sdo_requests, list) {
            if (sdo_req->state == EC_INT_REQUEST_QUEUED) {

                if (ec_sdo_request_timed_out(sdo_req)) {
                    sdo_req->state = EC_INT_REQUEST_FAILURE;
                    EC_SLAVE_DBG(slave, 1, "Internal SDO request"
                            " timed out.\n");
                    continue;
                }

                if (slave->current_state == EC_SLAVE_STATE_INIT) {
                    sdo_req->state = EC_INT_REQUEST_FAILURE;
                    continue;
                }

                sdo_req->state = EC_INT_REQUEST_BUSY;
                EC_SLAVE_DBG(slave, 1, "Processing internal"
                        " SDO request...\n");
                fsm->idle = 0;
                fsm->sdo_request = sdo_req;
                fsm->slave = slave;
                fsm->state = ec_fsm_master_state_sdo_request;
                ec_fsm_coe_transfer(&fsm->fsm_coe, slave, sdo_req);
                ec_fsm_coe_exec(&fsm->fsm_coe, fsm->datagram);
                return 1;
            }
        }

        list_for_each_entry(soe_req, &slave->config->soe_requests, list) {
            if (soe_req->state == EC_INT_REQUEST_QUEUED) {

                if (ec_soe_request_timed_out(soe_req)) {
                    soe_req->state = EC_INT_REQUEST_FAILURE;
                    EC_SLAVE_DBG(slave, 1, "Internal SoE request"
                            " timed out.\n");
                    continue;
                }

                if (slave->current_state == EC_SLAVE_STATE_INIT) {
                    soe_req->state = EC_INT_REQUEST_FAILURE;
                    continue;
                }

                soe_req->state = EC_INT_REQUEST_BUSY;
                EC_SLAVE_DBG(slave, 1, "Processing internal"
                        " SoE request...\n");
                fsm->idle = 0;
                fsm->soe_request = soe_req;
                fsm->slave = slave;
                fsm->state = ec_fsm_master_state_soe_request;
                ec_fsm_soe_transfer(&fsm->fsm_soe, slave, soe_req);
                ec_fsm_soe_exec(&fsm->fsm_soe, fsm->datagram);
                return 1;
            }
        }
    }
    return 0;
}

/****************************************************************************/

/** Master action: IDLE.
 *
 * Does secondary work.
 */
void ec_fsm_master_action_idle(
        ec_fsm_master_t *fsm /**< Master state machine. */
        )
{
    ec_master_t *master = fsm->master;
    ec_slave_t *slave;

    // Check for pending internal SDO or SoE requests
    if (ec_fsm_master_action_process_int_request(fsm)) {
        return;
    }

    // enable processing of requests
    for (slave = master->slaves;
            slave < master->slaves + master->slave_count;
            slave++) {
        ec_fsm_slave_set_ready(&slave->fsm);
    }

    /* Reaching this action means no slave needs configuration, so the slave
     * FSMs may now fetch SDO dictionaries in the background (see
     * ec_fsm_slave_action_process_dict()).  A dictionary fetch is a long
     * sequence of mailbox transfers; running it here would block the master
     * FSM -- and with it the AL status broadcast, the per-slave state poll
     * and slave configuration -- for its entire duration.  The permission is
     * withdrawn again in ec_fsm_master_enter_configure_slaves(). */
    master->allow_sdo_dict = 1;

    // check for pending SII write operations.
    if (ec_fsm_master_action_process_sii(fsm)) {
        return; // SII write request found
    }

    ec_fsm_master_restart(fsm);
}

/****************************************************************************/

/** Master action: Get state of next slave.
 */
void ec_fsm_master_action_next_slave_state(
        ec_fsm_master_t *fsm /**< Master state machine. */
        )
{
    ec_master_t *master = fsm->master;

    // is there another slave to query?
    fsm->slave++;
    if (fsm->slave < master->slaves + master->slave_count) {
        // fetch state from next slave
        fsm->idle = 1;
        ec_datagram_fprd(fsm->datagram,
                fsm->slave->station_address, 0x0130, 2);
        ec_datagram_zero(fsm->datagram);
        fsm->datagram->device_index = fsm->slave->device_index;
        fsm->retries = EC_FSM_RETRIES;
        fsm->state = ec_fsm_master_state_read_state;
        return;
    }

    // all slaves' states read — enter configuration phase
    ec_fsm_master_enter_configure_slaves(fsm);
}

/****************************************************************************/

/****************************************************************************/

/** Master state: READ STATE.
 *
 * Fetches the AL state of a slave.
 */
void ec_fsm_master_state_read_state(
        ec_fsm_master_t *fsm /**< Master state machine. */
        )
{
    ec_slave_t *slave = fsm->slave;
    ec_datagram_t *datagram = fsm->datagram;

    if (datagram->state == EC_DATAGRAM_TIMED_OUT && fsm->retries--) {
        return;
    }

    if (datagram->state != EC_DATAGRAM_RECEIVED) {
        EC_SLAVE_ERR(slave, "Failed to receive AL state datagram: Datagram %s.\n", ec_datagram_state_str(datagram));
        ec_fsm_master_restart(fsm);
        return;
    }

    // did the slave not respond to its station address?
    if (datagram->working_counter != 1) {
        if (!slave->error_flag) {
            slave->error_flag = 1;
            EC_SLAVE_DBG(slave, 1, "Slave did not respond to state query.\n");
        }
        ec_fsm_master_restart(fsm);
        return;
    }

    // A single slave responded
    ec_slave_set_state(slave, EC_READ_U8(datagram->data));

    if (!slave->error_flag) {
        // Check, if new slave state has to be acknowledged
        if (slave->current_state & EC_SLAVE_STATE_ACK_ERR) {
            fsm->idle = 0;
            fsm->state = ec_fsm_master_state_acknowledge;
            fsm->fsm_change.datagram = fsm->datagram;
            ec_fsm_change_ack(&fsm->fsm_change, slave);
            fsm->state(fsm); // execute immediately
            return;
        }
    }

    // Move to next slave (configuration deferred to after all states read)
    ec_fsm_master_action_next_slave_state(fsm);
}

/****************************************************************************/

/** Master state: ACKNOWLEDGE.
 */
void ec_fsm_master_state_acknowledge(
        ec_fsm_master_t *fsm /**< Master state machine. */
        )
{
    ec_slave_t *slave = fsm->slave;

    if (ec_fsm_change_exec(&fsm->fsm_change)) {
        return;
    }

    if (!ec_fsm_change_success(&fsm->fsm_change)) {
        fsm->slave->error_flag = 1;
        EC_SLAVE_ERR(slave, "Failed to acknowledge state change.\n");
    }

    // Move to next slave (configuration deferred to after all states read)
    ec_fsm_master_action_next_slave_state(fsm);
}

/****************************************************************************/

/** Start clearing slave addresses.
 */
void ec_fsm_master_enter_clear_addresses(
        ec_fsm_master_t *fsm /**< Master state machine. */
        )
{
    // broadcast clear all station addresses
    ec_datagram_bwr(fsm->datagram, 0x0010, 2);
    EC_WRITE_U16(fsm->datagram->data, 0x0000);
    fsm->datagram->device_index = fsm->dev_idx;
    fsm->retries = EC_FSM_RETRIES;
    fsm->state = ec_fsm_master_state_clear_addresses;
}

/****************************************************************************/

/** Master state: CLEAR ADDRESSES.
 */
void ec_fsm_master_state_clear_addresses(
        ec_fsm_master_t *fsm /**< Master state machine. */
        )
{
    ec_master_t *master = fsm->master;
    ec_datagram_t *datagram = fsm->datagram;

    if (datagram->state == EC_DATAGRAM_TIMED_OUT && fsm->retries--) {
        return;
    }

    if (datagram->state != EC_DATAGRAM_RECEIVED) {
        EC_MASTER_ERR(master, "Failed to receive address"
                " clearing datagram on %s link: Datagram %s.\n",
                ec_device_names[fsm->dev_idx != 0],
                ec_datagram_state_str(datagram));
        master->scan_busy = 0;
        master->scan_index = master->slave_count;
        ec_wq_wake_interruptible(&master->scan_queue);
        ec_fsm_master_restart(fsm);
        return;
    }

    if (datagram->working_counter != fsm->slaves_responding[fsm->dev_idx]) {
        EC_MASTER_WARN(master, "Failed to clear station addresses on %s link:"
                " Cleared %u of %u",
                ec_device_names[fsm->dev_idx != 0], datagram->working_counter,
                fsm->slaves_responding[fsm->dev_idx]);
    }

    EC_MASTER_DBG(master, 1, "Sending broadcast-write"
            " to measure transmission delays on %s link.\n",
            ec_device_names[fsm->dev_idx != 0]);

    ec_datagram_bwr(datagram, 0x0900, 1);
    ec_datagram_zero(datagram);
    fsm->datagram->device_index = fsm->dev_idx;
    fsm->retries = EC_FSM_RETRIES;
    fsm->state = ec_fsm_master_state_dc_measure_delays;
}

/****************************************************************************/

/** Master state: DC MEASURE DELAYS.
 */
void ec_fsm_master_state_dc_measure_delays(
        ec_fsm_master_t *fsm /**< Master state machine. */
        )
{
    ec_master_t *master = fsm->master;
    ec_datagram_t *datagram = fsm->datagram;

    if (datagram->state == EC_DATAGRAM_TIMED_OUT && fsm->retries--) {
        return;
    }

    if (datagram->state != EC_DATAGRAM_RECEIVED) {
        EC_MASTER_ERR(master, "Failed to receive delay measuring datagram"
                " on %s link: Datagram %s.\n",
                ec_device_names[fsm->dev_idx != 0],
                ec_datagram_state_str(datagram));
        master->scan_busy = 0;
        master->scan_index = master->slave_count;
        master->initial_scan_done = 1;
        ec_wq_wake_interruptible(&master->scan_queue);
        ec_fsm_master_restart(fsm);
        return;
    }

    EC_MASTER_DBG(master, 1, "%u slaves responded to delay measuring"
            " on %s link.\n",
            datagram->working_counter, ec_device_names[fsm->dev_idx != 0]);

    do {
        fsm->dev_idx++;
    } while (fsm->dev_idx < ec_master_num_devices(master) &&
            !fsm->slaves_responding[fsm->dev_idx]);
    if (fsm->dev_idx < ec_master_num_devices(master)) {
        ec_fsm_master_enter_clear_addresses(fsm);
        return;
    }

    EC_MASTER_INFO(master, "Scanning bus.\n");

    // begin scanning of slaves
    fsm->slave = master->slaves;
    master->scan_index = 0;
    EC_MASTER_DBG(master, 1, "Scanning slave %u on %s link.\n",
            fsm->slave->ring_position,
            ec_device_names[fsm->slave->device_index != 0]);
    fsm->state = ec_fsm_master_state_scan_slave;
    ec_fsm_slave_scan_start(&fsm->fsm_slave_scan, fsm->slave);
    ec_fsm_slave_scan_exec(&fsm->fsm_slave_scan); // execute immediately
    fsm->datagram->device_index = fsm->slave->device_index;
}

/****************************************************************************/

/** Master state: SCAN SLAVE.
 *
 * Executes the sub-statemachine for the scanning of a slave.
 */
void ec_fsm_master_state_scan_slave(
        ec_fsm_master_t *fsm /**< Master state machine. */
        )
{
    ec_master_t *master = fsm->master;
#ifdef EC_EOE
    ec_slave_t *slave = fsm->slave;
#endif

    if (ec_fsm_slave_scan_exec(&fsm->fsm_slave_scan)) {
        return;
    }

    master->scan_progress++;
    if (!ec_fsm_slave_scan_success(&fsm->fsm_slave_scan)) {
        size_t used = strlen(fsm->scan_failed);
        fsm->scan_failures++;
        if (used + 8 < sizeof(fsm->scan_failed)) {
            snprintf(fsm->scan_failed + used, sizeof(fsm->scan_failed) - used,
                    "%s%u", used ? ", " : "", fsm->slave->ring_position);
        }
    }

#ifdef EC_EOE
    if (slave->sii.mailbox_protocols & EC_MBOX_EOE) {
        // create EoE handler for this slave
        ec_eoe_t *eoe;
        if (!(eoe = ec_alloc(sizeof(ec_eoe_t)))) {
            EC_SLAVE_ERR(slave, "Failed to allocate EoE handler memory!\n");
        } else if (ec_eoe_init(eoe, slave)) {
            EC_SLAVE_ERR(slave, "Failed to init EoE handler!\n");
            ec_free(eoe);
        } else {
            list_add_tail(&eoe->list, &master->eoe_handlers);
        }
    }
#endif

    // another slave to fetch?
    fsm->slave++;
    master->scan_index++;
    if (fsm->slave < master->slaves + master->slave_count) {
        EC_MASTER_DBG(master, 1, "Scanning slave %u on %s link.\n",
                fsm->slave->ring_position,
                ec_device_names[fsm->slave->device_index != 0]);
        ec_fsm_slave_scan_start(&fsm->fsm_slave_scan, fsm->slave);
        ec_fsm_slave_scan_exec(&fsm->fsm_slave_scan); // execute immediately
        fsm->datagram->device_index = fsm->slave->device_index;
        return;
    }

    EC_MASTER_INFO(master, "Bus scanning completed in %llu ms.\n",
            (unsigned long long) ec_time_to_ms(
                ec_current_time() - fsm->scan_time));

    // The startup scan left slaves without their SII: the application could
    // not configure them.  Scan again, a bounded number of times, before
    // reporting the initial scan as done.
    if (fsm->scan_failures && !master->initial_scan_done) {
        if (master->initial_scan_attempts < EC_INITIAL_SCAN_ATTEMPTS) {
            EC_MASTER_WARN(master, "%u slave(s) not scanned (position %s),"
                    " scanning the bus again (attempt %u of %u).\n",
                    fsm->scan_failures, fsm->scan_failed,
                    master->initial_scan_attempts + 1,
                    EC_INITIAL_SCAN_ATTEMPTS);
            master->scan_busy = 0;
            fsm->rescan_required = 1;
            ec_fsm_master_restart(fsm);
            return;
        }
        EC_MASTER_ERR(master, "Bus scan incomplete after %u attempts:"
                " %u slave(s) not scanned (position %s).\n",
                master->initial_scan_attempts, fsm->scan_failures,
                fsm->scan_failed);
    }

    master->scan_busy = 0;
    master->scan_index = master->slave_count;
    master->initial_scan_done = 1;
    ec_wq_wake_interruptible(&master->scan_queue);

    ec_master_calc_dc(master);

    // Attach slave configurations
    ec_master_attach_slave_configs(master);

#ifdef EC_EOE
    // check if EoE processing has to be started
    ec_master_eoe_start(master);
#endif

    if (master->slave_count) {
        master->config_changed = 0;

        fsm->slave = master->slaves; // begin with first slave
        ec_fsm_master_enter_write_system_times(fsm);
    } else {
        ec_fsm_master_restart(fsm);
    }
}

/****************************************************************************/

/** Enter the configuration phase after all slave states have been read.
 *
 * Scans all slaves for configuration needs and starts the first one.
 * If no slave needs configuration, goes to idle.
 */
void ec_fsm_master_enter_configure_slaves(
        ec_fsm_master_t *fsm /**< Master state machine. */
        )
{
    ec_master_t *master = fsm->master;
    ec_fsm_slave_config_slot_t *slot;
    unsigned int i;
    int started = 0;

    /* Withdraw the background dictionary permission for this cycle.  It is
     * granted again by ec_fsm_master_action_idle(), which is only reached
     * when no slave needs configuration -- so a dictionary fetch can never
     * be started while configuration is pending. */
    master->allow_sdo_dict = 0;

    // Handle config_changed: restart from write_system_times
    if (master->config_changed) {
        master->config_changed = 0;
        EC_MASTER_DBG(master, 1, "Configuration changed.\n");
        fsm->slave = master->slaves;
        ec_fsm_master_enter_write_system_times(fsm);
        return;
    }

    // Fill as many slots as possible with slaves needing configuration.
    // Start at slot 1: slot 0 is reserved for slave scan.
    for (i = 1; i < EC_FSM_SLAVE_CONFIG_POOL_SIZE; i++) {
        slot = &fsm->config_slots[i];
        if (slot->in_use) {
            continue;
        }
        if (!ec_fsm_master_fill_config_slot(fsm, slot)) {
            break; // no more slaves need config
        }

        if (!started) {
            ec_sem_down(&master->config_sem);
            master->config_busy = 1;
            ec_sem_up(&master->config_sem);
            fsm->idle = 0;
            fsm->state = ec_fsm_master_state_configure_slaves;
        }
        started++;
    }

    if (!started) {
        // No slave needs configuration.
        // Note: config_busy was never set on this path (it's only set
        // inside the loop above when started transitions from 0 to 1),
        // so no cleanup or wakeup is needed here.
        ec_fsm_master_action_idle(fsm);
        return;
    }

    // Intentionally not calling the state function here: the datagrams
    // prepared by fill_config_slot() need to be queued and sent first.
    // The state function will execute on the next cycle after receive.
}

/****************************************************************************/

/** Master state: CONFIGURE SLAVES.
 *
 * Executes all active slave configuration slots in parallel.
 * Each cycle, executes every in_use slot whose datagram has been received.
 * When all slots finish, goes to idle.
 */
void ec_fsm_master_state_configure_slaves(
        ec_fsm_master_t *fsm /**< Master state machine. */
        )
{
    ec_master_t *master = fsm->master;
    ec_fsm_slave_config_slot_t *slot;
    unsigned int i;
    int any_active = 0;

    // Start at slot 1: slot 0 is reserved for slave scan.
    for (i = 1; i < EC_FSM_SLAVE_CONFIG_POOL_SIZE; i++) {
        slot = &fsm->config_slots[i];
        if (!slot->in_use) {
            continue;
        }

        // Skip if datagram not yet processed (not sent, or not queued due
        // to frame size limits — will be queued on a subsequent cycle)
        if (slot->datagram.state == EC_DATAGRAM_INIT
                || slot->datagram.state == EC_DATAGRAM_SENT
                || slot->datagram.state == EC_DATAGRAM_QUEUED) {
            any_active = 1;
            continue;
        }

        if (ec_fsm_slave_config_exec(&slot->fsm, &slot->datagram)) {
            // still running
            any_active = 1;
            continue;
        }

        slot->fsm.slave->force_config = 0;

        // Stop refilling if configuration changed; let active slots drain,
        // then the FSM will restart and re-evaluate from write_system_times.
        slot->in_use = 0;
        if (!master->config_changed
                && ec_fsm_master_fill_config_slot(fsm, slot)) {
            any_active = 1;
        }
    }

    if (any_active) {
        return;
    }

    // All configurations done
    ec_sem_down(&master->config_sem);
    master->config_busy = 0;
    ec_sem_up(&master->config_sem);
    ec_wq_wake_interruptible(&master->config_queue);

    fsm->idle = 1;
    ec_fsm_master_action_idle(fsm);
}

/****************************************************************************/

/** Start writing DC system times.
 */
void ec_fsm_master_enter_write_system_times(
        ec_fsm_master_t *fsm /**< Master state machine. */
        )
{
    ec_master_t *master = fsm->master;

    if (master->dc_ref_time) {

        while (fsm->slave < master->slaves + master->slave_count) {
            if (!fsm->slave->base_dc_supported
                    || !fsm->slave->has_dc_system_time) {
                fsm->slave++;
                continue;
            }

            EC_SLAVE_DBG(fsm->slave, 1, "Checking system time offset.\n");

            // read DC system time (0x0910, 64 bit)
            //                         gap (64 bit)
            //     and time offset (0x0920, 64 bit)
            ec_datagram_fprd(fsm->datagram, fsm->slave->station_address,
                    0x0910, 24);
            fsm->datagram->device_index = fsm->slave->device_index;
            fsm->retries = EC_FSM_RETRIES;
            fsm->state = ec_fsm_master_state_dc_read_offset;
            return;
        }

    } else {
        if (master->active) {
            EC_MASTER_WARN(master, "No application time received up to now,"
                    " but master already active.\n");
        } else {
            EC_MASTER_DBG(master, 1, "No app_time received up to now.\n");
        }
    }

    // scanning and setting system times complete
    ec_master_request_op(master);
    EC_MASTER_DBG(master, 1, "After requesting OP, rescan_required is %u.\n",
            fsm->rescan_required);
    ec_fsm_master_restart(fsm);
}

/****************************************************************************/

/** Configure 32 bit time offset.
 *
 * \return New offset.
 */
uint64_t ec_fsm_master_dc_offset32(
        ec_fsm_master_t *fsm, /**< Master state machine. */
        uint64_t system_time, /**< System time register. */
        uint64_t old_offset, /**< Time offset register. */
        ec_time_t time_since_read /**< Time for correction. */
        )
{
    ec_slave_t *slave = fsm->slave;
    uint32_t correction, system_time32, old_offset32, new_offset;
    int32_t time_diff;

    system_time32 = (uint32_t) system_time;
    old_offset32 = (uint32_t) old_offset;

    // correct read system time by elapsed time since read operation
    correction = ec_time_to_ns(time_since_read);
    system_time32 += correction;
    time_diff = (uint32_t) slave->master->app_time - system_time32;

    EC_SLAVE_DBG(slave, 1, "DC 32 bit system time offset calculation:"
            " system_time=%u (corrected with %u),"
            " app_time=%llu, diff=%i\n",
            system_time32, correction,
            (unsigned long long) slave->master->app_time, time_diff);

    if (EC_ABS(time_diff) > EC_SYSTEM_TIME_TOLERANCE_NS) {
        new_offset = time_diff + old_offset32;
        EC_SLAVE_DBG(slave, 1, "Setting time offset to %u (was %u)\n",
                new_offset, old_offset32);
        return (uint64_t) new_offset;
    } else {
        EC_SLAVE_DBG(slave, 1, "Not touching time offset.\n");
        return old_offset;
    }
}

/****************************************************************************/

/** Configure 64 bit time offset.
 *
 * \return New offset.
 */
uint64_t ec_fsm_master_dc_offset64(
        ec_fsm_master_t *fsm, /**< Master state machine. */
        uint64_t system_time, /**< System time register. */
        uint64_t old_offset, /**< Time offset register. */
        ec_time_t time_since_read /**< Time for correction. */
        )
{
    ec_slave_t *slave = fsm->slave;
    uint64_t new_offset, correction;
    int64_t time_diff;

    // correct read system time by elapsed time since read operation
    correction = (uint64_t) ec_time_to_ns(time_since_read);
    system_time += correction;
    time_diff = fsm->slave->master->app_time - system_time;

    EC_SLAVE_DBG(slave, 1, "DC 64 bit system time offset calculation:"
            " system_time=%llu (corrected with %llu),"
            " app_time=%llu, diff=%lli\n",
            (unsigned long long) system_time,
            (unsigned long long) correction,
            (unsigned long long) slave->master->app_time,
            (long long) time_diff);

    if (EC_ABS(time_diff) > EC_SYSTEM_TIME_TOLERANCE_NS) {
        new_offset = time_diff + old_offset;
        EC_SLAVE_DBG(slave, 1, "Setting time offset to %llu (was %llu)\n",
                (unsigned long long) new_offset,
                (unsigned long long) old_offset);
    } else {
        new_offset = old_offset;
        EC_SLAVE_DBG(slave, 1, "Not touching time offset.\n");
    }

    return new_offset;
}

/****************************************************************************/

/** Master state: DC READ OFFSET.
 */
void ec_fsm_master_state_dc_read_offset(
        ec_fsm_master_t *fsm /**< Master state machine. */
        )
{
    ec_datagram_t *datagram = fsm->datagram;
    ec_slave_t *slave = fsm->slave;
    uint64_t system_time, old_offset, new_offset;
    ec_time_t time_since_read;

    if (datagram->state == EC_DATAGRAM_TIMED_OUT && fsm->retries--)
        return;

    if (datagram->state != EC_DATAGRAM_RECEIVED) {
        EC_SLAVE_ERR(slave, "Failed to receive DC times datagram: Datagram %s.\n", ec_datagram_state_str(datagram));
        fsm->slave++;
        ec_fsm_master_enter_write_system_times(fsm);
        return;
    }

    if (datagram->working_counter != 1) {
        char _wc[32];
        ec_datagram_wc_error_str(datagram, _wc, sizeof(_wc));
        EC_SLAVE_WARN(slave, "Failed to get DC times: %s\n", _wc);
        fsm->slave++;
        ec_fsm_master_enter_write_system_times(fsm);
        return;
    }

    system_time = EC_READ_U64(datagram->data);     // 0x0910
    old_offset = EC_READ_U64(datagram->data + 16); // 0x0920
    time_since_read = ec_current_time() - datagram->time_sent;

    if (slave->base_dc_range == EC_DC_32) {
        new_offset = ec_fsm_master_dc_offset32(fsm,
                system_time, old_offset, time_since_read);
    } else {
        new_offset = ec_fsm_master_dc_offset64(fsm,
                system_time, old_offset, time_since_read);
    }

    // set DC system time offset and transmission delay
    ec_datagram_fpwr(datagram, slave->station_address, 0x0920, 12);
    EC_WRITE_U64(datagram->data, new_offset);
    EC_WRITE_U32(datagram->data + 8, slave->transmission_delay);
    fsm->datagram->device_index = slave->device_index;
    fsm->retries = EC_FSM_RETRIES;
    fsm->state = ec_fsm_master_state_dc_write_offset;
}

/****************************************************************************/

/** Master state: DC WRITE OFFSET.
 */
void ec_fsm_master_state_dc_write_offset(
        ec_fsm_master_t *fsm /**< Master state machine. */
        )
{
    ec_datagram_t *datagram = fsm->datagram;
    ec_slave_t *slave = fsm->slave;

    if (datagram->state == EC_DATAGRAM_TIMED_OUT && fsm->retries--)
        return;

    if (datagram->state != EC_DATAGRAM_RECEIVED) {
        EC_SLAVE_ERR(slave,
                "Failed to receive DC system time offset datagram: Datagram %s.\n", ec_datagram_state_str(datagram));
        fsm->slave++;
        ec_fsm_master_enter_write_system_times(fsm);
        return;
    }

    if (datagram->working_counter != 1) {
        char _wc[32];
        ec_datagram_wc_error_str(datagram, _wc, sizeof(_wc));
        EC_SLAVE_ERR(slave, "Failed to set DC system time offset: %s\n", _wc);
        fsm->slave++;
        ec_fsm_master_enter_write_system_times(fsm);
        return;
    }

    fsm->slave++;
    ec_fsm_master_enter_write_system_times(fsm);
}

/****************************************************************************/

/** Master state: ASSIGN SII.
 */
void ec_fsm_master_state_assign_sii(
        ec_fsm_master_t *fsm /**< Master state machine. */
        )
{
    ec_datagram_t *datagram = fsm->datagram;
    ec_sii_write_request_t *request = fsm->sii_request;
    ec_slave_t *slave = request->slave;

    if (datagram->state == EC_DATAGRAM_TIMED_OUT && fsm->retries--)
        return;

    if (datagram->state != EC_DATAGRAM_RECEIVED) {
        EC_SLAVE_ERR(slave, "Failed to receive SII assignment datagram: Datagram %s.\n", ec_datagram_state_str(datagram));
        goto cont;
    }

    if (datagram->working_counter != 1) {
        char _wc[32];
        ec_datagram_wc_error_str(datagram, _wc, sizeof(_wc));
        EC_SLAVE_ERR(slave, "Failed to assign SII back to EtherCAT: %s\n", _wc);
        goto cont;
    }

cont:
    // found pending SII write operation. execute it!
    EC_SLAVE_DBG(slave, 1, "Writing SII data (after assignment)...\n");
    fsm->sii_index = 0;
    ec_fsm_sii_write(&fsm->fsm_sii, slave, request->offset,
            request->words, EC_FSM_SII_USE_CONFIGURED_ADDRESS);
    fsm->state = ec_fsm_master_state_write_sii;
    fsm->state(fsm); // execute immediately
}

/****************************************************************************/

/** Master state: WRITE SII.
 */
void ec_fsm_master_state_write_sii(
        ec_fsm_master_t *fsm /**< Master state machine. */
        )
{
    ec_master_t *master = fsm->master;
    ec_sii_write_request_t *request = fsm->sii_request;
    ec_slave_t *slave = request->slave;

    if (ec_fsm_sii_exec(&fsm->fsm_sii)) return;

    if (!ec_fsm_sii_success(&fsm->fsm_sii)) {
        EC_SLAVE_ERR(slave, "Failed to write SII data.\n");
        request->state = EC_INT_REQUEST_FAILURE;
        ec_wq_wake_all(&master->request_queue);
        ec_fsm_master_restart(fsm);
        return;
    }

    fsm->sii_index++;
    if ((size_t) fsm->sii_index < request->nwords) {
        ec_fsm_sii_write(&fsm->fsm_sii, slave,
                request->offset + fsm->sii_index,
                request->words + fsm->sii_index,
                EC_FSM_SII_USE_CONFIGURED_ADDRESS);
        ec_fsm_sii_exec(&fsm->fsm_sii); // execute immediately
        return;
    }

    // finished writing SII
    EC_SLAVE_DBG(slave, 1, "Finished writing %zu words of SII data.\n",
            request->nwords);

    if (request->offset <= 4 && request->offset + request->nwords > 4) {
        // alias was written
        slave->sii.alias = EC_READ_U16(request->words + 4);
        // TODO: read alias from register 0x0012
        slave->effective_alias = slave->sii.alias;
    }
    // TODO: Evaluate other SII contents!

    request->state = EC_INT_REQUEST_SUCCESS;
    ec_wq_wake_all(&master->request_queue);

    // check for another SII write request
    if (ec_fsm_master_action_process_sii(fsm))
        return; // processing another request

    ec_fsm_master_restart(fsm);
}

/****************************************************************************/

/** Master state: SDO REQUEST.
 */
void ec_fsm_master_state_sdo_request(
        ec_fsm_master_t *fsm /**< Master state machine. */
        )
{
    ec_sdo_request_t *request = fsm->sdo_request;

    if (!request) {
        // configuration was cleared in the meantime
        ec_fsm_master_restart(fsm);
        return;
    }

    if (ec_fsm_coe_exec(&fsm->fsm_coe, fsm->datagram)) {
        return;
    }

    if (!ec_fsm_coe_success(&fsm->fsm_coe)) {
        EC_SLAVE_DBG(fsm->slave, 1,
                "Failed to process internal SDO request.\n");
        request->state = EC_INT_REQUEST_FAILURE;
        ec_wq_wake_all(&fsm->master->request_queue);
        ec_fsm_master_restart(fsm);
        return;
    }

    // SDO request finished
    request->state = EC_INT_REQUEST_SUCCESS;
    ec_wq_wake_all(&fsm->master->request_queue);

    EC_SLAVE_DBG(fsm->slave, 1, "Finished internal SDO request.\n");

    // check for another SDO/SoE request
    if (ec_fsm_master_action_process_int_request(fsm)) {
        return; // processing another request
    }

    ec_fsm_master_restart(fsm);
}

/****************************************************************************/

/** Master state: SoE REQUEST.
 */
void ec_fsm_master_state_soe_request(
        ec_fsm_master_t *fsm /**< Master state machine. */
        )
{
    ec_soe_request_t *request = fsm->soe_request;

    if (!request) {
        // configuration was cleared in the meantime
        ec_fsm_master_restart(fsm);
        return;
    }

    if (ec_fsm_soe_exec(&fsm->fsm_soe, fsm->datagram)) {
        return;
    }

    if (!ec_fsm_soe_success(&fsm->fsm_soe)) {
        EC_SLAVE_DBG(fsm->slave, 1,
                "Failed to process internal SoE request.\n");
        request->state = EC_INT_REQUEST_FAILURE;
        ec_wq_wake_all(&fsm->master->request_queue);
        ec_fsm_master_restart(fsm);
        return;
    }

    // SoE request finished
    request->state = EC_INT_REQUEST_SUCCESS;
    ec_wq_wake_all(&fsm->master->request_queue);

    EC_SLAVE_DBG(fsm->slave, 1, "Finished internal SoE request.\n");

    // check for another CoE/SoE request
    if (ec_fsm_master_action_process_int_request(fsm)) {
        return; // processing another request
    }

    ec_fsm_master_restart(fsm);
}

/****************************************************************************/
