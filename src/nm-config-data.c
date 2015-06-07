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
 * Copyright (C) 2011 Red Hat, Inc.
 * Copyright (C) 2013 Thomas Bechtold <thomasbechtold@jpberlin.de>
 */

#include "nm-config-data.h"

#include <string.h>

#include "nm-config.h"
#include "gsystem-local-alloc.h"
#include "nm-device.h"
#include "nm-core-internal.h"
#include "nm-keyfile-internal.h"
#include "nm-macros-internal.h"

typedef struct {
	char *group_name;
	gboolean stop_match;
	struct {
		/* have a separate boolean field @has, because a @spec with
		 * value %NULL does not necessarily mean, that the property
		 * "match-device" was unspecified. */
		gboolean has;
		GSList *spec;
	} match_device;
} ConnectionInfo;

typedef struct {
	char *config_main_file;
	char *config_description;

	GKeyFile *keyfile;
	GKeyFile *keyfile_user;
	GKeyFile *keyfile_intern;

	/* A zero-terminated list of pre-processed information from the
	 * [connection] sections. This is to speed up lookup. */
	ConnectionInfo *connection_infos;

	struct {
		char *uri;
		char *response;
		guint interval;
	} connectivity;

	struct {
		char **arr;
		GSList *specs;
		GSList *specs_config;
	} no_auto_default;

	GSList *ignore_carrier;
	GSList *assume_ipv6ll_only;

	char *dns_mode;
	char *rc_manager;
} NMConfigDataPrivate;


enum {
	PROP_0,
	PROP_CONFIG_MAIN_FILE,
	PROP_CONFIG_DESCRIPTION,
	PROP_KEYFILE_USER,
	PROP_KEYFILE_INTERN,
	PROP_CONNECTIVITY_URI,
	PROP_CONNECTIVITY_INTERVAL,
	PROP_CONNECTIVITY_RESPONSE,
	PROP_NO_AUTO_DEFAULT,

	LAST_PROP
};

G_DEFINE_TYPE (NMConfigData, nm_config_data, G_TYPE_OBJECT)

#define NM_CONFIG_DATA_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), NM_TYPE_CONFIG_DATA, NMConfigDataPrivate))

/************************************************************************/

#define _HAS_PREFIX(str, prefix) \
	({ \
		const char *_str = (str); \
		g_str_has_prefix ( _str, ""prefix"") && _str[STRLEN(prefix)] != '\0'; \
	})

/************************************************************************/

const char *
nm_config_data_get_config_main_file (const NMConfigData *self)
{
	g_return_val_if_fail (self, NULL);

	return NM_CONFIG_DATA_GET_PRIVATE (self)->config_main_file;
}

const char *
nm_config_data_get_config_description (const NMConfigData *self)
{
	g_return_val_if_fail (self, NULL);

	return NM_CONFIG_DATA_GET_PRIVATE (self)->config_description;
}

char *
nm_config_data_get_value (const NMConfigData *self, const char *group, const char *key)
{
	g_return_val_if_fail (self, NULL);
	g_return_val_if_fail (group && *group, NULL);
	g_return_val_if_fail (key && *key, NULL);

	return g_key_file_get_string (NM_CONFIG_DATA_GET_PRIVATE (self)->keyfile, group, key, NULL);
}

gint
nm_config_data_get_value_boolean (const NMConfigData *self, const char *group, const char *key, gint default_value)
{
	char *str;
	gint value = default_value;

	str = nm_config_data_get_value (self, group, key);
	if (str) {
		value = nm_config_parse_boolean (str, default_value);
		g_free (str);
	}
	return value;
}

const char *
nm_config_data_get_connectivity_uri (const NMConfigData *self)
{
	g_return_val_if_fail (self, NULL);

	return NM_CONFIG_DATA_GET_PRIVATE (self)->connectivity.uri;
}

const guint
nm_config_data_get_connectivity_interval (const NMConfigData *self)
{
	g_return_val_if_fail (self, 0);

	return NM_CONFIG_DATA_GET_PRIVATE (self)->connectivity.interval;
}

const char *
nm_config_data_get_connectivity_response (const NMConfigData *self)
{
	g_return_val_if_fail (self != NULL, NULL);

	return NM_CONFIG_DATA_GET_PRIVATE (self)->connectivity.response;
}

const char *const*
nm_config_data_get_no_auto_default (const NMConfigData *self)
{
	g_return_val_if_fail (self, FALSE);

	return (const char *const*) NM_CONFIG_DATA_GET_PRIVATE (self)->no_auto_default.arr;
}

gboolean
nm_config_data_get_no_auto_default_for_device (const NMConfigData *self, NMDevice *device)
{
	NMConfigDataPrivate *priv;

	g_return_val_if_fail (NM_IS_CONFIG_DATA (self), FALSE);
	g_return_val_if_fail (NM_IS_DEVICE (device), FALSE);

	priv = NM_CONFIG_DATA_GET_PRIVATE (self);
	return    nm_device_spec_match_list (device, priv->no_auto_default.specs)
	       || nm_device_spec_match_list (device, priv->no_auto_default.specs_config);
}

const char *
nm_config_data_get_dns_mode (const NMConfigData *self)
{
	g_return_val_if_fail (self, NULL);

	return NM_CONFIG_DATA_GET_PRIVATE (self)->dns_mode;
}

const char *
nm_config_data_get_rc_manager (const NMConfigData *self)
{
	g_return_val_if_fail (self, NULL);

	return NM_CONFIG_DATA_GET_PRIVATE (self)->rc_manager;
}

gboolean
nm_config_data_get_ignore_carrier (const NMConfigData *self, NMDevice *device)
{
	g_return_val_if_fail (NM_IS_CONFIG_DATA (self), FALSE);
	g_return_val_if_fail (NM_IS_DEVICE (device), FALSE);

	return nm_device_spec_match_list (device, NM_CONFIG_DATA_GET_PRIVATE (self)->ignore_carrier);
}

gboolean
nm_config_data_get_assume_ipv6ll_only (const NMConfigData *self, NMDevice *device)
{
	g_return_val_if_fail (NM_IS_CONFIG_DATA (self), FALSE);
	g_return_val_if_fail (NM_IS_DEVICE (device), FALSE);

	return nm_device_spec_match_list (device, NM_CONFIG_DATA_GET_PRIVATE (self)->assume_ipv6ll_only);
}

GKeyFile *
nm_config_data_clone_keyfile_intern (const NMConfigData *self)
{
	NMConfigDataPrivate *priv;
	GKeyFile *keyfile;

	g_return_val_if_fail (NM_IS_CONFIG_DATA (self), FALSE);

	priv = NM_CONFIG_DATA_GET_PRIVATE (self);

	keyfile = nm_config_create_keyfile ();
	if (priv->keyfile_intern)
		_nm_keyfile_copy (keyfile, priv->keyfile_intern);
	return keyfile;
}

GKeyFile *
_nm_config_data_get_keyfile (const NMConfigData *self)
{
	return NM_CONFIG_DATA_GET_PRIVATE (self)->keyfile;
}

GKeyFile *
_nm_config_data_get_keyfile_intern (const NMConfigData *self)
{
	return NM_CONFIG_DATA_GET_PRIVATE (self)->keyfile_intern;
}

GKeyFile *
_nm_config_data_get_keyfile_user (const NMConfigData *self)
{
	return NM_CONFIG_DATA_GET_PRIVATE (self)->keyfile_user;
}

/************************************************************************/

static GKeyFile *
_merge_keyfiles (GKeyFile *keyfile_user, GKeyFile *keyfile_intern)
{
	gs_strfreev char **groups = NULL;
	guint g, k;
	GKeyFile *keyfile;

	keyfile = nm_config_create_keyfile ();
	if (keyfile_user)
		_nm_keyfile_copy (keyfile, keyfile_user);
	if (!keyfile_intern)
		return keyfile;

	groups = g_key_file_get_groups (keyfile_intern, NULL);
	for (g = 0; groups[g]; g++) {
		const char *group = groups[g];
		gs_strfreev char **keys = NULL;
		gboolean is_intern;

		keys = g_key_file_get_keys (keyfile_intern, group, NULL, NULL);
		if (!keys)
			continue;

		is_intern = g_str_has_prefix (group, NM_CONFIG_KEYFILE_GROUPPREFIX_INTERN);

		for (k = 0; keys[k]; k++) {
			const char *key = keys[k];
			gs_free char *value = NULL;

			if (!is_intern && _HAS_PREFIX (key, NM_CONFIG_KEYFILE_KEYPREFIX_WAS)) {
				const char *key_base = &key[STRLEN (NM_CONFIG_KEYFILE_KEYPREFIX_WAS)];

				if (!g_key_file_has_key (keyfile_intern, group, key_base, NULL))
					g_key_file_remove_key (keyfile, group, key_base, NULL);
				continue;
			}
			if (!is_intern && _HAS_PREFIX (key, NM_CONFIG_KEYFILE_KEYPREFIX_SET))
				continue;

			value = g_key_file_get_value (keyfile_intern, group, key, NULL);
			g_key_file_set_value (keyfile, group, key, value);
		}
	}
	return keyfile;
}

/************************************************************************/

char *
nm_config_data_get_connection_default (const NMConfigData *self,
                                       const char *property,
                                       NMDevice *device)
{
	NMConfigDataPrivate *priv;
	const ConnectionInfo *connection_info;

	g_return_val_if_fail (self, NULL);
	g_return_val_if_fail (property && *property, NULL);
	g_return_val_if_fail (strchr (property, '.'), NULL);

	priv = NM_CONFIG_DATA_GET_PRIVATE (self);

	if (!priv->connection_infos)
		return NULL;

	for (connection_info = &priv->connection_infos[0]; connection_info->group_name; connection_info++) {
		char *value;
		gboolean match;

		value = g_key_file_get_value (priv->keyfile, connection_info->group_name, property, NULL);
		if (!value && !connection_info->stop_match)
			continue;

		match = TRUE;
		if (connection_info->match_device.has)
			match = device && nm_device_spec_match_list (device, connection_info->match_device.spec);

		if (match)
			return value;
		g_free (value);
	}
	return NULL;
}

static ConnectionInfo *
_get_connection_infos (GKeyFile *keyfile)
{
	char **groups;
	guint i;
	char *connection_tag = NULL;
	GSList *connection_groups = NULL;
	ConnectionInfo *connection_infos = NULL;

	/* get the list of existing [connection.\+] sections that we consider
	 * for nm_config_data_get_connection_default(). Also, get them
	 * in the right order. */
	groups = g_key_file_get_groups (keyfile, NULL);
	for (i = 0; groups && groups[i]; i++) {
		if (g_str_has_prefix (groups[i], "connection")) {
			if (strlen (groups[i]) == STRLEN ("connection"))
				connection_tag = groups[i];
			else
				connection_groups = g_slist_prepend (connection_groups, groups[i]);
		} else
			g_free (groups[i]);
	}
	g_free (groups);
	if (connection_tag) {
		/* We want the group "connection" checked at last, so that
		 * all other "connection.\+" have preference. Those other
		 * groups are checked in order of appearance. */
		connection_groups = g_slist_prepend (connection_groups, connection_tag);
	}
	if (connection_groups) {
		guint len = g_slist_length (connection_groups);
		GSList *iter;

		connection_infos = g_new0 (ConnectionInfo, len + 1);
		for (iter = connection_groups; iter; iter = iter->next) {
			ConnectionInfo *connection_info;
			char *value;

			nm_assert (len >= 1);
			connection_info = &connection_infos[--len];
			connection_info->group_name = iter->data;

			value = g_key_file_get_value (keyfile, iter->data, "match-device", NULL);
			if (value) {
				connection_info->match_device.has = TRUE;
				connection_info->match_device.spec = nm_match_spec_split (value);
				g_free (value);
			}
			connection_info->stop_match = nm_config_keyfile_get_boolean (keyfile, iter->data, "stop-match", FALSE);
		}
		g_slist_free (connection_groups);
	}

	return connection_infos;
}

/************************************************************************/

static gboolean
_slist_str_equals (GSList *a, GSList *b)
{
	while (a && b && g_strcmp0 (a->data, b->data) == 0) {
		a = a->next;
		b = b->next;
	}
	return !a && !b;
}

NMConfigChangeFlags
nm_config_data_diff (NMConfigData *old_data, NMConfigData *new_data)
{
	NMConfigChangeFlags changes = NM_CONFIG_CHANGE_NONE;
	NMConfigDataPrivate *priv_old, *priv_new;

	g_return_val_if_fail (NM_IS_CONFIG_DATA (old_data), NM_CONFIG_CHANGE_NONE);
	g_return_val_if_fail (NM_IS_CONFIG_DATA (new_data), NM_CONFIG_CHANGE_NONE);

	priv_old = NM_CONFIG_DATA_GET_PRIVATE (old_data);
	priv_new = NM_CONFIG_DATA_GET_PRIVATE (new_data);

	if (!_nm_keyfile_equals (priv_old->keyfile_user, priv_new->keyfile_user, TRUE))
		changes |= NM_CONFIG_CHANGE_VALUES | NM_CONFIG_CHANGE_VALUES_USER;

	if (!_nm_keyfile_equals (priv_old->keyfile_intern, priv_new->keyfile_intern, TRUE))
		changes |= NM_CONFIG_CHANGE_VALUES | NM_CONFIG_CHANGE_VALUES_INTERN;

	if (   g_strcmp0 (nm_config_data_get_config_main_file (old_data), nm_config_data_get_config_main_file (new_data)) != 0
	    || g_strcmp0 (nm_config_data_get_config_description (old_data), nm_config_data_get_config_description (new_data)) != 0)
		changes |= NM_CONFIG_CHANGE_CONFIG_FILES;

	if (   nm_config_data_get_connectivity_interval (old_data) != nm_config_data_get_connectivity_interval (new_data)
	    || g_strcmp0 (nm_config_data_get_connectivity_uri (old_data), nm_config_data_get_connectivity_uri (new_data))
	    || g_strcmp0 (nm_config_data_get_connectivity_response (old_data), nm_config_data_get_connectivity_response (new_data)))
		changes |= NM_CONFIG_CHANGE_CONNECTIVITY;

	if (   !_slist_str_equals (priv_old->no_auto_default.specs, priv_new->no_auto_default.specs)
	    || !_slist_str_equals (priv_old->no_auto_default.specs_config, priv_new->no_auto_default.specs_config))
		changes |= NM_CONFIG_CHANGE_NO_AUTO_DEFAULT;

	if (g_strcmp0 (nm_config_data_get_dns_mode (old_data), nm_config_data_get_dns_mode (new_data)))
		changes |= NM_CONFIG_CHANGE_DNS_MODE;

	if (g_strcmp0 (nm_config_data_get_rc_manager (old_data), nm_config_data_get_rc_manager (new_data)))
		changes |= NM_CONFIG_CHANGE_RC_MANAGER;

	return changes;
}

/************************************************************************/

static void
get_property (GObject *object,
              guint prop_id,
              GValue *value,
              GParamSpec *pspec)
{
	NMConfigData *self = NM_CONFIG_DATA (object);

	switch (prop_id) {
	case PROP_CONFIG_MAIN_FILE:
		g_value_set_string (value, nm_config_data_get_config_main_file (self));
		break;
	case PROP_CONFIG_DESCRIPTION:
		g_value_set_string (value, nm_config_data_get_config_description (self));
		break;
	case PROP_CONNECTIVITY_URI:
		g_value_set_string (value, nm_config_data_get_connectivity_uri (self));
		break;
	case PROP_CONNECTIVITY_INTERVAL:
		g_value_set_uint (value, nm_config_data_get_connectivity_interval (self));
		break;
	case PROP_CONNECTIVITY_RESPONSE:
		g_value_set_string (value, nm_config_data_get_connectivity_response (self));
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
set_property (GObject *object,
              guint prop_id,
              const GValue *value,
              GParamSpec *pspec)
{
	NMConfigData *self = NM_CONFIG_DATA (object);
	NMConfigDataPrivate *priv = NM_CONFIG_DATA_GET_PRIVATE (self);

	/* This type is immutable. All properties are construct only. */
	switch (prop_id) {
	case PROP_CONFIG_MAIN_FILE:
		priv->config_main_file = g_value_dup_string (value);
		break;
	case PROP_CONFIG_DESCRIPTION:
		priv->config_description = g_value_dup_string (value);
		break;
	case PROP_KEYFILE_USER:
		priv->keyfile_user = g_value_dup_boxed (value);
		if (   priv->keyfile_user
		    && !_nm_keyfile_has_values (priv->keyfile_user)) {
			g_key_file_unref (priv->keyfile_user);
			priv->keyfile_user = NULL;
		}
		break;
	case PROP_KEYFILE_INTERN:
		priv->keyfile_intern = g_value_dup_boxed (value);
		if (   priv->keyfile_intern
		    && !_nm_keyfile_has_values (priv->keyfile_intern)) {
			g_key_file_unref (priv->keyfile_intern);
			priv->keyfile_intern = NULL;
		}
		break;
	case PROP_NO_AUTO_DEFAULT:
		{
			char ** value_arr = g_value_get_boxed (value);
			guint i, j = 0;

			priv->no_auto_default.arr = g_new (char *, g_strv_length (value_arr) + 1);
			priv->no_auto_default.specs = NULL;

			for (i = 0; value_arr && value_arr[i]; i++) {
				if (   *value_arr[i]
				    && nm_utils_hwaddr_valid (value_arr[i], -1)
				    && _nm_utils_strv_find_first (value_arr, i, value_arr[i]) < 0) {
					priv->no_auto_default.arr[j++] = g_strdup (value_arr[i]);
					priv->no_auto_default.specs = g_slist_prepend (priv->no_auto_default.specs, g_strdup_printf ("mac:%s", value_arr[i]));
				}
			}
			priv->no_auto_default.arr[j++] = NULL;
			priv->no_auto_default.specs = g_slist_reverse (priv->no_auto_default.specs);
		}
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
dispose (GObject *object)
{
}

static void
finalize (GObject *gobject)
{
	NMConfigDataPrivate *priv = NM_CONFIG_DATA_GET_PRIVATE (gobject);
	guint i;

	g_free (priv->config_main_file);
	g_free (priv->config_description);

	g_free (priv->connectivity.uri);
	g_free (priv->connectivity.response);

	g_slist_free_full (priv->no_auto_default.specs, g_free);
	g_slist_free_full (priv->no_auto_default.specs_config, g_free);
	g_strfreev (priv->no_auto_default.arr);

	g_free (priv->dns_mode);
	g_free (priv->rc_manager);

	g_slist_free_full (priv->ignore_carrier, g_free);
	g_slist_free_full (priv->assume_ipv6ll_only, g_free);

	if (priv->connection_infos) {
		for (i = 0; priv->connection_infos[i].group_name; i++) {
			g_free (priv->connection_infos[i].group_name);
			g_slist_free_full (priv->connection_infos[i].match_device.spec, g_free);
		}
		g_free (priv->connection_infos);
	}

	g_key_file_unref (priv->keyfile);
	if (priv->keyfile_user)
		g_key_file_unref (priv->keyfile_user);
	if (priv->keyfile_intern)
		g_key_file_unref (priv->keyfile_intern);

	G_OBJECT_CLASS (nm_config_data_parent_class)->finalize (gobject);
}

static void
nm_config_data_init (NMConfigData *self)
{
}

static void
constructed (GObject *object)
{
	NMConfigData *self = NM_CONFIG_DATA (object);
	NMConfigDataPrivate *priv = NM_CONFIG_DATA_GET_PRIVATE (self);
	char *interval;

	priv->keyfile = _merge_keyfiles (priv->keyfile_user, priv->keyfile_intern);

	priv->connection_infos = _get_connection_infos (priv->keyfile);

	priv->connectivity.uri = g_key_file_get_value (priv->keyfile, "connectivity", "uri", NULL);
	priv->connectivity.response = g_key_file_get_value (priv->keyfile, "connectivity", "response", NULL);

	/* On missing config value, fallback to 300. On invalid value, disable connectivity checking by setting
	 * the interval to zero. */
	interval = g_key_file_get_value (priv->keyfile, "connectivity", "interval", NULL);
	priv->connectivity.interval = interval
	    ? _nm_utils_ascii_str_to_int64 (interval, 10, 0, G_MAXUINT, 0)
	    : NM_CONFIG_DEFAULT_CONNECTIVITY_INTERVAL;
	g_free (interval);

	priv->dns_mode = g_key_file_get_value (priv->keyfile, "main", "dns", NULL);
	priv->rc_manager = g_key_file_get_value (priv->keyfile, "main", "rc-manager", NULL);

	priv->ignore_carrier = nm_config_get_device_match_spec (priv->keyfile, "main", "ignore-carrier");
	priv->assume_ipv6ll_only = nm_config_get_device_match_spec (priv->keyfile, "main", "assume-ipv6ll-only");

	priv->no_auto_default.specs_config = nm_config_get_device_match_spec (priv->keyfile, "main", "no-auto-default");

	G_OBJECT_CLASS (nm_config_data_parent_class)->constructed (object);
}

NMConfigData *
nm_config_data_new (const char *config_main_file,
                    const char *config_description,
                    const char *const*no_auto_default,
                    GKeyFile *keyfile_user,
                    GKeyFile *keyfile_intern)
{
	return g_object_new (NM_TYPE_CONFIG_DATA,
	                     NM_CONFIG_DATA_CONFIG_MAIN_FILE, config_main_file,
	                     NM_CONFIG_DATA_CONFIG_DESCRIPTION, config_description,
	                     NM_CONFIG_DATA_KEYFILE_USER, keyfile_user,
	                     NM_CONFIG_DATA_KEYFILE_INTERN, keyfile_intern,
	                     NM_CONFIG_DATA_NO_AUTO_DEFAULT, no_auto_default,
	                     NULL);
}

NMConfigData *
nm_config_data_new_update_keyfile_intern (const NMConfigData *base, GKeyFile *keyfile_intern)
{
	NMConfigDataPrivate *priv = NM_CONFIG_DATA_GET_PRIVATE (base);

	return g_object_new (NM_TYPE_CONFIG_DATA,
	                     NM_CONFIG_DATA_CONFIG_MAIN_FILE, priv->config_main_file,
	                     NM_CONFIG_DATA_CONFIG_DESCRIPTION, priv->config_description,
	                     NM_CONFIG_DATA_KEYFILE_USER, priv->keyfile_user, /* the keyfile is unchanged. It's safe to share it. */
	                     NM_CONFIG_DATA_KEYFILE_INTERN, keyfile_intern,
	                     NM_CONFIG_DATA_NO_AUTO_DEFAULT, priv->no_auto_default.arr,
	                     NULL);
}

NMConfigData *
nm_config_data_new_update_no_auto_default (const NMConfigData *base,
                                           const char *const*no_auto_default)
{
	NMConfigDataPrivate *priv = NM_CONFIG_DATA_GET_PRIVATE (base);

	return g_object_new (NM_TYPE_CONFIG_DATA,
	                     NM_CONFIG_DATA_CONFIG_MAIN_FILE, priv->config_main_file,
	                     NM_CONFIG_DATA_CONFIG_DESCRIPTION, priv->config_description,
	                     NM_CONFIG_DATA_KEYFILE_USER, priv->keyfile_user, /* the keyfile is unchanged. It's safe to share it. */
	                     NM_CONFIG_DATA_KEYFILE_INTERN, priv->keyfile_intern,
	                     NM_CONFIG_DATA_NO_AUTO_DEFAULT, no_auto_default,
	                     NULL);
}

static void
nm_config_data_class_init (NMConfigDataClass *config_class)
{
	GObjectClass *object_class = G_OBJECT_CLASS (config_class);

	g_type_class_add_private (config_class, sizeof (NMConfigDataPrivate));

	object_class->constructed = constructed;
	object_class->dispose = dispose;
	object_class->finalize = finalize;
	object_class->get_property = get_property;
	object_class->set_property = set_property;

	g_object_class_install_property
	    (object_class, PROP_CONFIG_MAIN_FILE,
	     g_param_spec_string (NM_CONFIG_DATA_CONFIG_MAIN_FILE, "", "",
	                          NULL,
	                          G_PARAM_READWRITE |
	                          G_PARAM_CONSTRUCT_ONLY |
	                          G_PARAM_STATIC_STRINGS));

	g_object_class_install_property
	    (object_class, PROP_CONFIG_DESCRIPTION,
	     g_param_spec_string (NM_CONFIG_DATA_CONFIG_DESCRIPTION, "", "",
	                          NULL,
	                          G_PARAM_READWRITE |
	                          G_PARAM_CONSTRUCT_ONLY |
	                          G_PARAM_STATIC_STRINGS));

	g_object_class_install_property
	      (object_class, PROP_KEYFILE_USER,
	       g_param_spec_boxed (NM_CONFIG_DATA_KEYFILE_USER, "", "",
	                           G_TYPE_KEY_FILE,
	                           G_PARAM_WRITABLE |
	                           G_PARAM_CONSTRUCT_ONLY |
	                           G_PARAM_STATIC_STRINGS));

	g_object_class_install_property
	      (object_class, PROP_KEYFILE_INTERN,
	       g_param_spec_boxed (NM_CONFIG_DATA_KEYFILE_INTERN, "", "",
	                           G_TYPE_KEY_FILE,
	                           G_PARAM_WRITABLE |
	                           G_PARAM_CONSTRUCT_ONLY |
	                           G_PARAM_STATIC_STRINGS));

	g_object_class_install_property
	    (object_class, PROP_CONNECTIVITY_URI,
	     g_param_spec_string (NM_CONFIG_DATA_CONNECTIVITY_URI, "", "",
	                          NULL,
	                          G_PARAM_READABLE |
	                          G_PARAM_STATIC_STRINGS));

	g_object_class_install_property
	    (object_class, PROP_CONNECTIVITY_INTERVAL,
	     g_param_spec_uint (NM_CONFIG_DATA_CONNECTIVITY_INTERVAL, "", "",
	                        0, G_MAXUINT, 0,
	                        G_PARAM_READABLE |
	                        G_PARAM_STATIC_STRINGS));

	g_object_class_install_property
	    (object_class, PROP_CONNECTIVITY_RESPONSE,
	     g_param_spec_string (NM_CONFIG_DATA_CONNECTIVITY_RESPONSE, "", "",
	                          NULL,
	                          G_PARAM_READABLE |
	                          G_PARAM_STATIC_STRINGS));

	g_object_class_install_property
	    (object_class, PROP_NO_AUTO_DEFAULT,
	     g_param_spec_boxed (NM_CONFIG_DATA_NO_AUTO_DEFAULT, "", "",
	                         G_TYPE_STRV,
	                         G_PARAM_WRITABLE |
	                         G_PARAM_CONSTRUCT_ONLY |
	                         G_PARAM_STATIC_STRINGS));

}

