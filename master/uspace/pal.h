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
   Platform Abstraction Layer for userspace EtherCAT master.
*/

/****************************************************************************/

#ifndef __EC_USPACE_PAL_H__
#define __EC_USPACE_PAL_H__

#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdatomic.h>
#include <syslog.h>
#include <errno.h>
#include <stdarg.h>
#include <sys/syscall.h>
#include <sched.h>

#include <stdint.h>
#include <stdbool.h>

/* master_globals.h (via globals.h) provides the EC_RT_ATTR /
 * EC_PAL_SHARED macros used by the PAL headers below. */
#include "../master_globals.h"

#include "pal_misc.h"
#include "pal_sem.h"
#include "pal_mtx.h"
#include "pal_queue.h"
#include "pal_thread.h"
#include "pal_work.h"
#include "pal_irq_work.h"
#include "pal_eoe.h"

#define EC_REQUEST_CONST

//************************************************************************

typedef uint64_t ec_time_t;
#define ec_time_to_ns(time) (time)
#define ec_time_to_us(time) ((time) / 1000LL)
#define ec_time_to_ms(time) ((time) / 1000000LL)
#define ec_us_to_time(us) ((ec_time_t) ((us) * 1000LL))
#define ec_ms_to_time(ms) ((ec_time_t) ((ms) * 1000000LL))

//************************************************************************

static inline void ec_schedule_ms(unsigned long ms) {
    struct timespec ts;

    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;

    nanosleep(&ts, NULL);
}

//************************************************************************

#define EC_IDLE_SEND_INTERVAL 4000

struct ec_device;
typedef struct ec_device ec_device_t;

/* Forward declaration of transport type */
struct ec_transport;

typedef struct {
    struct ec_transport *transport;      /**< Transport layer. */
    ec_time_t last_link_check;           /**< Time of last link state check (ms). */
    int last_link_state;                 /**< Last reported link state (-1 = unknown). */
} ec_device_pal_t;

struct ec_master;
typedef struct ec_master ec_master_t;

#include "cdev.h"


/** Userspace-specific master fields. */
typedef struct {
    struct ec_transport *transport;        /**< Main transport (borrowed). */
    struct ec_transport *backup_transport; /**< Backup transport (borrowed, NULL if none). */
    uint8_t main_mac[ETH_ALEN];           /**< Copied MAC address. */
    uint8_t backup_mac[ETH_ALEN];         /**< Copied backup MAC address. */
} ec_master_pal_t;

#endif /* __EC_USPACE_PAL_H__ */
