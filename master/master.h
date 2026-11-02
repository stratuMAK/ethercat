/*****************************************************************************
 *
 *  Copyright (C) 2006-2024  Florian Pose, Ingenieurgemeinschaft IgH
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
   \file
   EtherCAT master structure.
*/

/****************************************************************************/

#ifndef __EC_MASTER_H__
#define __EC_MASTER_H__


#include "pal.h"

#include "device.h"
#include "domain.h"
#include "ethernet.h"
#include "fsm_master.h"

/****************************************************************************/

/** Convenience macro for printing master-specific information to syslog.
 *
 * This will print the message in \a fmt with a prefixed "EtherCAT <INDEX>: ",
 * where INDEX is the master index.
 *
 * \param master EtherCAT master
 * \param fmt format string (like in printf())
 * \param args arguments (optional)
 */
#define EC_MASTER_INFO(master, fmt, args...) \
    ec_log(EC_LOG_INFO, "EtherCAT %u: " fmt, master->index, ##args)

/** Convenience macro for printing master-specific errors to syslog.
 *
 * This will print the message in \a fmt with a prefixed "EtherCAT <INDEX>: ",
 * where INDEX is the master index.
 *
 * \param master EtherCAT master
 * \param fmt format string (like in printf())
 * \param args arguments (optional)
 */
#define EC_MASTER_ERR(master, fmt, args...) \
    ec_log(EC_LOG_ERR, "EtherCAT ERROR %u: " fmt, master->index, ##args)

/** Convenience macro for printing master-specific warnings to syslog.
 *
 * This will print the message in \a fmt with a prefixed "EtherCAT <INDEX>: ",
 * where INDEX is the master index.
 *
 * \param master EtherCAT master
 * \param fmt format string (like in printf())
 * \param args arguments (optional)
 */
#define EC_MASTER_WARN(master, fmt, args...) \
    ec_log(EC_LOG_WARNING, "EtherCAT WARNING %u: " fmt, master->index, ##args)

/** Convenience macro for printing master-specific debug messages to syslog.
 *
 * This will print the message in \a fmt with a prefixed "EtherCAT <INDEX>: ",
 * where INDEX is the master index.
 *
 * \param master EtherCAT master
 * \param level Debug level. Master's debug level must be >= \a level for
 * output.
 * \param fmt format string (like in printf())
 * \param args arguments (optional)
 */
#define EC_MASTER_DBG(master, level, fmt, args...) \
    do { \
        if (master->debug_level >= level) { \
            ec_log(EC_LOG_DEBUG, "EtherCAT DEBUG %u: " fmt, \
                    master->index, ##args); \
        } \
    } while (0)


/** Size of the external datagram ring.
 *
 * The external datagram ring is used for slave FSMs.
 */
#define EC_EXT_RING_SIZE 32

/** Maximum number of masters.
 */
#define EC_MAX_MASTERS 32

/****************************************************************************/

/** EtherCAT master phase.
 */
typedef enum {
    EC_ORPHANED, /**< Orphaned phase. The master has no Ethernet device
                   attached. */
    EC_IDLE, /**< Idle phase. An Ethernet device is attached, but the master
               is not in use, yet. */
    EC_OPERATION /**< Operation phase. The master was requested by a realtime
                   application. */
} ec_master_phase_t;

/****************************************************************************/

/** Cyclic statistics.
 */
typedef struct {
    unsigned int timeouts; /**< datagram timeouts */
    unsigned int corrupted; /**< corrupted frames */
    unsigned int unmatched; /**< unmatched datagrams (received, but not
                               queued any longer) */
    ec_time_t output_time; /**< time of last output */
} ec_stats_t;

/****************************************************************************/

/** Device statistics.
 */
typedef struct {
    EC_PAL_SHARED uint64_t tx_count; /**< Number of frames sent. */
    uint64_t last_tx_count; /**< Number of frames sent of last statistics cycle. */
    EC_PAL_SHARED uint64_t rx_count; /**< Number of frames received. */
    uint64_t last_rx_count; /**< Number of frames received of last statistics
                         cycle. */
    EC_PAL_SHARED uint64_t tx_bytes; /**< Number of bytes sent. */
    uint64_t last_tx_bytes; /**< Number of bytes sent of last statistics cycle. */
    EC_PAL_SHARED uint64_t rx_bytes; /**< Number of bytes received. */
    uint64_t last_rx_bytes; /**< Number of bytes received of last statistics cycle.
                        */
    uint64_t last_loss; /**< Tx/Rx difference of last statistics cycle. */
    EC_PAL_SHARED int32_t tx_frame_rates[EC_RATE_COUNT]; /**< Transmit rates in frames/s for
                                         different statistics cycle periods.
                                        */
    EC_PAL_SHARED int32_t rx_frame_rates[EC_RATE_COUNT]; /**< Receive rates in frames/s for
                                         different statistics cycle periods.
                                        */
    EC_PAL_SHARED int32_t tx_byte_rates[EC_RATE_COUNT]; /**< Transmit rates in byte/s for
                                        different statistics cycle periods. */
    EC_PAL_SHARED int32_t rx_byte_rates[EC_RATE_COUNT]; /**< Receive rates in byte/s for
                                        different statistics cycle periods. */
    EC_PAL_SHARED int32_t loss_rates[EC_RATE_COUNT]; /**< Frame loss rates for different
                                     statistics cycle periods. */
    ec_time_t last_cycle; /**< Time of last statistic cycle. */
} ec_device_stats_t;

/****************************************************************************/

#if EC_MAX_NUM_DEVICES < 1
#error Invalid number of devices
#endif

/****************************************************************************/

/** EtherCAT master.
 *
 * Manages slaves, domains and IO.
 */
struct ec_master {
    unsigned int index; /**< Index. */
    unsigned int reserved; /**< \a True, if the master is in use. */

    ec_master_pal_t pal; /**< Platform specific fields */

    ec_semaphore_t master_sem; /**< Master semaphore. */

    ec_device_t devices[EC_MAX_NUM_DEVICES]; /**< EtherCAT devices. */
    const uint8_t *macs[EC_MAX_NUM_DEVICES]; /**< Device MAC addresses. */
#if EC_MAX_NUM_DEVICES > 1
    unsigned int num_devices; /**< Number of devices. Access this always via
                                ec_master_num_devices(), because it may be
                                optimized! */
#endif
    ec_semaphore_t device_sem; /**< Device semaphore. */
    ec_device_stats_t device_stats; /**< Device statistics. */

    ec_fsm_master_t fsm; /**< Master state machine. */
    ec_datagram_t fsm_datagram; /**< Datagram used for state machines. */
    EC_PAL_SHARED ec_master_phase_t phase; /**< Master phase. */
    EC_PAL_SHARED unsigned int active; /**< Master has been activated. */
    EC_PAL_SHARED unsigned int config_changed; /**< The configuration changed. */
    EC_PAL_SHARED unsigned int injection_seq_fsm; /**< Datagram injection
                                      sequence number for the FSM side. */
    EC_PAL_SHARED unsigned int injection_seq_rt; /**< Datagram injection
                                     sequence number for the realtime side. */

    ec_slave_t *slaves; /**< Array of slaves on the bus. */
    EC_PAL_SHARED unsigned int slave_count; /**< Number of slaves on the
                                              bus. Written during scan,
                                              read by the app/tool. */

    /* Configuration applied by the application. */
    struct list_head configs; /**< List of slave configurations. */
    struct list_head domains; /**< List of domains. */

    EC_PAL_SHARED uint64_t app_time; /**< Time of the last ecrt_master_sync() call. */
    EC_PAL_SHARED uint64_t dc_ref_time; /**< Common reference timestamp for DC start times. */
    ec_datagram_t ref_sync_datagram; /**< Datagram used for synchronizing the
                                       reference clock to the master clock. */
    ec_datagram_t sync_datagram; /**< Datagram used for DC drift
                                   compensation. */
    ec_datagram_t sync_mon_datagram; /**< Datagram used for DC synchronisation
                                       monitoring. */
    ec_slave_config_t *dc_ref_config; /**< Application-selected DC reference
                                        clock slave config. */
    ec_slave_t *dc_ref_clock; /**< DC reference clock slave. */

    EC_PAL_SHARED unsigned int scan_busy; /**< Current scan state. */
    EC_PAL_SHARED unsigned int scan_index; /**< Index of slave currently scanned. */
    EC_PAL_SHARED unsigned int allow_scan; /**< \a True, if slave scanning is allowed. */
    ec_semaphore_t scan_sem; /**< Semaphore protecting the \a scan_busy
                                 variable and the \a allow_scan flag. */
    ec_wait_queue_t scan_queue; /**< Queue for processes that wait for
                                    slave scanning. */
    EC_PAL_SHARED unsigned int initial_scan_done; /**< Nonzero after the first bus scan
                                         attempt has completed (either slaves
                                         found and scanned, or no slaves). */
    EC_PAL_SHARED unsigned int scan_progress; /**< Counts every scan begun
                                        and every slave scanned, across
                                        rescans: what the startup wait
                                        watches for progress. */
    unsigned int initial_scan_attempts; /**< Startup scans begun (see
                                          EC_INITIAL_SCAN_ATTEMPTS). */

    EC_PAL_SHARED unsigned int config_busy; /**< State of slave configuration. */
    ec_semaphore_t config_sem; /**< Semaphore protecting the \a config_busy
                                   variable and the allow_config flag. */
    ec_wait_queue_t config_queue; /**< Queue for processes that wait for
                                      slave configuration. */

    unsigned int allow_sdo_dict; /**< \a True, if the slave FSMs may fetch SDO
                                   dictionaries in the background.  Granted by
                                   the master FSM once no slave needs
                                   configuration any more and withdrawn as soon
                                   as configuration starts again, so
                                   dictionaries never compete with slave
                                   configuration.  Only touched by the master
                                   FSM and the slave FSMs, both of which run
                                   under \a master_sem. */

    struct list_head datagram_queue; /**< Datagram queue. */
    uint8_t datagram_index; /**< Current datagram index. */

    struct list_head ext_datagram_queue; /**< Queue for non-application
                                           datagrams. */
    ec_semaphore_t ext_queue_sem; /**< Semaphore protecting the \a
                                      ext_datagram_queue. */

    ec_datagram_t ext_datagram_ring[EC_EXT_RING_SIZE]; /**< External datagram
                                                         ring. */
    EC_PAL_SHARED unsigned int ext_ring_idx_rt; /**< Index in external
                                    datagram ring for RT side. */
    EC_PAL_SHARED unsigned int ext_ring_idx_fsm; /**< Index in external
                                     datagram ring for FSM side. */
    unsigned int send_interval; /**< Interval between two calls to
                                  ecrt_master_send(). */
    size_t max_queue_size; /**< Maximum size of datagram queue */

    ec_slave_t *fsm_slave; /**< Slave that is queried next for FSM exec. */
    struct list_head fsm_exec_list; /**< Slave FSM execution list. */
    unsigned int fsm_exec_count; /**< Number of entries in execution list. */

    unsigned int debug_level; /**< Master debug level. */
    unsigned int run_on_cpu;  /**< bind kernel threads to this cpu */
    ec_stats_t stats; /**< Cyclic statistics. */

    ec_thread_t *thread; /**< Master thread. */

#ifdef EC_EOE
    ec_thread_t *eoe_thread; /**< EoE thread. */
    struct list_head eoe_handlers; /**< Ethernet over EtherCAT handlers. */
#endif

    ec_rt_mutex_t io_mutex;  /**< Mutex used in \a IDLE and \a OP phase. */

    void (*send_cb)(void *); /**< Current send datagrams callback. */
    void (*receive_cb)(void *); /**< Current receive datagrams callback. */
    void *cb_data; /**< Current callback data. */
    void (*app_send_cb)(void *); /**< Application's send datagrams
                                          callback. */
    void (*app_receive_cb)(void *); /**< Application's receive datagrams
                                      callback. */
    void *app_cb_data; /**< Application callback data. */

    struct list_head sii_requests; /**< SII write requests. */
    struct list_head emerg_reg_requests; /**< Emergency register access
                                           requests. */

    ec_wait_queue_t request_queue; /**< Wait queue for external requests
                                       from user space. */
    ec_work_t sc_reset_work; /**< Task to reset slave configuration. */
    ec_irq_work_t sc_reset_work_kicker; /**< NMI-Safe kicker to trigger
                                            reset task above. */
};

/****************************************************************************/

// static funtions
void ec_master_init_static(void);

// master creation/deletion
int ec_master_init(ec_master_t *, unsigned int, const uint8_t *,
        const uint8_t *, unsigned int, unsigned int);
void ec_master_clear(ec_master_t *);

/** Number of Ethernet devices.
 */
#if EC_MAX_NUM_DEVICES > 1
#define ec_master_num_devices(MASTER) ((MASTER)->num_devices)
#else
#define ec_master_num_devices(MASTER) 1
#endif

// phase transitions
int ec_master_enter_idle_phase(ec_master_t *);
void ec_master_leave_idle_phase(ec_master_t *);
int ec_master_enter_operation_phase(ec_master_t *);
void ec_master_leave_operation_phase(ec_master_t *);

#ifdef EC_EOE
// EoE
void ec_master_eoe_start(ec_master_t *);
void ec_master_eoe_stop(ec_master_t *);
#endif

// datagram IO
void ec_master_receive_datagrams(ec_master_t *, ec_device_t *,
        const uint8_t *, size_t) EC_RT_ATTR;
void ec_master_queue_datagram(ec_master_t *, ec_datagram_t *) EC_RT_ATTR;
void ec_master_queue_datagram_ext(ec_master_t *, ec_datagram_t *);

// misc.
void ec_master_set_send_interval(ec_master_t *, unsigned int);
void ec_master_attach_slave_configs(ec_master_t *);
ec_slave_t *ec_master_find_slave(ec_master_t *, uint16_t, uint16_t);
const ec_slave_t *ec_master_find_slave_const(const ec_master_t *, uint16_t,
        uint16_t);
void ec_master_output_stats(ec_master_t *) EC_RT_ATTR;
#ifdef EC_EOE
void ec_master_clear_eoe_handlers(ec_master_t *);
#endif
void ec_master_clear_slaves(ec_master_t *);

unsigned int ec_master_config_count(const ec_master_t *);
ec_slave_config_t *ec_master_get_config(
        const ec_master_t *, unsigned int);
const ec_slave_config_t *ec_master_get_config_const(
        const ec_master_t *, unsigned int);
unsigned int ec_master_domain_count(const ec_master_t *);
ec_domain_t *ec_master_find_domain(ec_master_t *, unsigned int);
const ec_domain_t *ec_master_find_domain_const(const ec_master_t *,
        unsigned int);
#ifdef EC_EOE
uint16_t ec_master_eoe_handler_count(const ec_master_t *);
const ec_eoe_t *ec_master_get_eoe_handler_const(const ec_master_t *, uint16_t);
#endif

int ec_master_debug_level(ec_master_t *, unsigned int);

ec_domain_t *ecrt_master_create_domain_err(ec_master_t *);
ec_slave_config_t *ecrt_master_slave_config_err(ec_master_t *, uint16_t,
        uint16_t, uint32_t, uint32_t);

void ec_master_calc_dc(ec_master_t *);
void ec_master_request_op(ec_master_t *);

void ec_master_internal_send_cb(void *);
void ec_master_internal_receive_cb(void *);

void ec_master_idle_thread_schedule(ec_master_t *master, int sent_bytes);
void ec_master_operation_thread_schedule(ec_master_t *master);

ec_time_t ec_current_time(void) EC_RT_ATTR;

extern const unsigned int rate_intervals[EC_RATE_COUNT]; // see master.c

#include "pal_affinity.h"

/****************************************************************************/

#endif
