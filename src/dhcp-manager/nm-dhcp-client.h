/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/* This program is free software; you can redistribute it and/or modify
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
 * Copyright (C) 2005 - 2010 Red Hat, Inc.
 */

#ifndef __NETWORKMANAGER_DHCP_CLIENT_H__
#define __NETWORKMANAGER_DHCP_CLIENT_H__

#include <glib.h>
#include <glib-object.h>

#include <nm-setting-ip4-config.h>
#include <nm-setting-ip6-config.h>
#include <nm-ip4-config.h>
#include <nm-ip6-config.h>

#define NM_TYPE_DHCP_CLIENT            (nm_dhcp_client_get_type ())
#define NM_DHCP_CLIENT(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), NM_TYPE_DHCP_CLIENT, NMDhcpClient))
#define NM_DHCP_CLIENT_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass), NM_TYPE_DHCP_CLIENT, NMDhcpClientClass))
#define NM_IS_DHCP_CLIENT(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), NM_TYPE_DHCP_CLIENT))
#define NM_IS_DHCP_CLIENT_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass), NM_TYPE_DHCP_CLIENT))
#define NM_DHCP_CLIENT_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj), NM_TYPE_DHCP_CLIENT, NMDhcpClientClass))

#define NM_DHCP_CLIENT_INTERFACE "iface"
#define NM_DHCP_CLIENT_IFINDEX   "ifindex"
#define NM_DHCP_CLIENT_HWADDR    "hwaddr"
#define NM_DHCP_CLIENT_IPV6      "ipv6"
#define NM_DHCP_CLIENT_UUID      "uuid"
#define NM_DHCP_CLIENT_PRIORITY  "priority"
#define NM_DHCP_CLIENT_TIMEOUT   "timeout"

#define NM_DHCP_CLIENT_SIGNAL_STATE_CHANGED "state-changed"

typedef enum {
	NM_DHCP_STATE_UNKNOWN = 0,
	NM_DHCP_STATE_BOUND,        /* lease changed (state_is_bound) */
	NM_DHCP_STATE_TIMEOUT,      /* TIMEOUT */
	NM_DHCP_STATE_DONE,         /* END */
	NM_DHCP_STATE_FAIL,          /* failed or quit unexpectedly */
	__NM_DHCP_STATE_MAX,
	NM_DHCP_STATE_MAX = __NM_DHCP_STATE_MAX - 1,
} NMDhcpState;

typedef struct {
	GObject parent;
} NMDhcpClient;

typedef struct {
	GObjectClass parent;

	/* Methods */

	gboolean (*ip4_start)     (NMDhcpClient *self,
	                           const char *dhcp_client_id,
	                           const char *anycast_addr,
	                           const char *hostname);

	gboolean (*ip6_start)     (NMDhcpClient *self,
	                           const char *anycast_addr,
	                           const char *hostname,
	                           gboolean info_only,
	                           NMSettingIP6ConfigPrivacy privacy,
	                           const GByteArray *duid);

	void (*stop)              (NMDhcpClient *self,
	                           gboolean release,
	                           const GByteArray *duid);

	/**
	 * get_duid:
	 * @self: the #NMDhcpClient
	 *
	 * Attempts to find an existing DHCPv6 DUID for this client in the DHCP
	 * client's persistent configuration.  Returned DUID should be the binary
	 * representation of the DUID.  If no DUID is found, %NULL should be
	 * returned.
	 */
	GByteArray * (*get_duid) (NMDhcpClient *self);

	/* Signals */
	void (*state_changed) (NMDhcpClient *self,
	                       NMDhcpState state,
	                       GObject *ip_config,
	                       GVariant *options);
} NMDhcpClientClass;

GType nm_dhcp_client_get_type (void);

pid_t nm_dhcp_client_get_pid (NMDhcpClient *self);

const char *nm_dhcp_client_get_iface (NMDhcpClient *self);

int         nm_dhcp_client_get_ifindex (NMDhcpClient *self);

gboolean nm_dhcp_client_get_ipv6 (NMDhcpClient *self);

const char *nm_dhcp_client_get_uuid (NMDhcpClient *self);

const GByteArray *nm_dhcp_client_get_duid (NMDhcpClient *self);

const GByteArray *nm_dhcp_client_get_hw_addr (NMDhcpClient *self);

guint32 nm_dhcp_client_get_priority (NMDhcpClient *self);

gboolean nm_dhcp_client_start_ip4 (NMDhcpClient *self,
                                   const char *dhcp_client_id,
                                   const char *dhcp_anycast_addr,
                                   const char *hostname);

gboolean nm_dhcp_client_start_ip6 (NMDhcpClient *self,
                                   const char *dhcp_anycast_addr,
                                   const char *hostname,
                                   gboolean info_only,
                                   NMSettingIP6ConfigPrivacy privacy);

void nm_dhcp_client_stop (NMDhcpClient *self, gboolean release);

void nm_dhcp_client_new_options (NMDhcpClient *self,
                                 GVariant *options,
                                 const char *reason);

/* Backend helpers for subclasses */
void nm_dhcp_client_stop_existing (const char *pid_file, const char *binary_name);

void nm_dhcp_client_stop_pid (pid_t pid, const char *iface);

void nm_dhcp_client_watch_child (NMDhcpClient *self, pid_t pid);

void nm_dhcp_client_set_state (NMDhcpClient *self,
                               NMDhcpState new_state,
                               GObject *ip_config,   /* NMIP4Config or NMIP6Config */
                               GVariant *options); /* str:str hash */

#endif /* __NETWORKMANAGER_DHCP_CLIENT_H__ */

