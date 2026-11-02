/*****************************************************************************
 *
 *  transport_sim — in-process simulated EtherCAT bus for unit tests.
 *
 *  Implements the public ec_transport_ops_t interface with a virtual bus
 *  that emulates slaves at datagram level: position/configured/broadcast
 *  addressing, working counters, a register space per slave, the AL state
 *  machine and the SII EEPROM interface.  This makes the master's scan,
 *  configuration and cyclic logic testable without hardware; the core is
 *  shared with kernel mode, so these tests cover kernel-mode logic too.
 *
 *  This file is part of the IgH EtherCAT Master.
 *
 *  The IgH EtherCAT Master is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License version 2, as
 *  published by the Free Software Foundation.
 *
 ****************************************************************************/

#ifndef __EC_TRANSPORT_SIM_H__
#define __EC_TRANSPORT_SIM_H__

#include "ecrt.h"
#include "ectp.h"

/** Maximum number of simulated slaves per bus. */
#define SIM_MAX_SLAVES 8

/** Per-slave register space (standard register area + process data RAM). */
#define SIM_REG_SIZE 0x3000

/** SII EEPROM size in words. */
#define SIM_EEPROM_WORDS 0x100

typedef struct sim_bus sim_bus_t;

/** Identity used to build a slave's SII EEPROM image.
 *
 * If \a sm2_len or \a sm3_len is nonzero, the EEPROM contains a sync
 * manager category describing SM0/SM1 (disabled, no mailbox) and SM2
 * (process data output) / SM3 (process data input) at the given
 * physical addresses, so the slave can carry PDOs configured via
 * ecrt_slave_config_pdos(). */
typedef struct {
    uint32_t vendor_id;
    uint32_t product_code;
    uint32_t revision_number;
    uint32_t serial_number;
    uint16_t alias;
    uint16_t sm2_phys; /**< Physical start of the output SM (e.g. 0x1100). */
    uint16_t sm2_len;  /**< Default length of the output SM (0: no SM2/3). */
    uint16_t sm3_phys; /**< Physical start of the input SM (e.g. 0x1400). */
    uint16_t sm3_len;  /**< Default length of the input SM. */
    uint16_t mbox_out_phys; /**< Physical start of the receive (master ->
                              slave) mailbox / SM0 (e.g. 0x1000). */
    uint16_t mbox_out_len;  /**< Receive mailbox size (0: no mailbox). */
    uint16_t mbox_in_phys;  /**< Physical start of the send (slave ->
                              master) mailbox / SM1 (e.g. 0x1080). */
    uint16_t mbox_in_len;   /**< Send mailbox size. */
    uint8_t dc_supported;   /**< Advertise distributed clocks (32 bit). */
    uint8_t eoe;            /**< Advertise EoE (requires mailbox);
                              received frames are echoed back. */
    uint8_t loopback;       /**< Echo output process data (SM2) into input
                              process data (SM3) after each frame, so a HAL
                              output pin round-trips to its input pin. Needs
                              both sm2_len and sm3_len nonzero. */
} sim_slave_identity_t;

/** Create a bus with \a nslaves slaves, each initialized from
 * \a identities (must have \a nslaves entries). Returns NULL on error. */
sim_bus_t *sim_bus_create(unsigned int nslaves,
        const sim_slave_identity_t *identities);

void sim_bus_destroy(sim_bus_t *bus);

/** The transport instance to pass to ecrt_startup_master(). Owned by the
 * bus; do not call ec_transport_destroy() on it. */
ec_transport_t *sim_bus_transport(sim_bus_t *bus);

/** Set the simulated link state (1 = up, 0 = down; initial: up).
 * With the link down, frames are dropped. */
void sim_bus_set_link(sim_bus_t *bus, int up);

/** Current AL state of slave \a pos (low nibble of the AL status
 * register, e.g. 1 = INIT, 2 = PREOP, 4 = SAFEOP, 8 = OP). */
unsigned int sim_bus_slave_al_state(sim_bus_t *bus, unsigned int pos);

/** Direct access to slave register memory (for test assertions and for
 * feeding simulated input process data). */
uint8_t *sim_bus_slave_regs(sim_bus_t *bus, unsigned int pos);

/** Number of frames processed by the bus so far. */
unsigned long sim_bus_frame_count(sim_bus_t *bus);

/** Lose every frame that accesses slave \a pos's SII (registers 0x0500 to
 * 0x050F) during the first \a scans bus scans -- a scan begins with the
 * broadcast write of the station addresses.  The slave still sees the
 * access; only the reply never comes back, which is what a reply delivered
 * after the datagram timeout looks like to the master.  Set it before the
 * master starts. */
void sim_bus_lose_sii(sim_bus_t *bus, unsigned int pos, unsigned int scans);

/** Deliver every reply \a us microseconds after its frame was sent (0: at
 * once), as a NIC whose interrupts are handled on a busy CPU does. */
void sim_bus_set_reply_delay(sim_bus_t *bus, unsigned int us);

/** Bus scans the master has begun so far (see sim_bus_lose_sii). */
unsigned int sim_bus_scan_count(sim_bus_t *bus);

/** CPU the master last asked the transport to pin its IRQs to, -1 if it
 * never did. */
int sim_bus_irq_cpu(sim_bus_t *bus);

/** Frames the bus had processed when that request came, -1 if it never
 * came: 0 means the IRQs were pinned before the first frame of the scan. */
long sim_bus_irq_pin_frames(sim_bus_t *bus);

/** Number of broadcast reads of the AL status register (0x0130) the
 * master has issued so far — the master FSM's bus-state poll.  Lets
 * tests assert that bus-state monitoring keeps running while a long
 * mailbox transfer (e.g. an SDO dictionary fetch) is in progress. */
unsigned long sim_bus_al_status_brd_count(sim_bus_t *bus);

/** Number of link-state polls (transport get_link calls) so far —
 * lets tests synchronize on "the master is running and watching the
 * link" instead of wall-clock sleeps. */
unsigned long sim_bus_link_polls(sim_bus_t *bus);

/** Place a response in slave \a pos's send mailbox and raise the
 * "mailbox full" flag, as a slave would after answering a request.
 * Lets a test simulate what a master that exited mid-transfer leaves
 * behind: a queued response nobody read.  \a type is the mailbox
 * protocol (3 = CoE).  Returns 0 on success. */
int sim_bus_mbox_preload(sim_bus_t *bus, unsigned int pos, uint8_t type,
        const void *payload, uint16_t len);

/** Whether slave \a pos's send mailbox still holds an unread response
 * (SM1 "mailbox full" status), or -1 for an invalid position. */
int sim_bus_mbox_full(sim_bus_t *bus, unsigned int pos);

/** Create or update an object-dictionary entry of slave \a pos (CoE
 * slaves only; entries serve SDO uploads and accept downloads).
 * Returns 0 on success. */
int sim_bus_od_set(sim_bus_t *bus, unsigned int pos, uint16_t index,
        uint8_t subindex, const void *data, size_t size);

/** Look up an object-dictionary entry; returns the data pointer and
 * stores the current size in \a size, or NULL if not present. */
const uint8_t *sim_bus_od_data(sim_bus_t *bus, unsigned int pos,
        uint16_t index, uint8_t subindex, size_t *size);

/** Number of SDO download requests slave \a pos has received. */
unsigned long sim_bus_od_download_count(sim_bus_t *bus, unsigned int pos);

#endif
