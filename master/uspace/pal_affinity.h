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
   PAL: device link-state helper for the userspace master.  (The NIC
   IRQs are pinned once at startup, see ecrt_startup_master().)
   Included after ec_master struct and macros are defined.
*/

#ifndef __EC_USPACE_PAL_AFFINITY_H__
#define __EC_USPACE_PAL_AFFINITY_H__

#include "ectp.h"

/** Periodically refresh the devices' link state from the transports
 * (1 Hz, implemented in device_uspace.c). Runs in the master threads:
 * the transport link query is a syscall (e.g. ioctl(SIOCGIFFLAGS) on
 * the raw transport) and must not run in the application's cyclic
 * receive path. */
void ec_pal_check_link_states(ec_master_t *master);

#endif /* __EC_USPACE_PAL_AFFINITY_H__ */
