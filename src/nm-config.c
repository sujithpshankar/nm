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

#include "config.h"

#include <string.h>
#include <stdio.h>

#include "nm-config.h"
#include "nm-logging.h"
#include "nm-utils.h"
#include "nm-glib-compat.h"
#include "nm-device.h"
#include "NetworkManagerUtils.h"
#include "gsystem-local-alloc.h"
#include "nm-enum-types.h"
#include "nm-core-internal.h"

#include <gio/gio.h>
#include <glib/gi18n.h>

#define NM_DEFAULT_SYSTEM_CONF_FILE    NMCONFDIR "/NetworkManager.conf"
#define NM_DEFAULT_SYSTEM_CONF_DIR     NMCONFDIR "/conf.d"
#define NM_OLD_SYSTEM_CONF_FILE        NMCONFDIR "/nm-system-settings.conf"
#define NM_NO_AUTO_DEFAULT_STATE_FILE  NMSTATEDIR "/no-auto-default.state"

struct NMConfigCmdLineOptions {
	char *config_main_file;
	char *config_dir;
	char *no_auto_default_file;
	char *plugins;
	gboolean configure_and_quit;
	char *connectivity_uri;

	/* We store interval as signed internally to track whether it's
	 * set or not via GOptionEntry
	 */
	int connectivity_interval;
	char *connectivity_response;
};

typedef struct {
	NMConfigCmdLineOptions cli;

	NMConfigData *config_data;
	NMConfigData *config_data_orig;

	char *config_dir;
	char *no_auto_default_file;

	char **plugins;
	gboolean monitor_connection_files;
	gboolean auth_polkit;
	char *dhcp_client;

	char *log_level;
	char *log_domains;

	char *debug;

	gboolean configure_and_quit;
} NMConfigPrivate;

enum {
	PROP_0,
	PROP_CMD_LINE_OPTIONS,
	LAST_PROP,
};

enum {
	SIGNAL_CONFIG_CHANGED,

	LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

static void nm_config_initable_iface_init (GInitableIface *iface);

G_DEFINE_TYPE_WITH_CODE (NMConfig, nm_config, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE, nm_config_initable_iface_init);
                         )


#define NM_CONFIG_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), NM_TYPE_CONFIG, NMConfigPrivate))

/************************************************************************/

static void _set_config_data (NMConfig *self, NMConfigData *new_data);

/************************************************************************/

gboolean
nm_config_keyfile_get_boolean (GKeyFile *keyfile,
                               const char *section,
                               const char *key,
                               gboolean default_value)
{
	gboolean value = default_value;
	char *str;

	g_return_val_if_fail (keyfile != NULL, default_value);
	g_return_val_if_fail (section != NULL, default_value);
	g_return_val_if_fail (key != NULL, default_value);

	str = g_key_file_get_value (keyfile, section, key, NULL);
	if (!str)
		return default_value;

	g_strstrip (str);
	if (str[0]) {
		if (!g_ascii_strcasecmp (str, "true") || !g_ascii_strcasecmp (str, "yes") || !g_ascii_strcasecmp (str, "on") || !g_ascii_strcasecmp (str, "1"))
			value = TRUE;
		else if (!g_ascii_strcasecmp (str, "false") || !g_ascii_strcasecmp (str, "no") || !g_ascii_strcasecmp (str, "off") || !g_ascii_strcasecmp (str, "0"))
			value = FALSE;
		else {
			nm_log_warn (LOGD_CORE, "Unrecognized value for %s.%s: '%s'. Assuming '%s'",
			             section, key, str, default_value ? "true" : "false");
		}
	}

	g_free (str);
	return value;
}

/************************************************************************/

NMConfigData *
nm_config_get_data (NMConfig *config)
{
	g_return_val_if_fail (config != NULL, NULL);

	return NM_CONFIG_GET_PRIVATE (config)->config_data;
}

/* The NMConfigData instance is reloadable and will be swapped on reload.
 * nm_config_get_data_orig() returns the original configuration, when the NMConfig
 * instance was created. */
NMConfigData *
nm_config_get_data_orig (NMConfig *config)
{
	g_return_val_if_fail (config != NULL, NULL);

	return NM_CONFIG_GET_PRIVATE (config)->config_data_orig;
}

const char **
nm_config_get_plugins (NMConfig *config)
{
	g_return_val_if_fail (config != NULL, NULL);

	return (const char **) NM_CONFIG_GET_PRIVATE (config)->plugins;
}

gboolean
nm_config_get_monitor_connection_files (NMConfig *config)
{
	g_return_val_if_fail (config != NULL, FALSE);

	return NM_CONFIG_GET_PRIVATE (config)->monitor_connection_files;
}

gboolean
nm_config_get_auth_polkit (NMConfig *config)
{
	g_return_val_if_fail (NM_IS_CONFIG (config), NM_CONFIG_DEFAULT_AUTH_POLKIT);

	return NM_CONFIG_GET_PRIVATE (config)->auth_polkit;
}

const char *
nm_config_get_dhcp_client (NMConfig *config)
{
	g_return_val_if_fail (config != NULL, NULL);

	return NM_CONFIG_GET_PRIVATE (config)->dhcp_client;
}

const char *
nm_config_get_log_level (NMConfig *config)
{
	g_return_val_if_fail (config != NULL, NULL);

	return NM_CONFIG_GET_PRIVATE (config)->log_level;
}

const char *
nm_config_get_log_domains (NMConfig *config)
{
	g_return_val_if_fail (config != NULL, NULL);

	return NM_CONFIG_GET_PRIVATE (config)->log_domains;
}

const char *
nm_config_get_debug (NMConfig *config)
{
	g_return_val_if_fail (config != NULL, NULL);

	return NM_CONFIG_GET_PRIVATE (config)->debug;
}

gboolean
nm_config_get_configure_and_quit (NMConfig *config)
{
	return NM_CONFIG_GET_PRIVATE (config)->configure_and_quit;
}

/************************************************************************/

static char **
no_auto_default_merge_from_file (const char *no_auto_default_file, const char *const* no_auto_default)
{
	GPtrArray *updated;
	char **list;
	int i, j;
	char *data;

	updated = g_ptr_array_new ();
	if (no_auto_default) {
		for (i = 0; no_auto_default[i]; i++)
			g_ptr_array_add (updated, g_strdup (no_auto_default[i]));
	}

	if (   no_auto_default_file
	    && g_file_get_contents (no_auto_default_file, &data, NULL, NULL)) {
		list = g_strsplit (data, "\n", -1);
		for (i = 0; list[i]; i++) {
			if (!*list[i])
				g_free (list[i]);
			else {
				for (j = 0; j < updated->len; j++) {
					if (!strcmp (list[i], updated->pdata[j]))
						break;
				}
				if (j == updated->len)
					g_ptr_array_add (updated, list[i]);
				else
					g_free (list[i]);
			}
		}
		g_free (list);
		g_free (data);
	}

	g_ptr_array_add (updated, NULL);
	return (char **) g_ptr_array_free (updated, FALSE);
}

gboolean
nm_config_get_no_auto_default_for_device (NMConfig *self, NMDevice *device)
{
	NMConfigData *config_data;

	g_return_val_if_fail (NM_IS_CONFIG (self), FALSE);
	g_return_val_if_fail (NM_IS_DEVICE (device), FALSE);

	config_data = NM_CONFIG_GET_PRIVATE (self)->config_data;
	return nm_device_spec_match_list (device, nm_config_data_get_no_auto_default_list (config_data));
}

void
nm_config_set_no_auto_default_for_device (NMConfig *self, NMDevice *device)
{
	NMConfigPrivate *priv = NM_CONFIG_GET_PRIVATE (self);
	char *current;
	GString *updated;
	GError *error = NULL;
	char **no_auto_default;
	NMConfigData *new_data = NULL;

	g_return_if_fail (NM_IS_CONFIG (self));
	g_return_if_fail (NM_IS_DEVICE (device));

	if (nm_config_get_no_auto_default_for_device (self, device))
		return;

	updated = g_string_new (NULL);
	if (g_file_get_contents (priv->no_auto_default_file, &current, NULL, NULL)) {
		g_string_append (updated, current);
		g_free (current);
		if (updated->str[updated->len - 1] != '\n')
			g_string_append_c (updated, '\n');
	}

	g_string_append (updated, nm_device_get_hw_address (device));
	g_string_append_c (updated, '\n');

	if (!g_file_set_contents (priv->no_auto_default_file, updated->str, updated->len, &error)) {
		nm_log_warn (LOGD_SETTINGS, "Could not update no-auto-default.state file: %s",
		             error->message);
		g_error_free (error);
	}

	g_string_free (updated, TRUE);

	no_auto_default = no_auto_default_merge_from_file (priv->no_auto_default_file, nm_config_data_get_no_auto_default (priv->config_data));
	new_data = nm_config_data_new_update_no_auto_default (priv->config_data, (const char *const*) no_auto_default);
	g_strfreev (no_auto_default);

	_set_config_data (self, new_data);
}

/************************************************************************/

static void
_nm_config_cmd_line_options_clear (NMConfigCmdLineOptions *cli)
{
	g_clear_pointer (&cli->config_main_file, g_free);
	g_clear_pointer (&cli->config_dir, g_free);
	g_clear_pointer (&cli->no_auto_default_file, g_free);
	g_clear_pointer (&cli->plugins, g_free);
	cli->configure_and_quit = FALSE;
	g_clear_pointer (&cli->connectivity_uri, g_free);
	g_clear_pointer (&cli->connectivity_response, g_free);
	cli->connectivity_interval = -1;
}

static void
_nm_config_cmd_line_options_copy (const NMConfigCmdLineOptions *cli, NMConfigCmdLineOptions *dst)
{
	g_return_if_fail (cli);
	g_return_if_fail (dst);
	g_return_if_fail (cli != dst);

	_nm_config_cmd_line_options_clear (dst);
	dst->config_dir = g_strdup (cli->config_dir);
	dst->config_main_file = g_strdup (cli->config_main_file);
	dst->no_auto_default_file = g_strdup (cli->no_auto_default_file);
	dst->plugins = g_strdup (cli->plugins);
	dst->configure_and_quit = cli->configure_and_quit;
	dst->connectivity_uri = g_strdup (cli->connectivity_uri);
	dst->connectivity_response = g_strdup (cli->connectivity_response);
	dst->connectivity_interval = cli->connectivity_interval;
}

NMConfigCmdLineOptions *
nm_config_cmd_line_options_new ()
{
	NMConfigCmdLineOptions *cli = g_new0 (NMConfigCmdLineOptions, 1);

	_nm_config_cmd_line_options_clear (cli);
	return cli;
}

void
nm_config_cmd_line_options_free (NMConfigCmdLineOptions *cli)
{
	g_return_if_fail (cli);

	_nm_config_cmd_line_options_clear (cli);
	g_free (cli);
}

void
nm_config_cmd_line_options_add_to_entries (NMConfigCmdLineOptions *cli,
                                           GOptionContext *opt_ctx)
{
	g_return_if_fail (opt_ctx);
	g_return_if_fail (cli);

	{
		GOptionEntry config_options[] = {
			{ "config", 0, 0, G_OPTION_ARG_FILENAME, &cli->config_main_file, N_("Config file location"), N_("/path/to/config.file") },
			{ "config-dir", 0, 0, G_OPTION_ARG_FILENAME, &cli->config_dir, N_("Config directory location"), N_("/path/to/config/dir") },
			{ "no-auto-default", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_FILENAME, &cli->no_auto_default_file, "no-auto-default.state location", NULL },
			{ "plugins", 0, 0, G_OPTION_ARG_STRING, &cli->plugins, N_("List of plugins separated by ','"), N_("plugin1,plugin2") },
			{ "configure-and-quit", 0, 0, G_OPTION_ARG_NONE, &cli->configure_and_quit, N_("Quit after initial configuration"), NULL },

				/* These three are hidden for now, and should eventually just go away. */
			{ "connectivity-uri", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_STRING, &cli->connectivity_uri, N_("An http(s) address for checking internet connectivity"), "http://example.com" },
			{ "connectivity-interval", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_INT, &cli->connectivity_interval, N_("The interval between connectivity checks (in seconds)"), "60" },
			{ "connectivity-response", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_STRING, &cli->connectivity_response, N_("The expected start of the response"), N_("Bingo!") },
			{ 0 },
		};

		g_option_context_add_main_entries (opt_ctx, config_options, NULL);
	}
}

/************************************************************************/

GKeyFile *
nm_config_create_keyfile ()
{
	GKeyFile *keyfile;

	keyfile = g_key_file_new ();
	g_key_file_set_list_separator (keyfile, ',');
	return keyfile;
}

static gboolean
read_config (GKeyFile *keyfile, const char *path, GError **error)
{
	GKeyFile *kf;
	char **groups, **keys;
	gsize ngroups, nkeys;
	int g, k;

	g_return_val_if_fail (keyfile, FALSE);
	g_return_val_if_fail (path, FALSE);
	g_return_val_if_fail (!error || !*error, FALSE);

	if (g_file_test (path, G_FILE_TEST_EXISTS) == FALSE) {
		g_set_error (error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_NOT_FOUND, "file %s not found", path);
		return FALSE;
	}

	nm_log_dbg (LOGD_SETTINGS, "Reading config file '%s'", path);

	kf = nm_config_create_keyfile ();
	if (!g_key_file_load_from_file (kf, path, G_KEY_FILE_NONE, error)) {
		g_key_file_free (kf);
		return FALSE;
	}

	/* Override the current settings with the new ones */
	groups = g_key_file_get_groups (kf, &ngroups);
	for (g = 0; groups[g]; g++) {
		keys = g_key_file_get_keys (kf, groups[g], &nkeys, NULL);
		if (!keys)
			continue;
		for (k = 0; keys[k]; k++) {
			int len = strlen (keys[k]);
			char *v;

			if (keys[k][len - 1] == '+') {
				char *base_key = g_strndup (keys[k], len - 1);
				char *old_val = g_key_file_get_value (keyfile, groups[g], base_key, NULL);
				char *new_val = g_key_file_get_value (kf, groups[g], keys[k], NULL);

				if (old_val && *old_val) {
					char *combined = g_strconcat (old_val, ",", new_val, NULL);

					g_key_file_set_value (keyfile, groups[g], base_key, combined);
					g_free (combined);
				} else
					g_key_file_set_value (keyfile, groups[g], base_key, new_val);

				g_free (base_key);
				g_free (old_val);
				g_free (new_val);
				continue;
			}

			g_key_file_set_value (keyfile, groups[g], keys[k],
			                      v = g_key_file_get_value (kf, groups[g], keys[k], NULL));
			g_free (v);
		}
		g_strfreev (keys);
	}
	g_strfreev (groups);
	g_key_file_free (kf);

	return TRUE;
}

static gboolean
read_base_config (GKeyFile *keyfile,
                  const char *cli_config_main_file,
                  char **out_config_main_file,
                  GError **error)
{
	GError *my_error = NULL;

	g_return_val_if_fail (keyfile, FALSE);
	g_return_val_if_fail (out_config_main_file && !*out_config_main_file, FALSE);
	g_return_val_if_fail (!error || !*error, FALSE);

	/* Try a user-specified config file first */
	if (cli_config_main_file) {
		/* Bad user-specific config file path is a hard error */
		if (read_config (keyfile, cli_config_main_file, error)) {
			*out_config_main_file = g_strdup (cli_config_main_file);
			return TRUE;
		} else
			return FALSE;
	}

	/* Even though we prefer NetworkManager.conf, we need to check the
	 * old nm-system-settings.conf first to preserve compat with older
	 * setups.  In package managed systems dropping a NetworkManager.conf
	 * onto the system would make NM use it instead of nm-system-settings.conf,
	 * changing behavior during an upgrade.  We don't want that.
	 */

	/* Try deprecated nm-system-settings.conf first */
	if (read_config (keyfile, NM_OLD_SYSTEM_CONF_FILE, &my_error)) {
		*out_config_main_file = g_strdup (NM_OLD_SYSTEM_CONF_FILE);
		return TRUE;
	}

	if (!g_error_matches (my_error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_NOT_FOUND)) {
		nm_log_warn (LOGD_CORE, "Old default config file %s invalid: %s\n",
		             NM_OLD_SYSTEM_CONF_FILE,
		             my_error->message);
	}
	g_clear_error (&my_error);

	/* Try the standard config file location next */
	if (read_config (keyfile, NM_DEFAULT_SYSTEM_CONF_FILE, &my_error)) {
		*out_config_main_file = g_strdup (NM_DEFAULT_SYSTEM_CONF_FILE);
		return TRUE;
	}

	if (!g_error_matches (my_error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_NOT_FOUND)) {
		nm_log_warn (LOGD_CORE, "Default config file %s invalid: %s\n",
		             NM_DEFAULT_SYSTEM_CONF_FILE,
		             my_error->message);
		g_propagate_error (error, my_error);
		return FALSE;
	}
	g_clear_error (&my_error);

	/* If for some reason no config file exists, use the default
	 * config file path.
	 */
	*out_config_main_file = g_strdup (NM_DEFAULT_SYSTEM_CONF_FILE);
	nm_log_info (LOGD_CORE, "No config file found or given; using %s\n",
	             NM_DEFAULT_SYSTEM_CONF_FILE);
	return TRUE;
}

static int
sort_asciibetically (gconstpointer a, gconstpointer b)
{
	const char *s1 = *(const char **)a;
	const char *s2 = *(const char **)b;

	return strcmp (s1, s2);
}

static GPtrArray *
_get_config_dir_files (const char *config_main_file,
                       const char *config_dir,
                       char **out_config_description)
{
	GFile *dir;
	GFileEnumerator *direnum;
	GFileInfo *info;
	GPtrArray *confs;
	GString *config_description;
	const char *name;
	guint i;

	g_return_val_if_fail (config_main_file, NULL);
	g_return_val_if_fail (config_dir, NULL);
	g_return_val_if_fail (out_config_description && !*out_config_description, NULL);

	confs = g_ptr_array_new_with_free_func (g_free);
	config_description = g_string_new (config_main_file);
	dir = g_file_new_for_path (config_dir);
	direnum = g_file_enumerate_children (dir, G_FILE_ATTRIBUTE_STANDARD_NAME, 0, NULL, NULL);
	if (direnum) {
		while ((info = g_file_enumerator_next_file (direnum, NULL, NULL))) {
			name = g_file_info_get_name (info);
			if (g_str_has_suffix (name, ".conf"))
				g_ptr_array_add (confs, g_strdup (name));
			g_object_unref (info);
		}
		g_object_unref (direnum);
	}
	g_object_unref (dir);

	if (confs->len > 0) {
		g_ptr_array_sort (confs, sort_asciibetically);
		g_string_append (config_description, " and conf.d: ");
		for (i = 0; i < confs->len; i++) {
			char *n = confs->pdata[i];

			if (i > 0)
				g_string_append (config_description, ", ");
			g_string_append (config_description, n);
			confs->pdata[i] = g_build_filename (config_dir, n, NULL);
			g_free (n);
		}
	}

	*out_config_description = g_string_free (config_description, FALSE);
	return confs;
}

static GKeyFile *
read_entire_config (const NMConfigCmdLineOptions *cli,
                    const char *config_dir,
                    char **out_config_main_file,
                    char **out_config_description,
                    GError **error)
{
	GKeyFile *keyfile = nm_config_create_keyfile ();
	GPtrArray *confs;
	guint i;
	char *o_config_main_file = NULL;
	char *o_config_description = NULL;
	char **plugins_tmp;

	g_return_val_if_fail (config_dir, NULL);
	g_return_val_if_fail (out_config_main_file && !*out_config_main_file, FALSE);
	g_return_val_if_fail (out_config_description && !*out_config_description, NULL);
	g_return_val_if_fail (!error || !*error, FALSE);

	/* First read the base config file */
	if (!read_base_config (keyfile, cli ? cli->config_main_file : NULL, &o_config_main_file, error)) {
		g_key_file_free (keyfile);
		return NULL;
	}

	g_assert (o_config_main_file);

	confs = _get_config_dir_files (o_config_main_file, config_dir, &o_config_description);
	for (i = 0; i < confs->len; i++) {
		if (!read_config (keyfile, confs->pdata[i], error)) {
			g_key_file_free (keyfile);
			g_free (o_config_main_file);
			g_free (o_config_description);
			g_ptr_array_unref (confs);
			return NULL;
		}
	}
	g_ptr_array_unref (confs);

	/* Merge settings from command line. They overwrite everything read from
	 * config files. */

	if (cli && cli->plugins && cli->plugins[0])
		g_key_file_set_value (keyfile, "main", "plugins", cli->plugins);
	plugins_tmp = g_key_file_get_string_list (keyfile, "main", "plugins", NULL, NULL);
	if (!plugins_tmp) {
		if (STRLEN (CONFIG_PLUGINS_DEFAULT) > 0)
			g_key_file_set_value (keyfile, "main", "plugins", CONFIG_PLUGINS_DEFAULT);
	} else
		g_strfreev (plugins_tmp);

	if (cli && cli->configure_and_quit)
		g_key_file_set_value (keyfile, "main", "configure-and-quit", "true");

	if (cli && cli->connectivity_uri && cli->connectivity_uri[0])
		g_key_file_set_value (keyfile, "connectivity", "uri", cli->connectivity_uri);
	if (cli && cli->connectivity_interval >= 0)
		g_key_file_set_integer (keyfile, "connectivity", "interval", cli->connectivity_interval);
	if (cli && cli->connectivity_response && cli->connectivity_response[0])
		g_key_file_set_value (keyfile, "connectivity", "response", cli->connectivity_response);

	*out_config_main_file = o_config_main_file;
	*out_config_description = o_config_description;
	return keyfile;
}

GSList *
nm_config_get_device_match_spec (const GKeyFile *keyfile, const char *group, const char *key)
{
	gs_free char *value = NULL;

	value = g_key_file_get_string ((GKeyFile *) keyfile, group, key, NULL);
	return nm_match_spec_split (value);
}

/************************************************************************/

void
nm_config_reload (NMConfig *self)
{
	NMConfigPrivate *priv;
	GError *error = NULL;
	GKeyFile *keyfile;
	NMConfigData *new_data = NULL;
	char *config_main_file = NULL;
	char *config_description = NULL;

	g_return_if_fail (NM_IS_CONFIG (self));

	priv = NM_CONFIG_GET_PRIVATE (self);

	/* pass on the original command line options. This means, that
	 * options specified at command line cannot ever be reloaded from
	 * file. That seems desirable.
	 */
	keyfile = read_entire_config (&priv->cli,
	                              priv->config_dir,
	                              &config_main_file,
	                              &config_description,
	                              &error);
	if (!keyfile) {
		nm_log_err (LOGD_CORE, "Failed to reload the configuration: %s", error->message);
		g_clear_error (&error);
		return;
	}
	new_data = nm_config_data_new (config_main_file, config_description, nm_config_data_get_no_auto_default (priv->config_data), keyfile);
	g_free (config_main_file);
	g_free (config_description);
	g_key_file_unref (keyfile);

	_set_config_data (self, new_data);
}

static const char *
_change_flags_one_to_string (NMConfigChangeFlags flag)
{
	switch (flag) {
	case NM_CONFIG_CHANGE_CONFIG_FILES:
		return "config-files";
	case NM_CONFIG_CHANGE_VALUES:
		return "values";
	case NM_CONFIG_CHANGE_CONNECTIVITY:
		return "connectivity";
	case NM_CONFIG_CHANGE_NO_AUTO_DEFAULT:
		return "no-auto-default";
	case NM_CONFIG_CHANGE_DNS_MODE:
		return "dns-mode";
	case NM_CONFIG_CHANGE_RC_MANAGER:
		return "rc-manager";
	default:
		g_return_val_if_reached ("unknown");
	}
}

char *
nm_config_change_flags_to_string (NMConfigChangeFlags flags)
{
	GString *str = g_string_new ("");
	NMConfigChangeFlags s = 0x01;

	while (flags) {
		if (NM_FLAGS_HAS (flags, s)) {
			if (str->len)
				g_string_append_c (str, ',');
			g_string_append (str, _change_flags_one_to_string (s));
		}
		flags = flags & ~s;
		s <<= 1;
	}
	return g_string_free (str, FALSE);
}

static void
_set_config_data (NMConfig *self, NMConfigData *new_data)
{
	NMConfigPrivate *priv = NM_CONFIG_GET_PRIVATE (self);
	NMConfigData *old_data = priv->config_data;
	NMConfigChangeFlags changes;
	gs_free char *log_str = NULL;

	changes = nm_config_data_diff (old_data, new_data);
	if (changes == NM_CONFIG_CHANGE_NONE) {
		g_object_unref (new_data);
		return;
	}

	nm_log_info (LOGD_CORE, "config: update %s (%s)", nm_config_data_get_config_description (new_data),
	             (log_str = nm_config_change_flags_to_string (changes)));
	priv->config_data = new_data;
	g_signal_emit (self, signals[SIGNAL_CONFIG_CHANGED], 0, new_data, changes, old_data);
	g_object_unref (old_data);
}

NM_DEFINE_SINGLETON_DESTRUCTOR (NMConfig);
NM_DEFINE_SINGLETON_WEAK_REF (NMConfig);

NMConfig *
nm_config_get (void)
{
	g_assert (singleton_instance);
	return singleton_instance;
}

NMConfig *
nm_config_setup (const NMConfigCmdLineOptions *cli, GError **error)
{
	g_assert (!singleton_instance);

	singleton_instance = nm_config_new (cli, error);
	if (singleton_instance)
		nm_singleton_instance_weak_ref_register ();
	return singleton_instance;
}

static gboolean
init_sync (GInitable *initable, GCancellable *cancellable, GError **error)
{
	NMConfig *self = NM_CONFIG (initable);
	NMConfigPrivate *priv = NM_CONFIG_GET_PRIVATE (self);
	GKeyFile *keyfile;
	char *config_main_file = NULL;
	char *config_description = NULL;
	char **no_auto_default;
	GSList *no_auto_default_orig_list;
	GPtrArray *no_auto_default_orig;

	if (priv->config_dir) {
		/* Object is already initialized. */
		if (priv->config_data)
			return TRUE;
		g_set_error (error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_NOT_FOUND, "unspecified error");
		return FALSE;
	}

	if (priv->cli.config_dir)
		priv->config_dir = g_strdup (priv->cli.config_dir);
	else
		priv->config_dir = g_strdup (NM_DEFAULT_SYSTEM_CONF_DIR);

	keyfile = read_entire_config (&priv->cli,
	                              priv->config_dir,
	                              &config_main_file,
	                              &config_description,
	                              error);
	if (!keyfile)
		return FALSE;

	/* Initialize read only private members */

	if (priv->cli.no_auto_default_file)
		priv->no_auto_default_file = g_strdup (priv->cli.no_auto_default_file);
	else
		priv->no_auto_default_file = g_strdup (NM_NO_AUTO_DEFAULT_STATE_FILE);

	priv->plugins = g_key_file_get_string_list (keyfile, "main", "plugins", NULL, NULL);
	if (!priv->plugins)
		priv->plugins = g_new0 (char *, 1);

	priv->monitor_connection_files = nm_config_keyfile_get_boolean (keyfile, "main", "monitor-connection-files", FALSE);

	priv->auth_polkit = nm_config_keyfile_get_boolean (keyfile, "main", "auth-polkit", NM_CONFIG_DEFAULT_AUTH_POLKIT);

	priv->dhcp_client = g_key_file_get_value (keyfile, "main", "dhcp", NULL);

	priv->log_level = g_key_file_get_value (keyfile, "logging", "level", NULL);
	priv->log_domains = g_key_file_get_value (keyfile, "logging", "domains", NULL);

	priv->debug = g_key_file_get_value (keyfile, "main", "debug", NULL);

	priv->configure_and_quit = nm_config_keyfile_get_boolean (keyfile, "main", "configure-and-quit", FALSE);

	no_auto_default_orig_list = nm_config_get_device_match_spec (keyfile, "main", "no-auto-default");

	no_auto_default_orig = _nm_utils_copy_slist_to_array (no_auto_default_orig_list, NULL, NULL);
	g_ptr_array_add (no_auto_default_orig, NULL);
	no_auto_default = no_auto_default_merge_from_file (priv->no_auto_default_file, (const char *const *) no_auto_default_orig->pdata);
	g_ptr_array_unref (no_auto_default_orig);

	g_slist_free_full (no_auto_default_orig_list, g_free);

	priv->config_data_orig = nm_config_data_new (config_main_file, config_description, (const char *const*) no_auto_default, keyfile);

	g_strfreev (no_auto_default);

	priv->config_data = g_object_ref (priv->config_data_orig);

	g_free (config_main_file);
	g_free (config_description);
	g_key_file_unref (keyfile);
	return TRUE;
}

NMConfig *
nm_config_new (const NMConfigCmdLineOptions *cli, GError **error)
{
	return NM_CONFIG (g_initable_new (NM_TYPE_CONFIG,
	                                  NULL,
	                                  error,
	                                  NM_CONFIG_CMD_LINE_OPTIONS, cli,
	                                  NULL));
}

static void
nm_config_init (NMConfig *config)
{
	NMConfigPrivate *priv = NM_CONFIG_GET_PRIVATE (config);

	priv->auth_polkit = NM_CONFIG_DEFAULT_AUTH_POLKIT;
}

static void
finalize (GObject *gobject)
{
	NMConfigPrivate *priv = NM_CONFIG_GET_PRIVATE (gobject);

	g_free (priv->config_dir);
	g_free (priv->no_auto_default_file);
	g_strfreev (priv->plugins);
	g_free (priv->dhcp_client);
	g_free (priv->log_level);
	g_free (priv->log_domains);
	g_free (priv->debug);

	_nm_config_cmd_line_options_clear (&priv->cli);

	g_clear_object (&priv->config_data);
	g_clear_object (&priv->config_data_orig);

	G_OBJECT_CLASS (nm_config_parent_class)->finalize (gobject);
}

static void
set_property (GObject *object, guint prop_id,
              const GValue *value, GParamSpec *pspec)
{
	NMConfig *self = NM_CONFIG (object);
	NMConfigPrivate *priv = NM_CONFIG_GET_PRIVATE (self);
	NMConfigCmdLineOptions *cli;

	switch (prop_id) {
	case PROP_CMD_LINE_OPTIONS:
		/* construct only */
		cli = g_value_get_pointer (value);
		if (!cli)
			_nm_config_cmd_line_options_clear (&priv->cli);
		else
			_nm_config_cmd_line_options_copy (cli, &priv->cli);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
nm_config_class_init (NMConfigClass *config_class)
{
	GObjectClass *object_class = G_OBJECT_CLASS (config_class);

	g_type_class_add_private (config_class, sizeof (NMConfigPrivate));
	object_class->finalize = finalize;
	object_class->set_property = set_property;

	g_object_class_install_property
	    (object_class, PROP_CMD_LINE_OPTIONS,
	     g_param_spec_pointer (NM_CONFIG_CMD_LINE_OPTIONS, "", "",
	                           G_PARAM_WRITABLE |
	                           G_PARAM_CONSTRUCT_ONLY |
	                           G_PARAM_STATIC_STRINGS));

	signals[SIGNAL_CONFIG_CHANGED] =
	    g_signal_new (NM_CONFIG_SIGNAL_CONFIG_CHANGED,
	                  G_OBJECT_CLASS_TYPE (object_class),
	                  G_SIGNAL_RUN_FIRST,
	                  G_STRUCT_OFFSET (NMConfigClass, config_changed),
	                  NULL, NULL, NULL,
	                  G_TYPE_NONE, 3, NM_TYPE_CONFIG_DATA, NM_TYPE_CONFIG_CHANGE_FLAGS, NM_TYPE_CONFIG_DATA);
}

static void
nm_config_initable_iface_init (GInitableIface *iface)
{
	iface->init = init_sync;
}

