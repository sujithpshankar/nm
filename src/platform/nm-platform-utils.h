/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/* nm-platform.c - Handle runtime kernel networking configuration
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Copyright (C) 2015 Red Hat, Inc.
 */

#ifndef __NM_PLATFORM_UTILS_H__
#define __NM_PLATFORM_UTILS_H__

#include "config.h"

#include <gudev/gudev.h>

#include "nm-platform.h"


/**
 * nmp_utils_ip_route_scope_native_to_nm:
 * @scope_native: the scope field of the route in native representation
 *
 * The scope field of an IP address defaults to non-zero value
 * RT_SCOPE_NOWHERE. NMPlatformIP4Route doesn't have a constructor
 * and we initialize the struct with memset(). Hence the default value
 * of the @scope_nm field is zero.
 *
 * That is the reason for the @scope_nm field having a different meaning
 * then the scope in kernel/netlink. This function maps between those
 * two.
 *
 * Returns: @scope in NM representation.
 */
static inline guint8
nmp_utils_ip_route_scope_native_to_nm (guint8 scope_native)
{
	return ~scope_native;
}

/**
 * nmp_utils_ip_route_scope_nm_to_native:
 * @scope_nm: the scope field of the route in NM representation
 *
 * The inverse of nmp_utils_ip_route_scope_native_to_nm.
 *
 * Returns: @scope in native representation.
 */
static inline guint8
nmp_utils_ip_route_scope_nm_to_native (guint8 scope_nm)
{
	return ~scope_nm;
}


const char *nmp_utils_ethtool_get_driver (const char *ifname);
gboolean nmp_utils_ethtool_supports_carrier_detect (const char *ifname);
gboolean nmp_utils_ethtool_supports_vlans (const char *ifname);
int nmp_utils_ethtool_get_peer_ifindex (const char *ifname);
gboolean nmp_utils_ethtool_get_wake_on_lan (const char *ifname);

gboolean nmp_utils_mii_supports_carrier_detect (const char *ifname);

const char *nmp_utils_udev_get_driver (GUdevDevice *device);


#endif /* __NM_PLATFORM_UTILS_H__ */
