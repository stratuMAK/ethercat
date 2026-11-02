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
   PAL: IRQ affinity stubs for kernel master (no-op).
*/

#ifndef __EC_KERNEL_PAL_AFFINITY_H__
#define __EC_KERNEL_PAL_AFFINITY_H__

/** No-op: kernel device link state is updated by the NIC driver via
 * ecdev_set_link(). */
static inline void ec_pal_check_link_states(ec_master_t *master)
{
    (void)master;
}

#endif /* __EC_KERNEL_PAL_AFFINITY_H__ */
