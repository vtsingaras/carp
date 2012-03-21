/*
 * carp_procfs.c -- procfs interface to carp module
 *
 * Copyright (c) 2012 Damien Churchill <damoxc@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#include <linux/proc_fs.h>
#include "carp.h"
#include "carp_log.h"

void carp_create_proc_entry(struct carp *carp)
{
    struct net_device *carp_dev = carp->dev;
    log("carp: %s", __func__);
}

void __net_init carp_create_proc_dir(struct net_device *nd)
{
    log("carp: %s", __func__);
}
