/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/* NetworkManager -- Network link manager
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
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Copyright (C) 2005 - 2013 Red Hat, Inc.
 * Copyright (C) 2006 - 2008 Novell, Inc.
 */

#include "config.h"

#include <glib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>

#include "nm-vpn-connection.h"
#include "nm-ip4-config.h"
#include "nm-ip6-config.h"
#include "nm-dbus-manager.h"
#include "nm-platform.h"
#include "nm-logging.h"
#include "nm-active-connection.h"
#include "nm-dbus-glib-types.h"
#include "NetworkManagerUtils.h"
#include "nm-glib-compat.h"
#include "settings/nm-settings-connection.h"
#include "nm-dispatcher.h"
#include "nm-agent-manager.h"
#include "nm-core-internal.h"
#include "nm-default-route-manager.h"
#include "nm-route-manager.h"
#include "gsystem-local-alloc.h"

#include "nm-vpn-connection-glue.h"

G_DEFINE_TYPE (NMVpnConnection, nm_vpn_connection, NM_TYPE_ACTIVE_CONNECTION)

typedef enum {
	/* Only system secrets */
	SECRETS_REQ_SYSTEM = 0,
	/* All existing secrets including agent secrets */
	SECRETS_REQ_EXISTING = 1,
	/* New secrets required; ask an agent */
	SECRETS_REQ_NEW = 2,
	/* Plugin requests secrets interactively */
	SECRETS_REQ_INTERACTIVE = 3,
	/* Placeholder for bounds checking */
	SECRETS_REQ_LAST
} SecretsReq;

/* Internal VPN states, private to NMVpnConnection */
typedef enum {
	STATE_UNKNOWN = 0,
	STATE_WAITING,
	STATE_PREPARE,
	STATE_NEED_AUTH,
	STATE_CONNECT,
	STATE_IP_CONFIG_GET,
	STATE_PRE_UP,
	STATE_ACTIVATED,
	STATE_DEACTIVATING,
	STATE_DISCONNECTED,
	STATE_FAILED,
} VpnState;

typedef struct {
	NMConnection *connection;
	gboolean service_can_persist;
	gboolean connection_can_persist;

	guint32 secrets_id;
	SecretsReq secrets_idx;
	char *username;

	VpnState vpn_state;
	guint dispatcher_id;
	NMVpnConnectionStateReason failure_reason;

	NMVpnServiceState service_state;

	GDBusProxy *proxy;
	GCancellable *cancellable;
	GVariant *connect_hash;
	guint connect_timeout;
	gboolean has_ip4;
	NMIP4Config *ip4_config;
	guint32 ip4_internal_gw;
	guint32 ip4_external_gw;
	gboolean has_ip6;
	NMIP6Config *ip6_config;
	struct in6_addr *ip6_internal_gw;
	struct in6_addr *ip6_external_gw;
	char *ip_iface;
	int ip_ifindex;
	char *banner;
	guint32 mtu;
} NMVpnConnectionPrivate;

#define NM_VPN_CONNECTION_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), NM_TYPE_VPN_CONNECTION, NMVpnConnectionPrivate))

enum {
	VPN_STATE_CHANGED,
	INTERNAL_STATE_CHANGED,
	INTERNAL_RETRY_AFTER_FAILURE,

	LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

enum {
	PROP_0,
	PROP_VPN_STATE,
	PROP_BANNER,
	PROP_IP4_CONFIG,
	PROP_IP6_CONFIG,
	PROP_MASTER = 2000,

	LAST_PROP
};

static void get_secrets (NMVpnConnection *self,
                         SecretsReq secrets_idx,
                         const char **hints);

static void plugin_interactive_secrets_required (NMVpnConnection *self,
                                                 const char *message,
                                                 const char **secrets);

static void _set_vpn_state (NMVpnConnection *connection,
                            VpnState vpn_state,
                            NMVpnConnectionStateReason reason,
                            gboolean quitting);

/*********************************************************************/

static NMVpnConnectionState
_state_to_nm_vpn_state (VpnState state)
{
	switch (state) {
	case STATE_WAITING:
	case STATE_PREPARE:
		return NM_VPN_CONNECTION_STATE_PREPARE;
	case STATE_NEED_AUTH:
		return NM_VPN_CONNECTION_STATE_NEED_AUTH;
	case STATE_CONNECT:
		return NM_VPN_CONNECTION_STATE_CONNECT;
	case STATE_IP_CONFIG_GET:
	case STATE_PRE_UP:
		return NM_VPN_CONNECTION_STATE_IP_CONFIG_GET;
	case STATE_ACTIVATED:
		return NM_VPN_CONNECTION_STATE_ACTIVATED;
	case STATE_DEACTIVATING: {
		/* Map DEACTIVATING to ACTIVATED to preserve external API behavior,
		 * since our API has no DEACTIVATING state of its own.  Since this can
		 * take some time, and the VPN isn't actually disconnected until it
		 * hits the DISCONNECTED state, to clients it should still appear
		 * connected.
		 */
		return NM_VPN_CONNECTION_STATE_ACTIVATED;
	}
	case STATE_DISCONNECTED:
		return NM_VPN_CONNECTION_STATE_DISCONNECTED;
	case STATE_FAILED:
		return NM_VPN_CONNECTION_STATE_FAILED;
	default:
		return NM_VPN_CONNECTION_STATE_UNKNOWN;
	}
}

static NMActiveConnectionState
_state_to_ac_state (VpnState vpn_state)
{
	/* Set the NMActiveConnection state based on VPN state */
	switch (vpn_state) {
	case STATE_WAITING:
	case STATE_PREPARE:
	case STATE_NEED_AUTH:
	case STATE_CONNECT:
	case STATE_IP_CONFIG_GET:
	case STATE_PRE_UP:
		return NM_ACTIVE_CONNECTION_STATE_ACTIVATING;
	case STATE_ACTIVATED:
		return NM_ACTIVE_CONNECTION_STATE_ACTIVATED;
	case STATE_DEACTIVATING:
		return NM_ACTIVE_CONNECTION_STATE_DEACTIVATING;
	case STATE_DISCONNECTED:
	case STATE_FAILED:
		return NM_ACTIVE_CONNECTION_STATE_DEACTIVATED;
	default:
		break;
	}
	return NM_ACTIVE_CONNECTION_STATE_UNKNOWN;
}

static void
call_plugin_disconnect (NMVpnConnection *self)
{
	NMVpnConnectionPrivate *priv = NM_VPN_CONNECTION_GET_PRIVATE (self);

	if (priv->proxy) {
		g_dbus_proxy_call (priv->proxy, "Disconnect", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
		g_clear_object (&priv->proxy);
	}
}

static void
vpn_cleanup (NMVpnConnection *connection, NMDevice *parent_dev)
{
	NMVpnConnectionPrivate *priv = NM_VPN_CONNECTION_GET_PRIVATE (connection);

	if (priv->ip_ifindex) {
		nm_platform_link_set_down (NM_PLATFORM_GET, priv->ip_ifindex);
		nm_route_manager_route_flush (nm_route_manager_get (), priv->ip_ifindex);
		nm_platform_address_flush (NM_PLATFORM_GET, priv->ip_ifindex);
	}

	nm_device_set_vpn4_config (parent_dev, NULL);
	nm_device_set_vpn6_config (parent_dev, NULL);

	g_free (priv->banner);
	priv->banner = NULL;

	g_free (priv->ip_iface);
	priv->ip_iface = NULL;
	priv->ip_ifindex = 0;

	/* Clear out connection secrets to ensure that the settings service
	 * gets asked for them next time the connection is activated.
	 */
	if (priv->connection)
		nm_connection_clear_secrets (priv->connection);
}

static void
dispatcher_pre_down_done (guint call_id, gpointer user_data)
{
	NMVpnConnection *self = NM_VPN_CONNECTION (user_data);
	NMVpnConnectionPrivate *priv = NM_VPN_CONNECTION_GET_PRIVATE (self);

	priv->dispatcher_id = 0;
	_set_vpn_state (self, STATE_DISCONNECTED, NM_VPN_CONNECTION_STATE_REASON_NONE, FALSE);
}

static void
dispatcher_pre_up_done (guint call_id, gpointer user_data)
{
	NMVpnConnection *self = NM_VPN_CONNECTION (user_data);
	NMVpnConnectionPrivate *priv = NM_VPN_CONNECTION_GET_PRIVATE (self);

	priv->dispatcher_id = 0;
	_set_vpn_state (self, STATE_ACTIVATED, NM_VPN_CONNECTION_STATE_REASON_NONE, FALSE);
}

static void
dispatcher_cleanup (NMVpnConnection *self)
{
	NMVpnConnectionPrivate *priv = NM_VPN_CONNECTION_GET_PRIVATE (self);

	if (priv->dispatcher_id) {
		nm_dispatcher_call_cancel (priv->dispatcher_id);
		priv->dispatcher_id = 0;
	}
}

static void
_set_vpn_state (NMVpnConnection *connection,
                VpnState vpn_state,
                NMVpnConnectionStateReason reason,
                gboolean quitting)
{
	NMVpnConnectionPrivate *priv;
	VpnState old_vpn_state;
	NMVpnConnectionState new_external_state, old_external_state;
	NMDevice *parent_dev = nm_active_connection_get_device (NM_ACTIVE_CONNECTION (connection));

	g_return_if_fail (NM_IS_VPN_CONNECTION (connection));

	priv = NM_VPN_CONNECTION_GET_PRIVATE (connection);

	if (vpn_state == priv->vpn_state)
		return;

	old_vpn_state = priv->vpn_state;
	priv->vpn_state = vpn_state;

	/* The device gets destroyed by active connection when it enters
	 * the deactivated state, so we need to ref it for usage below.
	 */
	if (parent_dev)
		g_object_ref (parent_dev);

	/* Update active connection base class state */
	nm_active_connection_set_state (NM_ACTIVE_CONNECTION (connection),
	                                _state_to_ac_state (vpn_state));

	/* Clear any in-progress secrets request */
	if (priv->secrets_id) {
		nm_settings_connection_cancel_secrets (NM_SETTINGS_CONNECTION (priv->connection), priv->secrets_id);
		priv->secrets_id = 0;
	}

	dispatcher_cleanup (connection);

	nm_default_route_manager_ip4_update_default_route (nm_default_route_manager_get (), connection);
	nm_default_route_manager_ip6_update_default_route (nm_default_route_manager_get (), connection);

	/* The connection gets destroyed by the VPN manager when it enters the
	 * disconnected/failed state, but we need to keep it around for a bit
	 * to send out signals and handle the dispatcher.  So ref it.
	 */
	g_object_ref (connection);

	old_external_state = _state_to_nm_vpn_state (old_vpn_state);
	new_external_state = _state_to_nm_vpn_state (priv->vpn_state);
	if (new_external_state != old_external_state) {
		g_signal_emit (connection, signals[VPN_STATE_CHANGED], 0, new_external_state, reason);
		g_signal_emit (connection, signals[INTERNAL_STATE_CHANGED], 0,
		               new_external_state,
		               old_external_state,
		               reason);
		g_object_notify (G_OBJECT (connection), NM_VPN_CONNECTION_VPN_STATE);
	}

	switch (vpn_state) {
	case STATE_NEED_AUTH:
		/* Do nothing; not part of 'default' because we don't want to touch
		 * priv->secrets_req as NEED_AUTH is re-entered during interactive
		 * secrets.
		 */
		break;
	case STATE_PRE_UP:
		if (!nm_dispatcher_call_vpn (DISPATCHER_ACTION_VPN_PRE_UP,
		                             priv->connection,
		                             parent_dev,
		                             priv->ip_iface,
		                             priv->ip4_config,
		                             priv->ip6_config,
		                             dispatcher_pre_up_done,
		                             connection,
		                             &priv->dispatcher_id)) {
			/* Just proceed on errors */
			dispatcher_pre_up_done (0, connection);
		}
		break;
	case STATE_ACTIVATED:
		/* Secrets no longer needed now that we're connected */
		nm_connection_clear_secrets (priv->connection);

		/* Let dispatcher scripts know we're up and running */
		nm_dispatcher_call_vpn (DISPATCHER_ACTION_VPN_UP,
		                        priv->connection,
		                        parent_dev,
		                        priv->ip_iface,
		                        priv->ip4_config,
		                        priv->ip6_config,
		                        NULL,
		                        NULL,
		                        NULL);
		break;
	case STATE_DEACTIVATING:
		if (quitting) {
			nm_dispatcher_call_vpn_sync (DISPATCHER_ACTION_VPN_PRE_DOWN,
			                             priv->connection,
			                             parent_dev,
			                             priv->ip_iface,
			                             priv->ip4_config,
			                             priv->ip6_config);
		} else {
			if (!nm_dispatcher_call_vpn (DISPATCHER_ACTION_VPN_PRE_DOWN,
			                             priv->connection,
			                             parent_dev,
			                             priv->ip_iface,
			                             priv->ip4_config,
			                             priv->ip6_config,
			                             dispatcher_pre_down_done,
			                             connection,
			                             &priv->dispatcher_id)) {
				/* Just proceed on errors */
				dispatcher_pre_down_done (0, connection);
			}
		}
		break;
	case STATE_FAILED:
	case STATE_DISCONNECTED:
		if (   old_vpn_state >= STATE_ACTIVATED
		    && old_vpn_state <= STATE_DEACTIVATING) {
			/* Let dispatcher scripts know we're about to go down */
			if (quitting) {
				nm_dispatcher_call_vpn_sync (DISPATCHER_ACTION_VPN_DOWN,
				                             priv->connection,
				                             parent_dev,
				                             priv->ip_iface,
				                             NULL,
				                             NULL);
			} else {
				nm_dispatcher_call_vpn (DISPATCHER_ACTION_VPN_DOWN,
				                        priv->connection,
				                        parent_dev,
				                        priv->ip_iface,
				                        NULL,
				                        NULL,
				                        NULL,
				                        NULL,
				                        NULL);
			}
		}

		/* Tear down and clean up the connection */
		call_plugin_disconnect (connection);
		vpn_cleanup (connection, parent_dev);
		/* Fall through */
	default:
		priv->secrets_idx = SECRETS_REQ_SYSTEM;
		break;
	}

	g_object_unref (connection);
	if (parent_dev)
		g_object_unref (parent_dev);
}

static gboolean
_service_and_connection_can_persist (NMVpnConnection *self)
{
	return NM_VPN_CONNECTION_GET_PRIVATE (self)->connection_can_persist &&
	       NM_VPN_CONNECTION_GET_PRIVATE (self)->service_can_persist;
}

static gboolean
_connection_only_can_persist (NMVpnConnection *self)
{
	return NM_VPN_CONNECTION_GET_PRIVATE (self)->connection_can_persist &&
	       !NM_VPN_CONNECTION_GET_PRIVATE (self)->service_can_persist;
}

static void
device_state_changed (NMActiveConnection *active,
                      NMDevice *device,
                      NMDeviceState new_state,
                      NMDeviceState old_state)
{
	if (_service_and_connection_can_persist (NM_VPN_CONNECTION (active))) {
		if (new_state <= NM_DEVICE_STATE_DISCONNECTED ||
		    new_state == NM_DEVICE_STATE_FAILED) {
			nm_active_connection_set_device (active, NULL);
		}
		return;
	}

	if (new_state <= NM_DEVICE_STATE_DISCONNECTED) {
		_set_vpn_state (NM_VPN_CONNECTION (active),
		                STATE_DISCONNECTED,
		                NM_VPN_CONNECTION_STATE_REASON_DEVICE_DISCONNECTED,
		                FALSE);
	} else if (new_state == NM_DEVICE_STATE_FAILED) {
		_set_vpn_state (NM_VPN_CONNECTION (active),
		                STATE_FAILED,
		                NM_VPN_CONNECTION_STATE_REASON_DEVICE_DISCONNECTED,
		                FALSE);
	}

	/* FIXME: map device DEACTIVATING state to VPN DEACTIVATING state and
	 * block device deactivation on VPN deactivation.
	 */
}

static void
add_ip4_vpn_gateway_route (NMIP4Config *config, NMDevice *parent_device, guint32 vpn_gw)
{
	NMIP4Config *parent_config;
	guint32 parent_gw;
	NMPlatformIP4Route route;
	guint32 route_metric;

	g_return_if_fail (NM_IS_IP4_CONFIG (config));
	g_return_if_fail (NM_IS_DEVICE (parent_device));
	g_return_if_fail (vpn_gw != 0);

	/* Set up a route to the VPN gateway's public IP address through the default
	 * network device if the VPN gateway is on a different subnet.
	 */

	parent_config = nm_device_get_ip4_config (parent_device);
	g_return_if_fail (parent_config != NULL);
	parent_gw = nm_ip4_config_get_gateway (parent_config);
	if (!parent_gw)
		return;

	route_metric = nm_device_get_ip4_route_metric (parent_device);

	memset (&route, 0, sizeof (route));
	route.network = vpn_gw;
	route.plen = 32;
	route.gateway = parent_gw;

	/* If the VPN gateway is in the same subnet as one of the parent device's
	 * IP addresses, don't add the host route to it, but a route through the
	 * parent device.
	 */
	if (nm_ip4_config_destination_is_direct (parent_config, vpn_gw, 32))
		route.gateway = 0;

	route.source = NM_IP_CONFIG_SOURCE_VPN;
	route.metric = route_metric;
	nm_ip4_config_add_route (config, &route);

	/* Ensure there's a route to the parent device's gateway through the
	 * parent device, since if the VPN claims the default route and the VPN
	 * routes include a subnet that matches the parent device's subnet,
	 * the parent device's gateway would get routed through the VPN and fail.
	 */
	memset (&route, 0, sizeof (route));
	route.network = parent_gw;
	route.plen = 32;
	route.source = NM_IP_CONFIG_SOURCE_VPN;
	route.metric = route_metric;

	nm_ip4_config_add_route (config, &route);
}

static void
add_ip6_vpn_gateway_route (NMIP6Config *config,
                           NMDevice *parent_device,
                           const struct in6_addr *vpn_gw)
{
	NMIP6Config *parent_config;
	const struct in6_addr *parent_gw;
	NMPlatformIP6Route route;
	guint32 route_metric;

	g_return_if_fail (NM_IS_IP6_CONFIG (config));
	g_return_if_fail (NM_IS_DEVICE (parent_device));
	g_return_if_fail (vpn_gw != NULL);

	parent_config = nm_device_get_ip6_config (parent_device);
	g_return_if_fail (parent_config != NULL);
	parent_gw = nm_ip6_config_get_gateway (parent_config);
	if (!parent_gw)
		return;

	route_metric = nm_device_get_ip6_route_metric (parent_device);

	memset (&route, 0, sizeof (route));
	route.network = *vpn_gw;
	route.plen = 128;
	route.gateway = *parent_gw;

	/* If the VPN gateway is in the same subnet as one of the parent device's
	 * IP addresses, don't add the host route to it, but a route through the
	 * parent device.
	 */
	if (nm_ip6_config_destination_is_direct (parent_config, vpn_gw, 128))
		route.gateway = in6addr_any;

	route.source = NM_IP_CONFIG_SOURCE_VPN;
	route.metric = route_metric;
	nm_ip6_config_add_route (config, &route);

	/* Ensure there's a route to the parent device's gateway through the
	 * parent device, since if the VPN claims the default route and the VPN
	 * routes include a subnet that matches the parent device's subnet,
	 * the parent device's gateway would get routed through the VPN and fail.
	 */
	memset (&route, 0, sizeof (route));
	route.network = *parent_gw;
	route.plen = 128;
	route.source = NM_IP_CONFIG_SOURCE_VPN;
	route.metric = route_metric;

	nm_ip6_config_add_route (config, &route);
}

NMVpnConnection *
nm_vpn_connection_new (NMConnection *connection,
                       NMDevice *parent_device,
                       const char *specific_object,
                       NMAuthSubject *subject)
{
	g_return_val_if_fail (NM_IS_CONNECTION (connection), NULL);
	g_return_val_if_fail (NM_IS_DEVICE (parent_device), NULL);

	return (NMVpnConnection *) g_object_new (NM_TYPE_VPN_CONNECTION,
	                                         NM_ACTIVE_CONNECTION_INT_CONNECTION, connection,
	                                         NM_ACTIVE_CONNECTION_INT_DEVICE, parent_device,
	                                         NM_ACTIVE_CONNECTION_SPECIFIC_OBJECT, specific_object,
	                                         NM_ACTIVE_CONNECTION_INT_SUBJECT, subject,
	                                         NM_ACTIVE_CONNECTION_VPN, TRUE,
	                                         NULL);
}

static const char *
nm_vpn_connection_get_service (NMVpnConnection *connection)
{
	NMVpnConnectionPrivate *priv = NM_VPN_CONNECTION_GET_PRIVATE (connection);
	NMSettingVpn *s_vpn;

	s_vpn = nm_connection_get_setting_vpn (priv->connection);
	return nm_setting_vpn_get_service_type (s_vpn);
}

static const char *
vpn_plugin_failure_to_string (NMVpnPluginFailure failure)
{
	switch (failure) {
	case NM_VPN_PLUGIN_FAILURE_LOGIN_FAILED:
		return "login-failed";
	case NM_VPN_PLUGIN_FAILURE_CONNECT_FAILED:
		return "connect-failed";
	case NM_VPN_PLUGIN_FAILURE_BAD_IP_CONFIG:
		return "bad-ip-config";
	default:
		break;
	}
	return "unknown";
}

static void
plugin_failed (NMVpnConnection *self, guint reason)
{
	NMVpnConnectionPrivate *priv = NM_VPN_CONNECTION_GET_PRIVATE (self);

	nm_log_warn (LOGD_VPN, "VPN plugin failed: %s (%d)", vpn_plugin_failure_to_string (reason), reason);

	switch (reason) {
	case NM_VPN_PLUGIN_FAILURE_LOGIN_FAILED:
		priv->failure_reason = NM_VPN_CONNECTION_STATE_REASON_LOGIN_FAILED;
		break;
	case NM_VPN_PLUGIN_FAILURE_BAD_IP_CONFIG:
		priv->failure_reason = NM_VPN_CONNECTION_STATE_REASON_IP_CONFIG_INVALID;
		break;
	default:
		priv->failure_reason = NM_VPN_CONNECTION_STATE_REASON_UNKNOWN;
		break;
	}
}

static const char *
vpn_service_state_to_string (NMVpnServiceState state)
{
	switch (state) {
	case NM_VPN_SERVICE_STATE_INIT:
		return "init";
	case NM_VPN_SERVICE_STATE_SHUTDOWN:
		return "shutdown";
	case NM_VPN_SERVICE_STATE_STARTING:
		return "starting";
	case NM_VPN_SERVICE_STATE_STARTED:
		return "started";
	case NM_VPN_SERVICE_STATE_STOPPING:
		return "stopping";
	case NM_VPN_SERVICE_STATE_STOPPED:
		return "stopped";
	default:
		break;
	}
	return "unknown";
}

static const char *state_table[] = {
	[STATE_UNKNOWN]       = "unknown",
	[STATE_WAITING]       = "waiting",
	[STATE_PREPARE]       = "prepare",
	[STATE_NEED_AUTH]     = "need-auth",
	[STATE_CONNECT]       = "connect",
	[STATE_IP_CONFIG_GET] = "ip-config-get",
	[STATE_PRE_UP]        = "pre-up",
	[STATE_ACTIVATED]     = "activated",
	[STATE_DEACTIVATING]  = "deactivating",
	[STATE_DISCONNECTED]  = "disconnected",
	[STATE_FAILED]        = "failed",
};

static const char *
vpn_state_to_string (VpnState state)
{
	if ((gsize) state < G_N_ELEMENTS (state_table))
		return state_table[state];
	return "unknown";
}

static const char *
vpn_reason_to_string (NMVpnConnectionStateReason reason)
{
	switch (reason) {
	case NM_VPN_CONNECTION_STATE_REASON_NONE:
		return "none";
	case NM_VPN_CONNECTION_STATE_REASON_USER_DISCONNECTED:
		return "user-disconnected";
	case NM_VPN_CONNECTION_STATE_REASON_DEVICE_DISCONNECTED:
		return "device-disconnected";
	case NM_VPN_CONNECTION_STATE_REASON_SERVICE_STOPPED:
		return "service-stopped";
	case NM_VPN_CONNECTION_STATE_REASON_IP_CONFIG_INVALID:
		return "ip-config-invalid";
	case NM_VPN_CONNECTION_STATE_REASON_CONNECT_TIMEOUT:
		return "connect-timeout";
	case NM_VPN_CONNECTION_STATE_REASON_SERVICE_START_TIMEOUT:
		return "service-start-timeout";
	case NM_VPN_CONNECTION_STATE_REASON_SERVICE_START_FAILED:
		return "service-start-failed";
	case NM_VPN_CONNECTION_STATE_REASON_NO_SECRETS:
		return "no-secrets";
	case NM_VPN_CONNECTION_STATE_REASON_LOGIN_FAILED:
		return "login-failed";
	case NM_VPN_CONNECTION_STATE_REASON_CONNECTION_REMOVED:
		return "connection-removed";
	default:
		break;
	}
	return "unknown";
}

static void
plugin_state_changed (NMVpnConnection *self, NMVpnServiceState new_service_state)
{
	NMVpnConnectionPrivate *priv = NM_VPN_CONNECTION_GET_PRIVATE (self);
	NMVpnServiceState old_service_state = priv->service_state;

	nm_log_info (LOGD_VPN, "VPN plugin state changed: %s (%d)",
	             vpn_service_state_to_string (new_service_state), new_service_state);
	priv->service_state = new_service_state;

	if (new_service_state == NM_VPN_SERVICE_STATE_STOPPED) {
		/* Clear connection secrets to ensure secrets get requested each time the
		 * connection is activated.
		 */
		nm_connection_clear_secrets (priv->connection);

		if ((priv->vpn_state >= STATE_WAITING) && (priv->vpn_state <= STATE_ACTIVATED)) {
			VpnState old_state = priv->vpn_state;

			nm_log_info (LOGD_VPN, "VPN plugin state change reason: %s (%d)",
			             vpn_reason_to_string (priv->failure_reason), priv->failure_reason);
			_set_vpn_state (self, STATE_FAILED, priv->failure_reason, FALSE);

			/* Reset the failure reason */
			priv->failure_reason = NM_VPN_CONNECTION_STATE_REASON_UNKNOWN;

			/* If the connection failed, the service cannot persist, but the
			 * connection can persist, ask listeners to re-activate the connection.
			 */
			if (   old_state == STATE_ACTIVATED
			    && priv->vpn_state == STATE_FAILED
			    && _connection_only_can_persist (self))
				g_signal_emit (self, signals[INTERNAL_RETRY_AFTER_FAILURE], 0);
		}
	} else if (new_service_state == NM_VPN_SERVICE_STATE_STARTING &&
	           old_service_state == NM_VPN_SERVICE_STATE_STARTED) {
		/* The VPN service got disconnected and is attempting to reconnect */
		_set_vpn_state (self, STATE_CONNECT, NM_VPN_CONNECTION_STATE_REASON_CONNECT_TIMEOUT, FALSE);
	}
}

static void
print_vpn_config (NMVpnConnection *connection)
{
	NMVpnConnectionPrivate *priv = NM_VPN_CONNECTION_GET_PRIVATE (connection);
	const NMPlatformIP4Address *address4;
	const NMPlatformIP6Address *address6;
	char *dns_domain = NULL;
	guint32 num, i;
	char buf[NM_UTILS_INET_ADDRSTRLEN];

	if (priv->ip4_external_gw) {
		nm_log_info (LOGD_VPN, "VPN Gateway: %s",
		             nm_utils_inet4_ntop (priv->ip4_external_gw, NULL));
	} else if (priv->ip6_external_gw) {
		nm_log_info (LOGD_VPN, "VPN Gateway: %s",
		             nm_utils_inet6_ntop (priv->ip6_external_gw, NULL));
	}

	nm_log_info (LOGD_VPN, "Tunnel Device: %s", priv->ip_iface ? priv->ip_iface : "(none)");

	if (priv->ip4_config) {
		nm_log_info (LOGD_VPN, "IPv4 configuration:");

		address4 = nm_ip4_config_get_address (priv->ip4_config, 0);

		if (priv->ip4_internal_gw)
			nm_log_info (LOGD_VPN, "  Internal Gateway: %s", nm_utils_inet4_ntop (priv->ip4_internal_gw, NULL));
		nm_log_info (LOGD_VPN, "  Internal Address: %s", nm_utils_inet4_ntop (address4->address, NULL));
		nm_log_info (LOGD_VPN, "  Internal Prefix: %d", address4->plen);
		nm_log_info (LOGD_VPN, "  Internal Point-to-Point Address: %s", nm_utils_inet4_ntop (address4->peer_address, NULL));
		nm_log_info (LOGD_VPN, "  Maximum Segment Size (MSS): %d", nm_ip4_config_get_mss (priv->ip4_config));

		num = nm_ip4_config_get_num_routes (priv->ip4_config);
		for (i = 0; i < num; i++) {
			const NMPlatformIP4Route *route = nm_ip4_config_get_route (priv->ip4_config, i);

			nm_log_info (LOGD_VPN, "  Static Route: %s/%d   Next Hop: %s",
			             nm_utils_inet4_ntop (route->network, NULL),
			             route->plen,
			             nm_utils_inet4_ntop (route->gateway, buf));
		}

		nm_log_info (LOGD_VPN, "  Forbid Default Route: %s",
		             nm_ip4_config_get_never_default (priv->ip4_config) ? "yes" : "no");

		num = nm_ip4_config_get_num_nameservers (priv->ip4_config);
		for (i = 0; i < num; i++) {
			nm_log_info (LOGD_VPN, "  Internal DNS: %s",
			             nm_utils_inet4_ntop (nm_ip4_config_get_nameserver (priv->ip4_config, i), NULL));
		}

		if (nm_ip4_config_get_num_domains (priv->ip4_config) > 0)
			dns_domain = (char *) nm_ip4_config_get_domain (priv->ip4_config, 0);

		nm_log_info (LOGD_VPN, "  DNS Domain: '%s'", dns_domain ? dns_domain : "(none)");
	} else
		nm_log_info (LOGD_VPN, "No IPv4 configuration");

	if (priv->ip6_config) {
		nm_log_info (LOGD_VPN, "IPv6 configuration:");

		address6 = nm_ip6_config_get_address (priv->ip6_config, 0);

		if (priv->ip6_internal_gw)
			nm_log_info (LOGD_VPN, "  Internal Gateway: %s", nm_utils_inet6_ntop (priv->ip6_internal_gw, NULL));
		nm_log_info (LOGD_VPN, "  Internal Address: %s", nm_utils_inet6_ntop (&address6->address, NULL));
		nm_log_info (LOGD_VPN, "  Internal Prefix: %d", address6->plen);
		nm_log_info (LOGD_VPN, "  Internal Point-to-Point Address: %s", nm_utils_inet6_ntop (&address6->peer_address, NULL));
		nm_log_info (LOGD_VPN, "  Maximum Segment Size (MSS): %d", nm_ip6_config_get_mss (priv->ip6_config));

		num = nm_ip6_config_get_num_routes (priv->ip6_config);
		for (i = 0; i < num; i++) {
			const NMPlatformIP6Route *route = nm_ip6_config_get_route (priv->ip6_config, i);

			nm_log_info (LOGD_VPN, "  Static Route: %s/%d   Next Hop: %s",
			             nm_utils_inet6_ntop (&route->network, NULL),
			             route->plen,
			             nm_utils_inet6_ntop (&route->gateway, buf));
		}

		nm_log_info (LOGD_VPN, "  Forbid Default Route: %s",
		             nm_ip6_config_get_never_default (priv->ip6_config) ? "yes" : "no");

		num = nm_ip6_config_get_num_nameservers (priv->ip6_config);
		for (i = 0; i < num; i++) {
			nm_log_info (LOGD_VPN, "  Internal DNS: %s",
			             nm_utils_inet6_ntop (nm_ip6_config_get_nameserver (priv->ip6_config, i), NULL));
		}

		if (nm_ip6_config_get_num_domains (priv->ip6_config) > 0)
			dns_domain = (char *) nm_ip6_config_get_domain (priv->ip6_config, 0);

		nm_log_info (LOGD_VPN, "  DNS Domain: '%s'", dns_domain ? dns_domain : "(none)");
	} else
		nm_log_info (LOGD_VPN, "No IPv6 configuration");

	if (priv->banner && strlen (priv->banner)) {
		nm_log_info (LOGD_VPN, "Login Banner:");
		nm_log_info (LOGD_VPN, "-----------------------------------------");
		nm_log_info (LOGD_VPN, "%s", priv->banner);
		nm_log_info (LOGD_VPN, "-----------------------------------------");
	}
}

static void
apply_parent_device_config (NMVpnConnection *connection)
{
	NMVpnConnectionPrivate *priv = NM_VPN_CONNECTION_GET_PRIVATE (connection);
	NMDevice *parent_dev = nm_active_connection_get_device (NM_ACTIVE_CONNECTION (connection));
	NMIP4Config *vpn4_parent_config = NULL;
	NMIP6Config *vpn6_parent_config = NULL;

	if (priv->ip4_config)
		vpn4_parent_config = nm_ip4_config_new (priv->ip_ifindex);
	if (priv->ip6_config)
		vpn6_parent_config = nm_ip6_config_new (priv->ip_ifindex);

	if (priv->ip_ifindex <= 0) {
		/* If the VPN didn't return a network interface, it is a route-based
		 * VPN (like kernel IPSec) and all IP addressing and routing should
		 * be done on the parent interface instead.
		 */

		if (vpn4_parent_config)
			nm_ip4_config_merge (vpn4_parent_config, priv->ip4_config);
		if (vpn6_parent_config)
			nm_ip6_config_merge (vpn6_parent_config, priv->ip6_config);
	}

	if (vpn4_parent_config) {
		/* Add any explicit route to the VPN gateway through the parent device */
		if (priv->ip4_external_gw)
			add_ip4_vpn_gateway_route (vpn4_parent_config, parent_dev, priv->ip4_external_gw);

		nm_device_set_vpn4_config (parent_dev, vpn4_parent_config);
		g_object_unref (vpn4_parent_config);
	}
	if (vpn6_parent_config) {
		/* Add any explicit route to the VPN gateway through the parent device */
		if (priv->ip6_external_gw)
			add_ip6_vpn_gateway_route (vpn6_parent_config, parent_dev, priv->ip6_external_gw);

		nm_device_set_vpn6_config (parent_dev, vpn6_parent_config);
		g_object_unref (vpn6_parent_config);
	}
}

static gboolean
nm_vpn_connection_apply_config (NMVpnConnection *connection)
{
	NMVpnConnectionPrivate *priv = NM_VPN_CONNECTION_GET_PRIVATE (connection);

	if (priv->ip_ifindex > 0) {
		nm_platform_link_set_up (NM_PLATFORM_GET, priv->ip_ifindex, NULL);

		if (priv->ip4_config) {
			if (!nm_ip4_config_commit (priv->ip4_config, priv->ip_ifindex,
			                           nm_vpn_connection_get_ip4_route_metric (connection)))
				return FALSE;
		}

		if (priv->ip6_config) {
			if (!nm_ip6_config_commit (priv->ip6_config, priv->ip_ifindex))
				return FALSE;
		}
	}

	apply_parent_device_config (connection);

	nm_default_route_manager_ip4_update_default_route (nm_default_route_manager_get (), connection);
	nm_default_route_manager_ip6_update_default_route (nm_default_route_manager_get (), connection);

	nm_log_info (LOGD_VPN, "VPN connection '%s' (IP Config Get) complete.",
	             nm_connection_get_id (priv->connection));
	_set_vpn_state (connection, STATE_PRE_UP, NM_VPN_CONNECTION_STATE_REASON_NONE, FALSE);
	return TRUE;
}

static void
nm_vpn_connection_config_maybe_complete (NMVpnConnection *connection,
                                         gboolean         success)
{
	NMVpnConnectionPrivate *priv = NM_VPN_CONNECTION_GET_PRIVATE (connection);

	if (priv->vpn_state < STATE_IP_CONFIG_GET || priv->vpn_state > STATE_ACTIVATED)
		return;

	if (success) {
		if (   (priv->has_ip4 && !priv->ip4_config)
		    || (priv->has_ip6 && !priv->ip6_config)) {
			/* Need to wait for other config */
			return;
		}
	}

	if (priv->connect_timeout) {
		g_source_remove (priv->connect_timeout);
		priv->connect_timeout = 0;
	}

	if (success) {
		print_vpn_config (connection);

		if (nm_vpn_connection_apply_config (connection))
			return;
	}

	g_clear_object (&priv->ip4_config);
	g_clear_object (&priv->ip6_config);

	nm_log_warn (LOGD_VPN, "VPN connection '%s' did not receive valid IP config information.",
	             nm_connection_get_id (priv->connection));
	_set_vpn_state (connection, STATE_FAILED, NM_VPN_CONNECTION_STATE_REASON_IP_CONFIG_INVALID, FALSE);
}

static gboolean
ip6_addr_from_variant (GVariant *v, struct in6_addr *addr)
{
	const guint8 *bytes;
	gsize len;

	g_return_val_if_fail (v, FALSE);
	g_return_val_if_fail (addr, FALSE);

	if (g_variant_is_of_type (v, G_VARIANT_TYPE ("ay"))) {
		bytes = g_variant_get_fixed_array (v, &len, sizeof (guint8));
		if (len == sizeof (struct in6_addr) && !IN6_IS_ADDR_UNSPECIFIED (bytes)) {
			memcpy (addr, bytes, len);
			return TRUE;
		}
	}
	return FALSE;
}

static struct in6_addr *
ip6_addr_dup_from_variant (GVariant *v)
{
	struct in6_addr *addr;

	addr = g_malloc0 (sizeof (*addr));
	if (ip6_addr_from_variant (v, addr))
		return addr;
	g_free (addr);
	return NULL;
}

static gboolean
process_generic_config (NMVpnConnection *self, GVariant *dict)
{
	NMVpnConnectionPrivate *priv = NM_VPN_CONNECTION_GET_PRIVATE (self);
	const char *str;
	GVariant *v;
	guint32 u32;
	gboolean b, success = FALSE;

	if (g_variant_lookup (dict, NM_VPN_PLUGIN_CAN_PERSIST, "b", &b) && b) {
		/* Defaults to FALSE, so only let service indicate TRUE */
		priv->service_can_persist = TRUE;
	}

	g_clear_pointer (&priv->ip_iface, g_free);
	if (g_variant_lookup (dict, NM_VPN_PLUGIN_CONFIG_TUNDEV, "&s", &str)) {
		/* Backwards compat with NM-openswan */
		if (g_strcmp0 (str, "_none_") != 0)
			priv->ip_iface = g_strdup (str);
	}

	if (priv->ip_iface) {
		/* Grab the interface index for address/routing operations */
		priv->ip_ifindex = nm_platform_link_get_ifindex (NM_PLATFORM_GET, priv->ip_iface);
		if (!priv->ip_ifindex) {
			nm_log_err (LOGD_VPN, "(%s): failed to look up VPN interface index", priv->ip_iface);
			nm_vpn_connection_config_maybe_complete (self, FALSE);
			return FALSE;
		}
	}

	g_clear_pointer (&priv->banner, g_free);
	if (g_variant_lookup (dict, NM_VPN_PLUGIN_CONFIG_BANNER, "&s", &str))
		priv->banner = g_strdup (str);

	/* External world-visible address of the VPN server */
	priv->ip4_external_gw = 0;
	g_clear_pointer (&priv->ip6_external_gw, g_free);

	if (g_variant_lookup (dict, NM_VPN_PLUGIN_CONFIG_EXT_GATEWAY, "u", &u32)) {
		priv->ip4_external_gw = u32;
		success = TRUE;
	} else if (g_variant_lookup (dict, NM_VPN_PLUGIN_CONFIG_EXT_GATEWAY, "@ay", &v)) {
		priv->ip6_external_gw = ip6_addr_dup_from_variant (v);
		success = !!priv->ip6_external_gw;
		g_variant_unref (v);
	}

	if (!success) {
		nm_log_err (LOGD_VPN, "(%s): VPN gateway is neither IPv4 nor IPv6", priv->ip_iface);
		nm_vpn_connection_config_maybe_complete (self, FALSE);
		return FALSE;
	}

	/* MTU; this is a per-connection value, though NM's API treats it
	 * like it's IP4-specific. So we store it for now and retrieve it
	 * later in ip4_config_get.
	 */
	priv->mtu = 0;
	if (g_variant_lookup (dict, NM_VPN_PLUGIN_CONFIG_EXT_GATEWAY, "u", &u32))
		priv->mtu = u32;

	return TRUE;
}

static void
nm_vpn_connection_config_get (NMVpnConnection *self, GVariant *dict)
{
	NMVpnConnectionPrivate *priv = NM_VPN_CONNECTION_GET_PRIVATE (self);
	gboolean b;

	g_return_if_fail (dict && g_variant_is_of_type (dict, G_VARIANT_TYPE_VARDICT));

	nm_log_info (LOGD_VPN, "VPN connection '%s' (IP Config Get) reply received.",
	             nm_connection_get_id (priv->connection));

	if (priv->vpn_state == STATE_CONNECT)
		_set_vpn_state (self, STATE_IP_CONFIG_GET, NM_VPN_CONNECTION_STATE_REASON_NONE, FALSE);

	if (!process_generic_config (self, dict))
		return;

	/* Note whether to expect IPv4 and IPv6 configs */
	priv->has_ip4 = FALSE;
	if (g_variant_lookup (dict, NM_VPN_PLUGIN_CONFIG_HAS_IP4, "b", &b))
		priv->has_ip4 = b;
	g_clear_object (&priv->ip4_config);

	priv->has_ip6 = FALSE;
	if (g_variant_lookup (dict, NM_VPN_PLUGIN_CONFIG_HAS_IP6, "b", &b))
		priv->has_ip6 = b;
	g_clear_object (&priv->ip6_config);
}

guint32
nm_vpn_connection_get_ip4_route_metric (NMVpnConnection *connection)
{
	NMVpnConnectionPrivate *priv = NM_VPN_CONNECTION_GET_PRIVATE (connection);

	if (priv->connection) {
		gint64 route_metric = nm_setting_ip_config_get_route_metric (nm_connection_get_setting_ip4_config (priv->connection));

		if (route_metric >= 0)
			return route_metric;
	}

	return NM_VPN_ROUTE_METRIC_DEFAULT;
}

guint32
nm_vpn_connection_get_ip6_route_metric (NMVpnConnection *connection)
{
	NMVpnConnectionPrivate *priv = NM_VPN_CONNECTION_GET_PRIVATE (connection);

	if (priv->connection) {
		gint64 route_metric = nm_setting_ip_config_get_route_metric (nm_connection_get_setting_ip6_config (priv->connection));

		if (route_metric >= 0)
			return route_metric;
	}

	return NM_VPN_ROUTE_METRIC_DEFAULT;
}

static void
nm_vpn_connection_ip4_config_get (NMVpnConnection *self, GVariant *dict)
{
	NMVpnConnectionPrivate *priv = NM_VPN_CONNECTION_GET_PRIVATE (self);
	NMPlatformIP4Address address;
	NMIP4Config *config;
	guint32 u32, route_metric;
	GVariantIter *iter;
	const char *str;
	GVariant *v;
	gboolean b;

	g_return_if_fail (dict && g_variant_is_of_type (dict, G_VARIANT_TYPE_VARDICT));

	if (priv->vpn_state == STATE_CONNECT)
		_set_vpn_state (self, STATE_IP_CONFIG_GET, NM_VPN_CONNECTION_STATE_REASON_NONE, FALSE);

	if (priv->has_ip4) {
		nm_log_info (LOGD_VPN, "VPN connection '%s' (IP4 Config Get) reply received.",
		             nm_connection_get_id (priv->connection));

		if (g_variant_n_children (dict) == 0) {
			priv->has_ip4 = FALSE;
			nm_vpn_connection_config_maybe_complete (self, TRUE);
			return;
		}
	} else {
		nm_log_info (LOGD_VPN, "VPN connection '%s' (IP4 Config Get) reply received from old-style plugin.",
		             nm_connection_get_id (priv->connection));

		/* In the old API, the generic and IPv4 configuration items
		 * were mixed together.
		 */
		if (!process_generic_config (self, dict))
			return;

		priv->has_ip4 = TRUE;
		priv->has_ip6 = FALSE;
	}

	config = nm_ip4_config_new (priv->ip_ifindex);

	memset (&address, 0, sizeof (address));
	address.plen = 24;
	if (priv->ip4_external_gw)
		nm_ip4_config_set_gateway (config, priv->ip4_external_gw);

	/* Internal address of the VPN subnet's gateway */
	if (g_variant_lookup (dict, NM_VPN_PLUGIN_IP4_CONFIG_INT_GATEWAY, "u", &u32))
		priv->ip4_internal_gw = u32;

	if (g_variant_lookup (dict, NM_VPN_PLUGIN_IP4_CONFIG_ADDRESS, "u", &u32))
		address.address = u32;

	if (g_variant_lookup (dict, NM_VPN_PLUGIN_IP4_CONFIG_PTP, "u", &u32))
		address.peer_address = u32;

	if (g_variant_lookup (dict, NM_VPN_PLUGIN_IP4_CONFIG_PREFIX, "u", &u32))
		address.plen = u32;

	if (address.address && address.plen) {
		address.source = NM_IP_CONFIG_SOURCE_VPN;
		nm_ip4_config_add_address (config, &address);
	} else {
		nm_log_err (LOGD_VPN, "invalid IP4 config received!");
		g_object_unref (config);
		nm_vpn_connection_config_maybe_complete (self, FALSE);
		return;
	}

	if (g_variant_lookup (dict, NM_VPN_PLUGIN_IP4_CONFIG_DNS, "au", &iter)) {
		while (g_variant_iter_next (iter, "u", &u32))
			nm_ip4_config_add_nameserver (config, u32);
		g_variant_iter_free (iter);
	}

	if (g_variant_lookup (dict, NM_VPN_PLUGIN_IP4_CONFIG_NBNS, "au", &iter)) {
		while (g_variant_iter_next (iter, "u", &u32))
			nm_ip4_config_add_wins (config, u32);
		g_variant_iter_free (iter);
	}

	if (g_variant_lookup (dict, NM_VPN_PLUGIN_IP4_CONFIG_MSS, "u", &u32))
		nm_ip4_config_set_mss (config, u32);

	if (priv->mtu)
		nm_ip4_config_set_mtu (config, priv->mtu, NM_IP_CONFIG_SOURCE_VPN);

	if (g_variant_lookup (dict, NM_VPN_PLUGIN_IP4_CONFIG_DOMAIN, "&s", &str))
		nm_ip4_config_add_domain (config, str);

	if (g_variant_lookup (dict, NM_VPN_PLUGIN_IP4_CONFIG_DOMAINS, "as", &iter)) {
		while (g_variant_iter_next (iter, "&s", &str))
			nm_ip4_config_add_domain (config, str);
		g_variant_iter_free (iter);
	}

	route_metric = nm_vpn_connection_get_ip4_route_metric (self);

	if (g_variant_lookup (dict, NM_VPN_PLUGIN_IP4_CONFIG_ROUTES, "aau", &iter)) {
		while (g_variant_iter_next (iter, "@au", &v)) {
			NMPlatformIP4Route route;

			if (g_variant_n_children (v) == 4) {
				memset (&route, 0, sizeof (route));
				g_variant_get_child (v, 0, "u", &route.network);
				g_variant_get_child (v, 1, "u", &route.plen);
				g_variant_get_child (v, 2, "u", &route.gateway);
				/* 4th item is unused route metric */
				route.metric = route_metric;
				route.source = NM_IP_CONFIG_SOURCE_VPN;

				/* Ignore host routes to the VPN gateway since NM adds one itself
				 * below.  Since NM knows more about the routing situation than
				 * the VPN server, we want to use the NM created route instead of
				 * whatever the server provides.
				 */
				if (!(priv->ip4_external_gw && route.network == priv->ip4_external_gw && route.plen == 32))
					nm_ip4_config_add_route (config, &route);
			}
			g_variant_unref (v);
		}
		g_variant_iter_free (iter);
	}

	if (g_variant_lookup (dict, NM_VPN_PLUGIN_IP4_CONFIG_NEVER_DEFAULT, "b", &b))
		nm_ip4_config_set_never_default (config, b);

	/* Merge in user overrides from the NMConnection's IPv4 setting */
	nm_ip4_config_merge_setting (config,
	                             nm_connection_get_setting_ip4_config (priv->connection),
	                             route_metric);

	g_clear_object (&priv->ip4_config);
	priv->ip4_config = config;
	nm_ip4_config_export (config);
	g_object_notify (G_OBJECT (self), NM_ACTIVE_CONNECTION_IP4_CONFIG);
	nm_vpn_connection_config_maybe_complete (self, TRUE);
}

static void
nm_vpn_connection_ip6_config_get (NMVpnConnection *self, GVariant *dict)
{
	NMVpnConnectionPrivate *priv = NM_VPN_CONNECTION_GET_PRIVATE (self);
	NMPlatformIP6Address address;
	guint32 u32, route_metric;
	NMIP6Config *config;
	GVariantIter *iter;
	const char *str;
	GVariant *v;
	gboolean b;

	g_return_if_fail (dict && g_variant_is_of_type (dict, G_VARIANT_TYPE_VARDICT));

	nm_log_info (LOGD_VPN, "VPN connection '%s' (IP6 Config Get) reply received.",
	             nm_connection_get_id (priv->connection));

	if (priv->vpn_state == STATE_CONNECT)
		_set_vpn_state (self, STATE_IP_CONFIG_GET, NM_VPN_CONNECTION_STATE_REASON_NONE, FALSE);

	if (g_variant_n_children (dict) == 0) {
		priv->has_ip6 = FALSE;
		nm_vpn_connection_config_maybe_complete (self, TRUE);
		return;
	}

	config = nm_ip6_config_new (priv->ip_ifindex);

	memset (&address, 0, sizeof (address));
	address.plen = 128;
	if (priv->ip6_external_gw)
		nm_ip6_config_set_gateway (config, priv->ip6_external_gw);

	/* Internal address of the VPN subnet's gateway */
	g_clear_pointer (&priv->ip6_internal_gw, g_free);
	if (g_variant_lookup (dict, NM_VPN_PLUGIN_IP6_CONFIG_INT_GATEWAY, "@ay", &v)) {
		priv->ip6_internal_gw = ip6_addr_dup_from_variant (v);
		g_variant_unref (v);
	}

	if (g_variant_lookup (dict, NM_VPN_PLUGIN_IP6_CONFIG_ADDRESS, "@ay", &v)) {
		ip6_addr_from_variant (v, &address.address);
		g_variant_unref (v);
	}

	if (g_variant_lookup (dict, NM_VPN_PLUGIN_IP6_CONFIG_PTP, "@ay", &v)) {
		ip6_addr_from_variant (v, &address.peer_address);
		g_variant_unref (v);
	}

	if (g_variant_lookup (dict, NM_VPN_PLUGIN_IP6_CONFIG_PREFIX, "u", &u32))
		address.plen = u32;

	if (!IN6_IS_ADDR_UNSPECIFIED (&address.address) && address.plen) {
		address.source = NM_IP_CONFIG_SOURCE_VPN;
		nm_ip6_config_add_address (config, &address);
	} else {
		nm_log_err (LOGD_VPN, "invalid IP6 config received!");
		g_object_unref (config);
		nm_vpn_connection_config_maybe_complete (self, FALSE);
		return;
	}

	if (g_variant_lookup (dict, NM_VPN_PLUGIN_IP6_CONFIG_DNS, "aay", &iter)) {
		while (g_variant_iter_next (iter, "@ay", &v)) {
			struct in6_addr dns;

			if (ip6_addr_from_variant (v, &dns))
				nm_ip6_config_add_nameserver (config, &dns);
			g_variant_unref (v);
		}
		g_variant_iter_free (iter);
	}

	if (g_variant_lookup (dict, NM_VPN_PLUGIN_IP6_CONFIG_MSS, "u", &u32))
		nm_ip6_config_set_mss (config, u32);

	if (g_variant_lookup (dict, NM_VPN_PLUGIN_IP6_CONFIG_DOMAIN, "&s", &str))
		nm_ip6_config_add_domain (config, str);

	if (g_variant_lookup (dict, NM_VPN_PLUGIN_IP6_CONFIG_DOMAINS, "as", &iter)) {
		while (g_variant_iter_next (iter, "&s", &str))
			nm_ip6_config_add_domain (config, str);
		g_variant_iter_free (iter);
	}

	route_metric = nm_vpn_connection_get_ip6_route_metric (self);

	if (g_variant_lookup (dict, NM_VPN_PLUGIN_IP6_CONFIG_ROUTES, "a(ayuayu)", &iter)) {
		GVariant *dest, *next_hop;
		guint32 prefix, metric;

		while (g_variant_iter_next (iter, "(@ayu@ayu)", &dest, &prefix, &next_hop, &metric)) {
			NMPlatformIP6Route route;

			memset (&route, 0, sizeof (route));

			if (!ip6_addr_from_variant (dest, &route.network)) {
				nm_log_warn (LOGD_VPN, "VPN connection '%s' received invalid IPv6 dest address",
						     nm_connection_get_id (priv->connection));
				goto next;
			}

			route.plen = prefix;
			ip6_addr_from_variant (next_hop, &route.gateway);
			route.metric = route_metric;
			route.source = NM_IP_CONFIG_SOURCE_VPN;

			/* Ignore host routes to the VPN gateway since NM adds one itself.
			 * Since NM knows more about the routing situation than the VPN
			 * server, we want to use the NM created route instead of whatever
			 * the server provides.
			 */
			if (!(priv->ip6_external_gw && IN6_ARE_ADDR_EQUAL (&route.network, priv->ip6_external_gw) && route.plen == 128))
				nm_ip6_config_add_route (config, &route);

next:
			g_variant_unref (dest);
			g_variant_unref (next_hop);
		}
		g_variant_iter_free (iter);
	}

	if (g_variant_lookup (dict, NM_VPN_PLUGIN_IP6_CONFIG_NEVER_DEFAULT, "b", &b))
		nm_ip6_config_set_never_default (config, b);

	/* Merge in user overrides from the NMConnection's IPv6 setting */
	nm_ip6_config_merge_setting (config,
	                             nm_connection_get_setting_ip6_config (priv->connection),
	                             route_metric);

	g_clear_object (&priv->ip6_config);
	priv->ip6_config = config;
	nm_ip6_config_export (config);
	g_object_notify (G_OBJECT (self), NM_ACTIVE_CONNECTION_IP6_CONFIG);
	nm_vpn_connection_config_maybe_complete (self, TRUE);
}

static gboolean
connect_timeout_cb (gpointer user_data)
{
	NMVpnConnection *connection = NM_VPN_CONNECTION (user_data);
	NMVpnConnectionPrivate *priv = NM_VPN_CONNECTION_GET_PRIVATE (connection);

	priv->connect_timeout = 0;

	/* Cancel activation if it's taken too long */
	if (priv->vpn_state == STATE_CONNECT ||
	    priv->vpn_state == STATE_IP_CONFIG_GET) {
		nm_log_warn (LOGD_VPN, "VPN connection '%s' connect timeout exceeded.",
		             nm_connection_get_id (priv->connection));
		_set_vpn_state (connection, STATE_FAILED, NM_VPN_CONNECTION_STATE_REASON_CONNECT_TIMEOUT, FALSE);
	}

	return FALSE;
}

static void
connect_success (NMVpnConnection *connection)
{
	NMVpnConnectionPrivate *priv = NM_VPN_CONNECTION_GET_PRIVATE (connection);

	/* 40 second timeout waiting for IP config signal from VPN service */
	priv->connect_timeout = g_timeout_add_seconds (40, connect_timeout_cb, connection);

	g_clear_pointer (&priv->connect_hash, g_variant_unref);
}

static void
connect_cb (GDBusProxy *proxy, GAsyncResult *result, gpointer user_data)
{
	NMVpnConnection *self;
	gs_unref_variant GVariant *reply = NULL;
	gs_free_error GError *error = NULL;

	reply = g_dbus_proxy_call_finish (proxy, result, &error);
	if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
		return;

	self = NM_VPN_CONNECTION (user_data);

	if (error) {
		nm_log_warn (LOGD_VPN, "VPN connection '%s' failed to connect: '%s'.",
		             nm_connection_get_id (NM_VPN_CONNECTION_GET_PRIVATE (self)->connection),
		             error->message);
		_set_vpn_state (self, STATE_FAILED, NM_VPN_CONNECTION_STATE_REASON_SERVICE_START_FAILED, FALSE);
	} else
		connect_success (self);
}

static void
connect_interactive_cb (GDBusProxy *proxy, GAsyncResult *result, gpointer user_data)
{
	NMVpnConnection *self;
	NMVpnConnectionPrivate *priv;
	gs_unref_variant GVariant *reply = NULL;
	gs_free_error GError *error = NULL;

	reply = g_dbus_proxy_call_finish (proxy, result, &error);
	if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
		return;

	self = NM_VPN_CONNECTION (user_data);
	priv = NM_VPN_CONNECTION_GET_PRIVATE (self);

	nm_log_info (LOGD_VPN, "VPN connection '%s' (ConnectInteractive) reply received.",
	             nm_connection_get_id (priv->connection));

	if (g_error_matches (error, NM_VPN_PLUGIN_ERROR, NM_VPN_PLUGIN_ERROR_INTERACTIVE_NOT_SUPPORTED)) {
		nm_log_dbg (LOGD_VPN, "VPN connection '%s' falling back to non-interactive connect.",
		            nm_connection_get_id (priv->connection));

		/* Fall back to Connect() */
		g_dbus_proxy_call (priv->proxy,
		                   "Connect",
		                   g_variant_new ("(@a{sa{sv}})", priv->connect_hash),
		                   G_DBUS_CALL_FLAGS_NONE,
		                   -1,
		                   priv->cancellable,
		                   (GAsyncReadyCallback) connect_cb,
		                   self);
	} else if (error) {
		nm_log_warn (LOGD_VPN, "VPN connection '%s' failed to connect interactively: '%s'.",
		             nm_connection_get_id (priv->connection), error->message);
		_set_vpn_state (self, STATE_FAILED, NM_VPN_CONNECTION_STATE_REASON_SERVICE_START_FAILED, FALSE);
	} else
		connect_success (self);
}

/* Add a username to a hashed connection */
static GVariant *
_hash_with_username (NMConnection *connection, const char *username)
{
	gs_unref_object NMConnection *dup = NULL;
	NMSettingVpn *s_vpn;

	/* Shortcut if we weren't given a username or if there already was one in
	 * the VPN setting; don't bother duplicating the connection and everything.
	 */
	s_vpn = nm_connection_get_setting_vpn (connection);
	g_assert (s_vpn);
	if (username == NULL || nm_setting_vpn_get_user_name (s_vpn))
		return nm_connection_to_dbus (connection, NM_CONNECTION_SERIALIZE_ALL);

	dup = nm_simple_connection_new_clone (connection);
	g_assert (dup);
	s_vpn = nm_connection_get_setting_vpn (dup);
	g_assert (s_vpn);
	g_object_set (s_vpn, NM_SETTING_VPN_USER_NAME, username, NULL);
	return nm_connection_to_dbus (dup, NM_CONNECTION_SERIALIZE_ALL);
}

static void
really_activate (NMVpnConnection *self, const char *username)
{
	NMVpnConnectionPrivate *priv;
	GVariantBuilder details;

	g_return_if_fail (NM_IS_VPN_CONNECTION (self));

	priv = NM_VPN_CONNECTION_GET_PRIVATE (self);
	g_return_if_fail (priv->vpn_state == STATE_NEED_AUTH);

	g_clear_pointer (&priv->connect_hash, g_variant_unref);
	priv->connect_hash = _hash_with_username (priv->connection, username);
	g_variant_ref_sink (priv->connect_hash);

	/* If at least one agent doesn't support VPN hints, then we can't use
	 * ConnectInteractive(), because that agent won't be able to pass hints
	 * from the VPN plugin's interactive secrets requests to the VPN authentication
	 * dialog and we won't get the secrets we need.  In this case fall back to
	 * the old Connect() call.
	 */
	if (nm_agent_manager_all_agents_have_capability (nm_agent_manager_get (),
	                                                 nm_active_connection_get_subject (NM_ACTIVE_CONNECTION (self)),
	                                                 NM_SECRET_AGENT_CAPABILITY_VPN_HINTS)) {
		nm_log_dbg (LOGD_VPN, "Allowing interactive secrets as all agents have that capability");

		g_variant_builder_init (&details, G_VARIANT_TYPE_VARDICT);
		g_dbus_proxy_call (priv->proxy,
		                   "ConnectInteractive",
		                   g_variant_new ("(@a{sa{sv}}a{sv})", priv->connect_hash, &details),
		                   G_DBUS_CALL_FLAGS_NONE,
		                   -1,
		                   priv->cancellable,
		                   (GAsyncReadyCallback) connect_interactive_cb,
		                   self);
	} else {
		nm_log_dbg (LOGD_VPN, "Calling old Connect function as not all agents support interactive secrets");
		g_dbus_proxy_call (priv->proxy,
		                   "Connect",
		                   g_variant_new ("(@a{sa{sv}})", priv->connect_hash),
		                   G_DBUS_CALL_FLAGS_NONE,
		                   -1,
		                   priv->cancellable,
		                   (GAsyncReadyCallback) connect_cb,
		                   self);
	}

	_set_vpn_state (self, STATE_CONNECT, NM_VPN_CONNECTION_STATE_REASON_NONE, FALSE);
}

static void
failure_cb (GDBusProxy *proxy,
            guint32     reason,
            gpointer    user_data)
{
	NMVpnConnection *self = NM_VPN_CONNECTION (user_data);

	plugin_failed (self, reason);
}

static void
state_changed_cb (GDBusProxy *proxy,
                  guint32     new_service_state,
                  gpointer    user_data)
{
	NMVpnConnection *self = NM_VPN_CONNECTION (user_data);

	plugin_state_changed (self, new_service_state);
}

static void
secrets_required_cb (GDBusProxy  *proxy,
                     const char  *message,
                     const char **secrets,
                     gpointer     user_data)
{
	NMVpnConnection *self = NM_VPN_CONNECTION (user_data);

	plugin_interactive_secrets_required (self, message, secrets);
}

static void
config_cb (GDBusProxy *proxy,
           GVariant   *dict,
           gpointer    user_data)
{
	NMVpnConnection *self = NM_VPN_CONNECTION (user_data);
	NMVpnConnectionPrivate *priv = NM_VPN_CONNECTION_GET_PRIVATE (self);

	/* Only list to this signals during and after connection */
	if (priv->vpn_state >= STATE_NEED_AUTH)
		nm_vpn_connection_config_get (self, dict);
}

static void
ip4_config_cb (GDBusProxy *proxy,
               GVariant   *dict,
               gpointer    user_data)
{
	NMVpnConnection *self = NM_VPN_CONNECTION (user_data);
	NMVpnConnectionPrivate *priv = NM_VPN_CONNECTION_GET_PRIVATE (self);

	/* Only list to this signals during and after connection */
	if (priv->vpn_state >= STATE_NEED_AUTH)
		nm_vpn_connection_ip4_config_get (self, dict);
}

static void
ip6_config_cb (GDBusProxy *proxy,
               GVariant   *dict,
               gpointer    user_data)
{
	NMVpnConnection *self = NM_VPN_CONNECTION (user_data);
	NMVpnConnectionPrivate *priv = NM_VPN_CONNECTION_GET_PRIVATE (self);

	/* Only list to this signals during and after connection */
	if (priv->vpn_state >= STATE_NEED_AUTH)
		nm_vpn_connection_ip6_config_get (self, dict);
}

static void
on_proxy_acquired (GObject *object, GAsyncResult *result, gpointer user_data)
{
	NMVpnConnection *self;
	NMVpnConnectionPrivate *priv;
	gs_free_error GError *error = NULL;
	GDBusProxy *proxy;

	proxy = g_dbus_proxy_new_for_bus_finish (result, &error);
	if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
		return;

	self = NM_VPN_CONNECTION (user_data);
	priv = NM_VPN_CONNECTION_GET_PRIVATE (self);

	if (error) {
		nm_log_err (LOGD_VPN, "(%s/%s) failed to acquire dbus proxy for VPN service: %s",
		            nm_connection_get_uuid (priv->connection),
		            nm_connection_get_id (priv->connection),
		            error->message);
		_set_vpn_state (self,
		                STATE_FAILED,
		                NM_VPN_CONNECTION_STATE_REASON_SERVICE_START_FAILED,
		                FALSE);
		return;
	}

	priv->proxy = proxy;
	_nm_dbus_signal_connect (priv->proxy, "Failure", G_VARIANT_TYPE ("(u)"),
	                         G_CALLBACK (failure_cb), self);
	_nm_dbus_signal_connect (priv->proxy, "StateChanged", G_VARIANT_TYPE ("(u)"),
	                         G_CALLBACK (state_changed_cb), self);
	_nm_dbus_signal_connect (priv->proxy, "SecretsRequired", G_VARIANT_TYPE ("(sas)"),
	                         G_CALLBACK (secrets_required_cb), self);
	_nm_dbus_signal_connect (priv->proxy, "Config", G_VARIANT_TYPE ("(a{sv})"),
	                         G_CALLBACK (config_cb), self);
	_nm_dbus_signal_connect (priv->proxy, "Ip4Config", G_VARIANT_TYPE ("(a{sv})"),
	                         G_CALLBACK (ip4_config_cb), self);
	_nm_dbus_signal_connect (priv->proxy, "Ip6Config", G_VARIANT_TYPE ("(a{sv})"),
	                         G_CALLBACK (ip6_config_cb), self);

	_set_vpn_state (self, STATE_NEED_AUTH, NM_VPN_CONNECTION_STATE_REASON_NONE, FALSE);

	/* Kick off the secrets requests; first we get existing system secrets
	 * and ask the plugin if these are sufficient, next we get all existing
	 * secrets from system and from user agents and ask the plugin again,
	 * and last we ask the user for new secrets if required.
	 */
	get_secrets (self, SECRETS_REQ_SYSTEM, NULL);
}

void
nm_vpn_connection_activate (NMVpnConnection *self)
{
	NMVpnConnectionPrivate *priv;
	NMSettingVpn *s_vpn;

	g_return_if_fail (NM_IS_VPN_CONNECTION (self));

	priv = NM_VPN_CONNECTION_GET_PRIVATE (self);

	s_vpn = nm_connection_get_setting_vpn (priv->connection);
	g_assert (s_vpn);
	priv->connection_can_persist = nm_setting_vpn_get_persistent (s_vpn);

	_set_vpn_state (self, STATE_PREPARE, NM_VPN_CONNECTION_STATE_REASON_NONE, FALSE);

	priv->cancellable = g_cancellable_new ();
	g_dbus_proxy_new_for_bus (G_BUS_TYPE_SYSTEM,
	                          G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
	                          NULL,
	                          nm_vpn_connection_get_service (self),
	                          NM_VPN_DBUS_PLUGIN_PATH,
	                          NM_VPN_DBUS_PLUGIN_INTERFACE,
	                          priv->cancellable,
	                          (GAsyncReadyCallback) on_proxy_acquired,
	                          self);
}

NMConnection *
nm_vpn_connection_get_connection (NMVpnConnection *connection)
{
	g_return_val_if_fail (NM_IS_VPN_CONNECTION (connection), NULL);

	return NM_VPN_CONNECTION_GET_PRIVATE (connection)->connection;
}

const char*
nm_vpn_connection_get_connection_id (NMVpnConnection *connection)
{
	NMConnection *c;

	g_return_val_if_fail (NM_IS_VPN_CONNECTION (connection), NULL);

	c = NM_VPN_CONNECTION_GET_PRIVATE (connection)->connection;
	return c ? nm_connection_get_id (c) : NULL;
}

NMVpnConnectionState
nm_vpn_connection_get_vpn_state (NMVpnConnection *connection)
{
	g_return_val_if_fail (NM_IS_VPN_CONNECTION (connection), NM_VPN_CONNECTION_STATE_UNKNOWN);

	return _state_to_nm_vpn_state (NM_VPN_CONNECTION_GET_PRIVATE (connection)->vpn_state);
}

const char *
nm_vpn_connection_get_banner (NMVpnConnection *connection)
{
	g_return_val_if_fail (NM_IS_VPN_CONNECTION (connection), NULL);

	return NM_VPN_CONNECTION_GET_PRIVATE (connection)->banner;
}

NMIP4Config *
nm_vpn_connection_get_ip4_config (NMVpnConnection *connection)
{
	g_return_val_if_fail (NM_IS_VPN_CONNECTION (connection), NULL);

	return NM_VPN_CONNECTION_GET_PRIVATE (connection)->ip4_config;
}

NMIP6Config *
nm_vpn_connection_get_ip6_config (NMVpnConnection *connection)
{
	g_return_val_if_fail (NM_IS_VPN_CONNECTION (connection), NULL);

	return NM_VPN_CONNECTION_GET_PRIVATE (connection)->ip6_config;
}

const char *
nm_vpn_connection_get_ip_iface (NMVpnConnection *connection)
{
	g_return_val_if_fail (NM_IS_VPN_CONNECTION (connection), NULL);

	return NM_VPN_CONNECTION_GET_PRIVATE (connection)->ip_iface;
}

int
nm_vpn_connection_get_ip_ifindex (NMVpnConnection *connection)
{
	g_return_val_if_fail (NM_IS_VPN_CONNECTION (connection), -1);

	return NM_VPN_CONNECTION_GET_PRIVATE (connection)->ip_ifindex;
}

guint32
nm_vpn_connection_get_ip4_internal_gateway (NMVpnConnection *connection)
{
	g_return_val_if_fail (NM_IS_VPN_CONNECTION (connection), 0);

	return NM_VPN_CONNECTION_GET_PRIVATE (connection)->ip4_internal_gw;
}

struct in6_addr *
nm_vpn_connection_get_ip6_internal_gateway (NMVpnConnection *connection)
{
	g_return_val_if_fail (NM_IS_VPN_CONNECTION (connection), 0);

	return NM_VPN_CONNECTION_GET_PRIVATE (connection)->ip6_internal_gw;
}

void
nm_vpn_connection_disconnect (NMVpnConnection *connection,
                              NMVpnConnectionStateReason reason,
                              gboolean quitting)
{
	g_return_if_fail (NM_IS_VPN_CONNECTION (connection));

	_set_vpn_state (connection, STATE_DISCONNECTED, reason, quitting);
}

gboolean
nm_vpn_connection_deactivate (NMVpnConnection *connection,
                              NMVpnConnectionStateReason reason,
                              gboolean quitting)
{
	NMVpnConnectionPrivate *priv;
	gboolean success = FALSE;

	g_return_val_if_fail (NM_IS_VPN_CONNECTION (connection), FALSE);

	priv = NM_VPN_CONNECTION_GET_PRIVATE (connection);
	if (priv->vpn_state > STATE_UNKNOWN && priv->vpn_state <= STATE_DEACTIVATING) {
		_set_vpn_state (connection, STATE_DEACTIVATING, reason, quitting);
		success = TRUE;
	}
	return success;
}

/******************************************************************************/

static void
plugin_need_secrets_cb (GDBusProxy *proxy, GAsyncResult *result, gpointer user_data)
{
	NMVpnConnection *self;
	NMVpnConnectionPrivate *priv;
	gs_unref_variant GVariant *reply = NULL;
	gs_free_error GError *error = NULL;
	const char *setting_name;

	reply = _nm_dbus_proxy_call_finish (proxy, result, G_VARIANT_TYPE ("(s)"), &error);
	if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
		return;

	self = NM_VPN_CONNECTION (user_data);
	priv = NM_VPN_CONNECTION_GET_PRIVATE (self);

	if (error) {
		nm_log_err (LOGD_VPN, "(%s/%s) plugin NeedSecrets request #%d failed: %s %s",
		            nm_connection_get_uuid (priv->connection),
		            nm_connection_get_id (priv->connection),
		            priv->secrets_idx + 1,
		            g_quark_to_string (error->domain),
		            error->message);
		_set_vpn_state (self, STATE_FAILED, NM_VPN_CONNECTION_STATE_REASON_NO_SECRETS, FALSE);
		return;
	}

	g_variant_get (reply, "(&s)", &setting_name);
	if (!strlen (setting_name)) {
		nm_log_dbg (LOGD_VPN, "(%s/%s) service indicated no additional secrets required",
			        nm_connection_get_uuid (priv->connection),
			        nm_connection_get_id (priv->connection));

		/* No secrets required; we can start the VPN */
		really_activate (self, priv->username);
		return;
	}

	/* More secrets required */
	if (priv->secrets_idx == SECRETS_REQ_NEW) {
		nm_log_err (LOGD_VPN, "(%s/%s) final secrets request failed to provide sufficient secrets",
		            nm_connection_get_uuid (priv->connection),
		            nm_connection_get_id (priv->connection));
		_set_vpn_state (self, STATE_FAILED, NM_VPN_CONNECTION_STATE_REASON_NO_SECRETS, FALSE);
	} else {
		nm_log_dbg (LOGD_VPN, "(%s/%s) service indicated additional secrets required",
		            nm_connection_get_uuid (priv->connection),
		            nm_connection_get_id (priv->connection));
		get_secrets (self, priv->secrets_idx + 1, NULL);
	}
}

static void
plugin_new_secrets_cb (GDBusProxy *proxy, GAsyncResult *result, gpointer user_data)
{
	NMVpnConnection *self;
	NMVpnConnectionPrivate *priv;
	gs_unref_variant GVariant *reply = NULL;
	gs_free_error GError *error = NULL;

	reply = g_dbus_proxy_call_finish (proxy, result, &error);
	if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
		return;

	self = NM_VPN_CONNECTION (user_data);
	priv = NM_VPN_CONNECTION_GET_PRIVATE (self);

	if (error) {
		nm_log_err (LOGD_VPN, "(%s/%s) sending new secrets to the plugin failed: %s %s",
		            nm_connection_get_uuid (priv->connection),
		            nm_connection_get_id (priv->connection),
		            g_quark_to_string (error->domain),
		            error->message);
		_set_vpn_state (self, STATE_FAILED, NM_VPN_CONNECTION_STATE_REASON_NO_SECRETS, FALSE);
	} else
		_set_vpn_state (self, STATE_CONNECT, NM_VPN_CONNECTION_STATE_REASON_NONE, FALSE);
}

static void
get_secrets_cb (NMSettingsConnection *connection,
                guint32 call_id,
                const char *agent_username,
                const char *setting_name,
                GError *error,
                gpointer user_data)
{
	NMVpnConnection *self = NM_VPN_CONNECTION (user_data);
	NMVpnConnectionPrivate *priv = NM_VPN_CONNECTION_GET_PRIVATE (self);
	GVariant *dict;

	g_return_if_fail (NM_CONNECTION (connection) == priv->connection);
	g_return_if_fail (call_id == priv->secrets_id);

	priv->secrets_id = 0;

	if (error && priv->secrets_idx >= SECRETS_REQ_NEW) {
		nm_log_err (LOGD_VPN, "Failed to request VPN secrets #%d: (%d) %s",
		            priv->secrets_idx + 1, error->code, error->message);
		_set_vpn_state (self, STATE_FAILED, NM_VPN_CONNECTION_STATE_REASON_NO_SECRETS, FALSE);
		return;
	}

	/* Cache the username for later */
	if (agent_username) {
		g_free (priv->username);
		priv->username = g_strdup (agent_username);
	}

	dict = _hash_with_username (priv->connection, priv->username);

	if (priv->secrets_idx == SECRETS_REQ_INTERACTIVE) {
		nm_log_dbg (LOGD_VPN, "(%s/%s) sending secrets to the plugin",
		            nm_connection_get_uuid (priv->connection),
		            nm_connection_get_id (priv->connection));

		/* Send the secrets back to the plugin */
		g_dbus_proxy_call (priv->proxy,
		                   "NewSecrets",
		                   g_variant_new ("(@a{sa{sv}})", dict),
		                   G_DBUS_CALL_FLAGS_NONE,
		                   -1,
		                   priv->cancellable,
		                   (GAsyncReadyCallback) plugin_new_secrets_cb,
		                   self);
	} else {
		nm_log_dbg (LOGD_VPN, "(%s/%s) asking service if additional secrets are required",
		            nm_connection_get_uuid (priv->connection),
		            nm_connection_get_id (priv->connection));

		/* Ask the VPN service if more secrets are required */
		g_dbus_proxy_call (priv->proxy,
		                   "NeedSecrets",
		                   g_variant_new ("(@a{sa{sv}})", dict),
		                   G_DBUS_CALL_FLAGS_NONE,
		                   -1,
		                   priv->cancellable,
		                   (GAsyncReadyCallback) plugin_need_secrets_cb,
		                   self);
	}
}

static void
get_secrets (NMVpnConnection *self,
             SecretsReq secrets_idx,
             const char **hints)
{
	NMVpnConnectionPrivate *priv = NM_VPN_CONNECTION_GET_PRIVATE (self);
	NMSecretAgentGetSecretsFlags flags = NM_SECRET_AGENT_GET_SECRETS_FLAG_NONE;
	GError *error = NULL;

	g_return_if_fail (secrets_idx < SECRETS_REQ_LAST);
	priv->secrets_idx = secrets_idx;

	nm_log_dbg (LOGD_VPN, "(%s/%s) requesting VPN secrets pass #%d",
	            nm_connection_get_uuid (priv->connection),
	            nm_connection_get_id (priv->connection),
	            priv->secrets_idx + 1);

	switch (priv->secrets_idx) {
	case SECRETS_REQ_SYSTEM:
		flags = NM_SECRET_AGENT_GET_SECRETS_FLAG_ONLY_SYSTEM;
		break;
	case SECRETS_REQ_EXISTING:
		flags = NM_SECRET_AGENT_GET_SECRETS_FLAG_NONE;
		break;
	case SECRETS_REQ_NEW:
	case SECRETS_REQ_INTERACTIVE:
		flags = NM_SECRET_AGENT_GET_SECRETS_FLAG_ALLOW_INTERACTION;
		break;
	default:
		g_assert_not_reached ();
	}

	if (nm_active_connection_get_user_requested (NM_ACTIVE_CONNECTION (self)))
		flags |= NM_SECRET_AGENT_GET_SECRETS_FLAG_USER_REQUESTED;

	priv->secrets_id = nm_settings_connection_get_secrets (NM_SETTINGS_CONNECTION (priv->connection),
	                                                       nm_active_connection_get_subject (NM_ACTIVE_CONNECTION (self)),
	                                                       NM_SETTING_VPN_SETTING_NAME,
	                                                       flags,
	                                                       hints,
	                                                       get_secrets_cb,
	                                                       self,
	                                                       &error);
	if (!priv->secrets_id) {
		if (error) {
			nm_log_err (LOGD_VPN, "failed to request VPN secrets #%d: (%d) %s",
			            priv->secrets_idx + 1, error->code, error->message);
		}
		_set_vpn_state (self, STATE_FAILED, NM_VPN_CONNECTION_STATE_REASON_NO_SECRETS, FALSE);
		g_clear_error (&error);
	}
}

static void
plugin_interactive_secrets_required (NMVpnConnection *self,
                                     const char *message,
                                     const char **secrets)
{
	NMVpnConnectionPrivate *priv = NM_VPN_CONNECTION_GET_PRIVATE (self);
	guint32 secrets_len = secrets ? g_strv_length ((char **) secrets) : 0;
	char **hints;
	guint32 i;

	nm_log_info (LOGD_VPN, "VPN plugin requested secrets; state %s (%d)",
	             vpn_state_to_string (priv->vpn_state), priv->vpn_state);

	g_return_if_fail (priv->vpn_state == STATE_CONNECT ||
	                  priv->vpn_state == STATE_NEED_AUTH);

	priv->secrets_idx = SECRETS_REQ_INTERACTIVE;
	_set_vpn_state (self, STATE_NEED_AUTH, NM_VPN_CONNECTION_STATE_REASON_NONE, FALSE);

	/* Copy hints and add message to the end */
	hints = g_malloc0 (sizeof (char *) * (secrets_len + 2));
	for (i = 0; i < secrets_len; i++)
		hints[i] = g_strdup (secrets[i]);
	if (message)
		hints[i] = g_strdup_printf ("x-vpn-message:%s", message);

	get_secrets (self, SECRETS_REQ_INTERACTIVE, (const char **) hints);
	g_strfreev (hints);
}

/******************************************************************************/

static void
device_changed (NMActiveConnection *active,
                NMDevice *new_device,
                NMDevice *old_device)
{
	NMVpnConnectionPrivate *priv = NM_VPN_CONNECTION_GET_PRIVATE (active);

	if (!_service_and_connection_can_persist (NM_VPN_CONNECTION (active)))
		return;
	if (priv->vpn_state < STATE_CONNECT || priv->vpn_state > STATE_ACTIVATED)
		return;

	/* Route-based VPNs must update their routing and send a new IP config
	 * since all their routes need to be adjusted for new_device.
	 */
	if (priv->ip_ifindex <= 0)
		return;

	/* Device changed underneath the VPN connection.  Let the plugin figure
	 * out that connectivity is down and start its reconnect attempt if it
	 * needs to.
	 */
	if (old_device) {
		nm_device_set_vpn4_config (old_device, NULL);
		nm_device_set_vpn6_config (old_device, NULL);
	}

	if (new_device)
		apply_parent_device_config (NM_VPN_CONNECTION (active));
}

/******************************************************************************/

static void
nm_vpn_connection_init (NMVpnConnection *self)
{
	NMVpnConnectionPrivate *priv = NM_VPN_CONNECTION_GET_PRIVATE (self);

	priv->vpn_state = STATE_WAITING;
	priv->secrets_idx = SECRETS_REQ_SYSTEM;
}

static void
constructed (GObject *object)
{
	NMConnection *connection;

	G_OBJECT_CLASS (nm_vpn_connection_parent_class)->constructed (object);

	connection = nm_active_connection_get_connection (NM_ACTIVE_CONNECTION (object));
	NM_VPN_CONNECTION_GET_PRIVATE (object)->connection = g_object_ref (connection);
}

static void
dispose (GObject *object)
{
	NMVpnConnectionPrivate *priv = NM_VPN_CONNECTION_GET_PRIVATE (object);

	g_clear_pointer (&priv->connect_hash, g_variant_unref);

	if (priv->connect_timeout) {
		g_source_remove (priv->connect_timeout);
		priv->connect_timeout = 0;
	}

	dispatcher_cleanup (NM_VPN_CONNECTION (object));

	if (priv->secrets_id) {
		nm_settings_connection_cancel_secrets (NM_SETTINGS_CONNECTION (priv->connection),
		                                       priv->secrets_id);
		priv->secrets_id = 0;
	}

	if (priv->cancellable) {
		g_cancellable_cancel (priv->cancellable);
		g_clear_object (&priv->cancellable);
	}
	g_clear_object (&priv->ip4_config);
	g_clear_object (&priv->ip6_config);
	g_clear_object (&priv->proxy);
	g_clear_object (&priv->connection);

	G_OBJECT_CLASS (nm_vpn_connection_parent_class)->dispose (object);
}

static void
finalize (GObject *object)
{
	NMVpnConnectionPrivate *priv = NM_VPN_CONNECTION_GET_PRIVATE (object);

	g_free (priv->banner);
	g_free (priv->ip_iface);
	g_free (priv->username);
	g_free (priv->ip6_internal_gw);
	g_free (priv->ip6_external_gw);

	G_OBJECT_CLASS (nm_vpn_connection_parent_class)->finalize (object);
}

static gboolean
ip_config_valid (VpnState state)
{
	return (state == STATE_PRE_UP || state == STATE_ACTIVATED);
}

static void
get_property (GObject *object, guint prop_id,
              GValue *value, GParamSpec *pspec)
{
	NMVpnConnectionPrivate *priv = NM_VPN_CONNECTION_GET_PRIVATE (object);
	NMDevice *parent_dev;

	switch (prop_id) {
	case PROP_VPN_STATE:
		g_value_set_uint (value, _state_to_nm_vpn_state (priv->vpn_state));
		break;
	case PROP_BANNER:
		g_value_set_string (value, priv->banner ? priv->banner : "");
		break;
	case PROP_IP4_CONFIG:
		if (ip_config_valid (priv->vpn_state) && priv->ip4_config)
			g_value_set_boxed (value, nm_ip4_config_get_dbus_path (priv->ip4_config));
		else
			g_value_set_boxed (value, "/");
		break;
	case PROP_IP6_CONFIG:
		if (ip_config_valid (priv->vpn_state) && priv->ip6_config)
			g_value_set_boxed (value, nm_ip6_config_get_dbus_path (priv->ip6_config));
		else
			g_value_set_boxed (value, "/");
		break;
	case PROP_MASTER:
		parent_dev = nm_active_connection_get_device (NM_ACTIVE_CONNECTION (object));
		g_value_set_boxed (value, parent_dev ? nm_device_get_path (parent_dev) : "/");
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
nm_vpn_connection_class_init (NMVpnConnectionClass *connection_class)
{
	GObjectClass *object_class = G_OBJECT_CLASS (connection_class);
	NMActiveConnectionClass *active_class = NM_ACTIVE_CONNECTION_CLASS (connection_class);

	g_type_class_add_private (connection_class, sizeof (NMVpnConnectionPrivate));

	/* virtual methods */
	object_class->get_property = get_property;
	object_class->constructed = constructed;
	object_class->dispose = dispose;
	object_class->finalize = finalize;
	active_class->device_state_changed = device_state_changed;
	active_class->device_changed = device_changed;

	g_object_class_override_property (object_class, PROP_MASTER, NM_ACTIVE_CONNECTION_MASTER);

	/* properties */
	g_object_class_install_property
		(object_class, PROP_VPN_STATE,
		 g_param_spec_uint (NM_VPN_CONNECTION_VPN_STATE, "", "",
		                    NM_VPN_CONNECTION_STATE_UNKNOWN,
		                    NM_VPN_CONNECTION_STATE_DISCONNECTED,
		                    NM_VPN_CONNECTION_STATE_UNKNOWN,
		                    G_PARAM_READABLE |
		                    G_PARAM_STATIC_STRINGS));

	g_object_class_install_property
		(object_class, PROP_BANNER,
		 g_param_spec_string (NM_VPN_CONNECTION_BANNER, "", "",
		                      NULL,
		                      G_PARAM_READABLE |
		                      G_PARAM_STATIC_STRINGS));

	g_object_class_override_property (object_class, PROP_IP4_CONFIG,
	                                  NM_ACTIVE_CONNECTION_IP4_CONFIG);
	g_object_class_override_property (object_class, PROP_IP6_CONFIG,
	                                  NM_ACTIVE_CONNECTION_IP6_CONFIG);

	/* signals */
	signals[VPN_STATE_CHANGED] =
		g_signal_new ("vpn-state-changed",
		              G_OBJECT_CLASS_TYPE (object_class),
		              G_SIGNAL_RUN_FIRST,
		              0, NULL, NULL, NULL,
		              G_TYPE_NONE, 2, G_TYPE_UINT, G_TYPE_UINT);

	signals[INTERNAL_STATE_CHANGED] =
		g_signal_new (NM_VPN_CONNECTION_INTERNAL_STATE_CHANGED,
		              G_OBJECT_CLASS_TYPE (object_class),
		              G_SIGNAL_RUN_FIRST,
		              0, NULL, NULL, NULL,
		              G_TYPE_NONE, 3, G_TYPE_UINT, G_TYPE_UINT, G_TYPE_UINT);

	signals[INTERNAL_RETRY_AFTER_FAILURE] =
		g_signal_new (NM_VPN_CONNECTION_INTERNAL_RETRY_AFTER_FAILURE,
		              G_OBJECT_CLASS_TYPE (object_class),
		              G_SIGNAL_RUN_FIRST,
		              0, NULL, NULL, NULL,
		              G_TYPE_NONE, 0);

	nm_dbus_manager_register_exported_type (nm_dbus_manager_get (),
	                                        G_TYPE_FROM_CLASS (object_class),
	                                        &dbus_glib_nm_vpn_connection_object_info);
}

