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
#include "nm-keyfile-internal.h"

#include <gio/gio.h>
#include <glib/gi18n.h>

#define DEFAULT_CONFIG_MAIN_FILE        NMCONFDIR "/NetworkManager.conf"
#define DEFAULT_CONFIG_DIR              NMCONFDIR "/conf.d"
#define DEFAULT_CONFIG_MAIN_FILE_OLD    NMCONFDIR "/nm-system-settings.conf"
#define DEFAULT_SYSTEM_CONFIG_DIR       NMLIBDIR  "/conf.d"
#define DEFAULT_NO_AUTO_DEFAULT_FILE    NMSTATEDIR "/no-auto-default.state"
#define DEFAULT_INTERN_CONFIG_FILE      NMSTATEDIR "/NetworkManager-intern.conf"

struct NMConfigCmdLineOptions {
	char *config_main_file;
	char *intern_config_file;
	char *config_dir;
	char *system_config_dir;
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
	char *system_config_dir;
	char *no_auto_default_file;
	char *intern_config_file;

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

#define _HAS_PREFIX(str, prefix) \
	({ \
		const char *_str = (str); \
		g_str_has_prefix ( _str, ""prefix"") && _str[STRLEN(prefix)] != '\0'; \
	})

/************************************************************************/

gint
nm_config_parse_boolean (const char *str,
                         gint default_value)
{
	gsize len;
	char *s = NULL;

	if (!str)
		return default_value;

	while (str[0] && g_ascii_isspace (str[0]))
		str++;

	if (!str[0])
		return default_value;

	len = strlen (str);
	if (g_ascii_isspace (str[len - 1])) {
		s = g_strdup (str);
		g_strchomp (s);
		str = s;
	}

	if (!g_ascii_strcasecmp (str, "true") || !g_ascii_strcasecmp (str, "yes") || !g_ascii_strcasecmp (str, "on") || !g_ascii_strcasecmp (str, "1"))
		default_value = TRUE;
	else if (!g_ascii_strcasecmp (str, "false") || !g_ascii_strcasecmp (str, "no") || !g_ascii_strcasecmp (str, "off") || !g_ascii_strcasecmp (str, "0"))
		default_value = FALSE;
	if (s)
		g_free (s);
	return default_value;
}

gint
nm_config_keyfile_get_boolean (GKeyFile *keyfile,
                               const char *section,
                               const char *key,
                               gint default_value)
{
	gs_free char *str = NULL;

	g_return_val_if_fail (keyfile != NULL, default_value);
	g_return_val_if_fail (section != NULL, default_value);
	g_return_val_if_fail (key != NULL, default_value);

	str = g_key_file_get_value (keyfile, section, key, NULL);
	return nm_config_parse_boolean (str, default_value);
}

void
nm_config_keyfile_set_string_list (GKeyFile *keyfile,
                                   const char *group,
                                   const char *key,
                                   const char *const* strv,
                                   gssize len)
{
	gsize l;
	char *new_value;

	if (len < 0)
		len = strv ? g_strv_length ((char **) strv) : 0;

	g_key_file_set_string_list (keyfile, group, key, strv, len);

	/* g_key_file_set_string_list() appends a trailing separator to the value.
	 * We don't like that, get rid of it. */

	new_value = g_key_file_get_value (keyfile, group, key, NULL);
	if (!new_value)
		return;

	l = strlen (new_value);
	if (l > 0 && new_value[l - 1] == NM_CONFIG_KEYFILE_LIST_SEPARATOR) {
		/* Maybe we should check that value doesn't end with "\\,", i.e.
		 * with an escaped separator. But the way g_key_file_set_string_list()
		 * is implemented (currently), it always adds a trailing separator. */
		new_value[l - 1] = '\0';
		g_key_file_set_value (keyfile, group, key, new_value);
	}
	g_free (new_value);
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
no_auto_default_from_file (const char *no_auto_default_file)
{
	GPtrArray *no_auto_default_new;
	char **list;
	guint i;
	char *data;

	no_auto_default_new = g_ptr_array_new ();

	if (   no_auto_default_file
	    && g_file_get_contents (no_auto_default_file, &data, NULL, NULL)) {
		list = g_strsplit (data, "\n", -1);
		for (i = 0; list[i]; i++) {
			if (   *list[i]
			    && nm_utils_hwaddr_valid (list[i], -1)
			    && _nm_utils_strv_find_first (list, i, list[i]) < 0)
				g_ptr_array_add (no_auto_default_new, list[i]);
			else
				g_free (list[i]);
		}
		g_free (list);
		g_free (data);
	}

	g_ptr_array_add (no_auto_default_new, NULL);
	return (char **) g_ptr_array_free (no_auto_default_new, FALSE);
}

static gboolean
no_auto_default_to_file (const char *no_auto_default_file, const char *const*no_auto_default, GError **error)
{
	GString *data;
	gboolean success;
	guint i;

	data = g_string_new ("");
	for (i = 0; no_auto_default && no_auto_default[i]; i++) {
		g_string_append (data, no_auto_default[i]);
		g_string_append_c (data, '\n');
	}
	success = g_file_set_contents (no_auto_default_file, data->str, data->len, error);
	g_string_free (data, TRUE);
	return success;
}

gboolean
nm_config_get_no_auto_default_for_device (NMConfig *self, NMDevice *device)
{
	g_return_val_if_fail (NM_IS_CONFIG (self), FALSE);

	return nm_config_data_get_no_auto_default_for_device (NM_CONFIG_GET_PRIVATE (self)->config_data, device);
}

void
nm_config_set_no_auto_default_for_device (NMConfig *self, NMDevice *device)
{
	NMConfigPrivate *priv;
	GError *error = NULL;
	NMConfigData *new_data = NULL;
	const char *hw_address;
	const char *const*no_auto_default_current;
	GPtrArray *no_auto_default_new = NULL;
	guint i;

	g_return_if_fail (NM_IS_CONFIG (self));
	g_return_if_fail (NM_IS_DEVICE (device));

	priv = NM_CONFIG_GET_PRIVATE (self);

	hw_address = nm_device_get_hw_address (device);

	no_auto_default_current = nm_config_data_get_no_auto_default (priv->config_data);

	if (_nm_utils_strv_find_first ((char **) no_auto_default_current, -1, hw_address) >= 0) {
		/* @hw_address is already blocked. We don't have to update our in-memory representation.
		 * Maybe we should write to no_auto_default_file anew, but let's save that too. */
		return;
	}

	no_auto_default_new = g_ptr_array_new ();
	for (i = 0; no_auto_default_current && no_auto_default_current[i]; i++)
		g_ptr_array_add (no_auto_default_new, (char *) no_auto_default_current[i]);
	g_ptr_array_add (no_auto_default_new, (char *) hw_address);
	g_ptr_array_add (no_auto_default_new, NULL);

	if (!no_auto_default_to_file (priv->no_auto_default_file, (const char *const*) no_auto_default_new->pdata, &error)) {
		nm_log_warn (LOGD_SETTINGS, "Could not update no-auto-default.state file: %s",
		             error->message);
		g_error_free (error);
	}

	new_data = nm_config_data_new_update_no_auto_default (priv->config_data, (const char *const*) no_auto_default_new->pdata);

	/* unref no_auto_default_set here. Note that _set_config_data() probably invalidates the content of the array. */
	g_ptr_array_unref (no_auto_default_new);

	_set_config_data (self, new_data);
}

/************************************************************************/

static void
_nm_config_cmd_line_options_clear (NMConfigCmdLineOptions *cli)
{
	g_clear_pointer (&cli->config_main_file, g_free);
	g_clear_pointer (&cli->config_dir, g_free);
	g_clear_pointer (&cli->system_config_dir, g_free);
	g_clear_pointer (&cli->no_auto_default_file, g_free);
	g_clear_pointer (&cli->intern_config_file, g_free);
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
	dst->system_config_dir = g_strdup (cli->system_config_dir);
	dst->config_main_file = g_strdup (cli->config_main_file);
	dst->no_auto_default_file = g_strdup (cli->no_auto_default_file);
	dst->intern_config_file = g_strdup (cli->intern_config_file);
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
			{ "config", 0, 0, G_OPTION_ARG_FILENAME, &cli->config_main_file, N_("Config file location"), N_(DEFAULT_CONFIG_MAIN_FILE) },
			{ "config-dir", 0, 0, G_OPTION_ARG_FILENAME, &cli->config_dir, N_("Config directory location"), N_(DEFAULT_CONFIG_DIR) },
			{ "system-config-dir", 0, 0, G_OPTION_ARG_FILENAME, &cli->system_config_dir, N_("System config directory location"), N_(DEFAULT_SYSTEM_CONFIG_DIR) },
			{ "intern-config", 0, 0, G_OPTION_ARG_FILENAME, &cli->intern_config_file, N_("Internal config file location"), N_(DEFAULT_INTERN_CONFIG_FILE) },
			{ "no-auto-default", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_FILENAME, &cli->no_auto_default_file, N_("State file for no-auto-default devices"), N_(DEFAULT_NO_AUTO_DEFAULT_FILE) },
			{ "plugins", 0, 0, G_OPTION_ARG_STRING, &cli->plugins, N_("List of plugins separated by ','"), N_(CONFIG_PLUGINS_DEFAULT) },
			{ "configure-and-quit", 0, 0, G_OPTION_ARG_NONE, &cli->configure_and_quit, N_("Quit after initial configuration"), NULL },

				/* These three are hidden for now, and should eventually just go away. */
			{ "connectivity-uri", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_STRING, &cli->connectivity_uri, N_("An http(s) address for checking internet connectivity"), "http://example.com" },
			{ "connectivity-interval", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_INT, &cli->connectivity_interval, N_("The interval between connectivity checks (in seconds)"), G_STRINGIFY (NM_CONFIG_DEFAULT_CONNECTIVITY_INTERVAL) },
			{ "connectivity-response", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_STRING, &cli->connectivity_response, N_("The expected start of the response"), N_(NM_CONFIG_DEFAULT_CONNECTIVITY_RESPONSE) },
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
	g_key_file_set_list_separator (keyfile, NM_CONFIG_KEYFILE_LIST_SEPARATOR);
	return keyfile;
}

static gboolean
read_config (GKeyFile *keyfile, const char *dirname, const char *path, GError **error)
{
	GKeyFile *kf;
	char **groups, **keys;
	gsize ngroups, nkeys;
	int g, k;
	gs_free char *path_free = NULL;

	g_return_val_if_fail (keyfile, FALSE);
	g_return_val_if_fail (path, FALSE);
	g_return_val_if_fail (!error || !*error, FALSE);

	if (dirname) {
		path_free = g_build_filename (dirname, path, NULL);
		path = path_free;
	}

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
		if (g_str_has_prefix (groups[g], NM_CONFIG_KEYFILE_GROUPPREFIX_INTERN)) {
			/* internal groups cannot be set by user configuration. */
			continue;
		}
		keys = g_key_file_get_keys (kf, groups[g], &nkeys, NULL);
		if (!keys)
			continue;
		for (k = 0; keys[k]; k++) {
			const char *key;
			char *new_value;
			char last_char;
			gsize key_len;

			key = keys[k];
			g_assert (key && *key);

			if (   _HAS_PREFIX (key, NM_CONFIG_KEYFILE_KEYPREFIX_WAS)
			    || _HAS_PREFIX (key, NM_CONFIG_KEYFILE_KEYPREFIX_SET)) {
				/* these keys are protected. We ignore them if the user sets them. */
				continue;
			}

			key_len = strlen (key);
			last_char = key[key_len - 1];
			if (   key_len > 1
			    && (last_char == '+' || last_char == '-')) {
				gs_free char *base_key = g_strndup (key, key_len - 1);
				gs_strfreev  char **old_val = g_key_file_get_string_list (keyfile, groups[g], base_key, NULL, NULL);
				gs_free char **new_val = g_key_file_get_string_list (kf, groups[g], key, NULL, NULL);
				gs_unref_ptrarray GPtrArray *new = g_ptr_array_new_with_free_func (g_free);
				char **iter_val;

				for (iter_val = old_val; iter_val && *iter_val; iter_val++) {
					if (   last_char != '-'
					    || _nm_utils_strv_find_first (new_val, -1, *iter_val) < 0)
						g_ptr_array_add (new, g_strdup (*iter_val));
				}
				for (iter_val = new_val; iter_val && *iter_val; iter_val++) {
					/* don't add duplicates. That means an "option=a,b"; "option+=a,c" results in "option=a,b,c" */
					if (   last_char == '+'
					    && _nm_utils_strv_find_first (old_val, -1, *iter_val) < 0)
						g_ptr_array_add (new, *iter_val);
					else
						g_free (*iter_val);
				}

				if (new->len > 0)
					nm_config_keyfile_set_string_list (keyfile, groups[g], base_key, (const char *const*) new->pdata, new->len);
				else
					g_key_file_remove_key (keyfile, groups[g], base_key, NULL);
				continue;
			}

			new_value = g_key_file_get_value (kf, groups[g], key, NULL);
			g_key_file_set_value (keyfile, groups[g], key, new_value);
			g_free (new_value);
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
		if (read_config (keyfile, NULL, cli_config_main_file, error)) {
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
	if (read_config (keyfile, NULL, DEFAULT_CONFIG_MAIN_FILE_OLD, &my_error)) {
		*out_config_main_file = g_strdup (DEFAULT_CONFIG_MAIN_FILE_OLD);
		return TRUE;
	}

	if (!g_error_matches (my_error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_NOT_FOUND)) {
		nm_log_warn (LOGD_CORE, "Old default config file %s invalid: %s\n",
		             DEFAULT_CONFIG_MAIN_FILE_OLD,
		             my_error->message);
	}
	g_clear_error (&my_error);

	/* Try the standard config file location next */
	if (read_config (keyfile, NULL, DEFAULT_CONFIG_MAIN_FILE, &my_error)) {
		*out_config_main_file = g_strdup (DEFAULT_CONFIG_MAIN_FILE);
		return TRUE;
	}

	if (!g_error_matches (my_error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_NOT_FOUND)) {
		nm_log_warn (LOGD_CORE, "Default config file %s invalid: %s\n",
		             DEFAULT_CONFIG_MAIN_FILE,
		             my_error->message);
		g_propagate_error (error, my_error);
		return FALSE;
	}
	g_clear_error (&my_error);

	/* If for some reason no config file exists, use the default
	 * config file path.
	 */
	*out_config_main_file = g_strdup (DEFAULT_CONFIG_MAIN_FILE);
	nm_log_info (LOGD_CORE, "No config file found or given; using %s\n",
	             DEFAULT_CONFIG_MAIN_FILE);
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
_get_config_dir_files (const char *config_dir)
{
	GFile *dir;
	GFileEnumerator *direnum;
	GFileInfo *info;
	GPtrArray *confs;
	const char *name;

	g_return_val_if_fail (config_dir, NULL);

	confs = g_ptr_array_new_with_free_func (g_free);
	if (!*config_dir)
		return confs;

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

	g_ptr_array_sort (confs, sort_asciibetically);
	return confs;
}

static GKeyFile *
read_entire_config (const NMConfigCmdLineOptions *cli,
                    const char *config_dir,
                    const char *system_config_dir,
                    char **out_config_main_file,
                    char **out_config_description,
                    GError **error)
{
	GKeyFile *keyfile;
	gs_unref_ptrarray GPtrArray *system_confs = NULL;
	gs_unref_ptrarray GPtrArray *confs = NULL;
	guint i;
	gs_free char *o_config_main_file = NULL;
	GString *str;
	char **plugins_default;

	g_return_val_if_fail (config_dir, NULL);
	g_return_val_if_fail (system_config_dir, NULL);
	g_return_val_if_fail (!out_config_main_file || !*out_config_main_file, FALSE);
	g_return_val_if_fail (!out_config_description || !*out_config_description, NULL);
	g_return_val_if_fail (!error || !*error, FALSE);

	/* create a default configuration file. */
	keyfile = nm_config_create_keyfile ();

	plugins_default = g_strsplit (CONFIG_PLUGINS_DEFAULT, ",", -1);
	if (plugins_default && plugins_default[0])
		nm_config_keyfile_set_string_list (keyfile, NM_CONFIG_KEYFILE_GROUP_MAIN, "plugins", (const char *const*) plugins_default, -1);
	g_strfreev (plugins_default);

	system_confs = _get_config_dir_files (system_config_dir);
	confs = _get_config_dir_files (config_dir);

	for (i = 0; i < system_confs->len; ) {
		const char *filename = system_confs->pdata[i];

		/* if a same named file exists in config_dir, skip it. */
		if (_nm_utils_strv_find_first ((char **) confs->pdata, confs->len, filename) >= 0) {
			g_ptr_array_remove_index (system_confs, i);
			continue;
		}

		if (!read_config (keyfile, system_config_dir, filename, error)) {
			g_key_file_free (keyfile);
			return NULL;
		}
		i++;
	}

	/* First read the base config file */
	if (!read_base_config (keyfile, cli ? cli->config_main_file : NULL, &o_config_main_file, error)) {
		g_key_file_free (keyfile);
		return NULL;
	}

	g_assert (o_config_main_file);

	for (i = 0; i < confs->len; i++) {
		if (!read_config (keyfile, config_dir, confs->pdata[i], error)) {
			g_key_file_free (keyfile);
			return NULL;
		}
	}

	/* Merge settings from command line. They overwrite everything read from
	 * config files. */
	if (cli && cli->plugins)
		g_key_file_set_value (keyfile, NM_CONFIG_KEYFILE_GROUP_MAIN, "plugins", cli->plugins);
	if (cli && cli->configure_and_quit)
		g_key_file_set_value (keyfile, NM_CONFIG_KEYFILE_GROUP_MAIN, "configure-and-quit", "true");
	if (cli && cli->connectivity_uri && cli->connectivity_uri[0])
		g_key_file_set_value (keyfile, NM_CONFIG_KEYFILE_GROUP_CONNECTIVITY, "uri", cli->connectivity_uri);
	if (cli && cli->connectivity_interval >= 0)
		g_key_file_set_integer (keyfile, NM_CONFIG_KEYFILE_GROUP_CONNECTIVITY, "interval", cli->connectivity_interval);
	if (cli && cli->connectivity_response && cli->connectivity_response[0])
		g_key_file_set_value (keyfile, NM_CONFIG_KEYFILE_GROUP_CONNECTIVITY, "response", cli->connectivity_response);

	str = g_string_new (o_config_main_file);
	if (system_confs->len > 0) {
		for (i = 0; i < system_confs->len; i++) {
			if (i == 0)
				g_string_append (str, " (lib: ");
			else
				g_string_append (str, ", ");
			g_string_append (str, system_confs->pdata[i]);
		}
		g_string_append (str, ")");
	}
	if (confs->len > 0) {
		for (i = 0; i < confs->len; i++) {
			if (i == 0)
				g_string_append (str, " (etc: ");
			else
				g_string_append (str, ", ");
			g_string_append (str, confs->pdata[i]);
		}
		g_string_append (str, ")");
	}

	if (out_config_main_file)
		*out_config_main_file = o_config_main_file;
	else
		g_free (o_config_main_file);
	if (out_config_description)
		*out_config_description = g_string_free (str, FALSE);
	else
		g_string_free (str, TRUE);

	o_config_main_file = NULL;
	return keyfile;
}

/**
 * intern_config_read:
 * @filename: the filename where to store the internal config
 * @keyfile_conf: the merged configuration from user (/etc/NM/NetworkManager.conf).
 * @out_needs_rewrite: whether the read keyfile contains inconsistent
 *   data (compared to @keyfile_conf). If %TRUE, you might want to rewrite
 *   the file.
 *
 * Does the opposite of intern_config_write(). It reads the internal configuration.
 * Note that the actual format of how the configuration is saved in @filename
 * is different then what we return here. NMConfig manages what is written internally
 * by having it inside a keyfile_intern. But we don't write that to disk as is.
 * Especially, we also store parts of @keyfile_conf as ".was" and on read we compare
 * what we have, with what ".was".
 *
 * Returns: a #GKeyFile instance with the internal configuration.
 */
static GKeyFile *
intern_config_read (const char *filename,
                    GKeyFile *keyfile_conf,
                    gboolean *out_needs_rewrite)
{
	GKeyFile *keyfile_intern;
	GKeyFile *keyfile;
	gboolean needs_rewrite = FALSE;
	gs_strfreev char **groups = NULL;
	guint g, k;
	gboolean has_intern = FALSE;

	g_return_val_if_fail (filename, NULL);

	if (!*filename) {
		if (out_needs_rewrite)
			*out_needs_rewrite = FALSE;
		return NULL;
	}

	keyfile_intern = nm_config_create_keyfile ();

	keyfile = nm_config_create_keyfile ();
	if (!g_key_file_load_from_file (keyfile, filename, G_KEY_FILE_NONE, NULL)) {
		needs_rewrite = TRUE;
		goto out;
	}

	groups = g_key_file_get_groups (keyfile, NULL);
	for (g = 0; groups && groups[g]; g++) {
		gs_strfreev char **keys = NULL;
		const char *group = groups[g];
		gboolean is_intern;

		keys = g_key_file_get_keys (keyfile, group, NULL, NULL);
		if (!keys)
			continue;

		is_intern = g_str_has_prefix (group, NM_CONFIG_KEYFILE_GROUPPREFIX_INTERN);

		for (k = 0; keys[k]; k++) {
			gs_free char *value_set = NULL;
			const char *key = keys[k];

			value_set = g_key_file_get_value (keyfile, group, key, NULL);

			if (is_intern) {
				has_intern = TRUE;
				g_key_file_set_value (keyfile_intern, group, key, value_set);
			} else if (_HAS_PREFIX (key, NM_CONFIG_KEYFILE_KEYPREFIX_SET)) {
				const char *key_base = &key[STRLEN (NM_CONFIG_KEYFILE_KEYPREFIX_SET)];
				gs_free char *value_was = NULL;
				gs_free char *value_conf = NULL;
				gs_free char *key_was = g_strdup_printf (NM_CONFIG_KEYFILE_KEYPREFIX_WAS"%s", key_base);

				if (keyfile_conf)
					value_conf = g_key_file_get_value (keyfile_conf, group, key_base, NULL);
				value_was = g_key_file_get_value (keyfile, group, key_was, NULL);

				if (g_strcmp0 (value_conf, value_was) != 0) {
					/* if value_was is no longer the same as @value_conf, it means the user
					 * changed the configuration since the last write. In this case, we
					 * drop the value. It also means our file is out-of-date, and we should
					 * rewrite it. */
					needs_rewrite = TRUE;
					continue;
				}
				has_intern = TRUE;
				g_key_file_set_value (keyfile_intern, group, key_base, value_set);
			} else if (_HAS_PREFIX (key, NM_CONFIG_KEYFILE_KEYPREFIX_WAS)) {
				const char *key_base = &key[STRLEN (NM_CONFIG_KEYFILE_KEYPREFIX_WAS)];
				gs_free char *key_set = g_strdup_printf (NM_CONFIG_KEYFILE_KEYPREFIX_SET"%s", key_base);
				gs_free char *value_was = NULL;
				gs_free char *value_conf = NULL;

				if (g_key_file_has_key (keyfile, group, key_set, NULL)) {
					/* we have a matching "set" key too. Handle the "was" key there. */
					continue;
				}

				if (keyfile_conf)
					value_conf = g_key_file_get_value (keyfile_conf, group, key_base, NULL);
				value_was = g_key_file_get_value (keyfile, group, key, NULL);

				if (g_strcmp0 (value_conf, value_was) != 0) {
					/* if value_was is no longer the same as @value_conf, it means the user
					 * changed the configuration since the last write. In this case, we
					 * don't overwrite the user-provided value. It also means our file is
					 * out-of-date, and we should rewrite it. */
					needs_rewrite = TRUE;
					continue;
				}
				has_intern = TRUE;
				/* signal the absense the value. That means, we must propagate the
				 * "was" key to NMConfigData, so that it knows to hide the corresponding
				 * user key. */
				g_key_file_set_value (keyfile_intern, group, key, "");
			} else
				needs_rewrite = TRUE;
		}
	}

out:
	g_key_file_unref (keyfile);

	*out_needs_rewrite = needs_rewrite;

	nm_log_dbg (LOGD_CORE, "intern config file \"%s\"", filename);

	if (!has_intern) {
		g_key_file_unref (keyfile_intern);
		return NULL;
	}
	return keyfile_intern;
}

static int
_intern_config_write_sort_fcn (const char **a, const char **b, gpointer dummy)
{
	const char *g_a = (a ? *a : NULL);
	const char *g_b = (b ? *b : NULL);
	gboolean a_is, b_is;

	a_is = g_str_has_prefix (g_a, NM_CONFIG_KEYFILE_GROUPPREFIX_INTERN);
	b_is = g_str_has_prefix (g_b, NM_CONFIG_KEYFILE_GROUPPREFIX_INTERN);

	if (a_is != b_is) {
		if (a_is)
			return 1;
		return -1;
	}
	return g_strcmp0 (g_a, g_b);
}

static gboolean
intern_config_write (const char *filename,
                     GKeyFile *keyfile_intern,
                     GKeyFile *keyfile_conf,
                     GError **error)
{
	GKeyFile *keyfile;
	gs_strfreev char **groups = NULL;
	guint g, k;
	gboolean has_intern = FALSE;
	gboolean success = FALSE;
	GError *local = NULL;

	g_return_val_if_fail (filename, FALSE);

	if (!*filename) {
		g_set_error (error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_NOT_FOUND, "no filename to write (use --intern-config?)");
		return FALSE;
	}

	keyfile = nm_config_create_keyfile ();

	if (keyfile_intern) {
		groups = g_key_file_get_groups (keyfile_intern, NULL);
		if (groups && groups[0]) {
			g_qsort_with_data (groups,
			                   g_strv_length (groups),
			                   sizeof (char *),
			                   (GCompareDataFunc) _intern_config_write_sort_fcn,
			                   NULL);
		}
	}
	for (g = 0; groups && groups[g]; g++) {
		gs_strfreev char **keys = NULL;
		const char *group = groups[g];
		gboolean is_intern;

		keys = g_key_file_get_keys (keyfile_intern, group, NULL, NULL);
		if (!keys)
			continue;

		is_intern = g_str_has_prefix (group, NM_CONFIG_KEYFILE_GROUPPREFIX_INTERN);

		for (k = 0; keys[k]; k++) {
			const char *key = keys[k];
			gs_free char *value_set = NULL;
			gs_free char *key_set = NULL;

			value_set = g_key_file_get_value (keyfile_intern, group, key, NULL);

			if (is_intern) {
				has_intern = TRUE;
				g_key_file_set_value (keyfile, group, key, value_set);
			} else {
				gs_free char *value_was = NULL;

				if (_HAS_PREFIX (key, NM_CONFIG_KEYFILE_KEYPREFIX_SET)) {
					/* Setting a key with .set prefix has no meaning, as these keys
					 * are protected. Just set the value you want to set instead.
					 * Why did this happen?? */
					g_warn_if_reached ();
				} else if (_HAS_PREFIX (key, NM_CONFIG_KEYFILE_KEYPREFIX_WAS)) {
					const char *key_base = &key[STRLEN (NM_CONFIG_KEYFILE_KEYPREFIX_WAS)];

					if (   _HAS_PREFIX (key_base, NM_CONFIG_KEYFILE_KEYPREFIX_SET)
					    || _HAS_PREFIX (key_base, NM_CONFIG_KEYFILE_KEYPREFIX_WAS)) {
						g_warn_if_reached ();
						continue;
					}

					if (g_key_file_has_key (keyfile_intern, group, key_base, NULL)) {
						/* There is also have a matching key_base entry. Skip processing
						 * the .was. key ad handle the key_base in the other else branch. */
						continue;
					}

					if (keyfile_conf) {
						value_was = g_key_file_get_value (keyfile_conf, group, key_base, NULL);
						if (value_was)
							g_key_file_set_value (keyfile, group, key, value_was);
					}
				} else {
					if (keyfile_conf) {
						value_was = g_key_file_get_value (keyfile_conf, group, key, NULL);
						if (g_strcmp0 (value_set, value_was) == 0) {
							/* there is no point in storing the identical value as we have via
							 * user configuration. Skip it. */
							continue;
						}
						if (value_was) {
							gs_free char *key_was = NULL;

							key_was = g_strdup_printf (NM_CONFIG_KEYFILE_KEYPREFIX_WAS"%s", key);
							g_key_file_set_value (keyfile, group, key_was, value_was);
						}
					}
					key = key_set = g_strdup_printf (NM_CONFIG_KEYFILE_KEYPREFIX_SET"%s", key);
					g_key_file_set_value (keyfile, group, key, value_set);
				}
			}
		}
		if (   is_intern
		    && g_key_file_has_group (keyfile, group)) {
			g_key_file_set_comment (keyfile, group, NULL,
			                        " Internal section. Not overwritable via user configuration in 'NetworkManager.conf'",
			                        NULL);
		}
	}

	g_key_file_set_comment (keyfile, NULL, NULL,
	                        " Internal configuration file. This file is written and read\n"
	                        " by NetworkManager and its configuration values are merged\n"
	                        " with the configuration from 'NetworkManager.conf'.\n"
	                        "\n"
	                        " Keys with a \""NM_CONFIG_KEYFILE_KEYPREFIX_SET"\" prefix specify the value to set.\n"
	                        " A corresponding key with a \""NM_CONFIG_KEYFILE_KEYPREFIX_WAS"\" prefix records the value\n"
	                        " of the user configuration at the time of storing the file.\n"
	                        " The value from internal configuration is rejected if the corresponding\n"
	                        " \""NM_CONFIG_KEYFILE_KEYPREFIX_WAS"\" key no longer matches the configuration from 'NetworkManager.conf'.\n"
	                        " That means, if you modify a value in 'NetworkManager.conf', the internal\n"
	                        " overwrite no longer matches and is ignored.\n"
	                        "\n"
	                        " Internal sections of the form [" NM_CONFIG_KEYFILE_GROUPPREFIX_INTERN "*] cannot\n"
	                        " be set by user configuration.\n"
	                        "\n"
	                        " CHANGES TO THIS FILE WILL BE OVERWRITTEN",
	                        NULL);

	success = g_key_file_save_to_file (keyfile, filename, &local);

	nm_log_dbg (LOGD_CORE, "write intern config file \"%s\"%s%s", filename, success ? "" : ": ", success ? "" : local->message);
	g_key_file_unref (keyfile);
	if (!success)
		g_propagate_error (error, local);
	return success;
}

/************************************************************************/

GSList *
nm_config_get_device_match_spec (const GKeyFile *keyfile, const char *group, const char *key)
{
	gs_free char *value = NULL;

	value = g_key_file_get_string ((GKeyFile *) keyfile, group, key, NULL);
	return nm_match_spec_split (value);
}

/************************************************************************/

/**
 * nm_config_set_values:
 * @self: the NMConfig instance
 * @keyfile_intern_new: (allow-none): the new internal settings to set.
 *   If %NULL, it is equal to an empty keyfile.
 * @allow_write: only if %TRUE, allow writing the changes to file. Otherwise,
 *   do the changes in-memory only.
 * @force_rewrite: if @allow_write is %FALSE, this has no effect. If %FALSE,
 *   only write the configuration to file, if there are any actual changes.
 *   If %TRUE, always write the configuration to file, even if tere are seemingly
 *   no changes.
 *
 *  This is the most flexible function to set values. It all depends on the
 *  keys and values you set in @keyfile_intern_new. You basically reset all
 *  internal configuration values to what is in @keyfile_intern_new.
 *
 *  There are 2 types of settings:
 *    - all groups/sections with a prefix [.intern.*] are taken as is. As these
 *      groups are separate from user configuration, there is no conflict. You set
 *      them, that's it.
 *    - otherwise you can overwrite individual values from user-configuration.
 *      Just set the value. Keys with a prefix NM_CONFIG_KEYFILE_KEYPREFIX_*
 *      are protected -- as they are not value user keys.
 *      You can also hide a certain user setting by putting only a key
 *      NM_CONFIG_KEYFILE_KEYPREFIX_WAS"keyname" into the keyfile.
 */
void
nm_config_set_values (NMConfig *self,
                      GKeyFile *keyfile_intern_new,
                      gboolean allow_write,
                      gboolean force_rewrite)
{
	NMConfigPrivate *priv;
	GKeyFile *keyfile_intern_current;
	GKeyFile *keyfile_user;
	GKeyFile *keyfile_new;
	GError *local = NULL;
	NMConfigData *new_data = NULL;

	g_return_if_fail (NM_IS_CONFIG (self));

	priv = NM_CONFIG_GET_PRIVATE (self);

	keyfile_intern_current = _nm_config_data_get_keyfile_intern (priv->config_data);

	keyfile_new = nm_config_create_keyfile ();
	if (keyfile_intern_new)
		_nm_keyfile_copy (keyfile_new, keyfile_intern_new);

	if (!_nm_keyfile_equals (keyfile_intern_current, keyfile_new, TRUE))
		new_data = nm_config_data_new_update_keyfile_intern (priv->config_data, keyfile_new);

	nm_log_dbg (LOGD_CORE, "set values(): %s", new_data ? "has changes" : "no changes");

	if (allow_write
	    && (new_data || force_rewrite)) {
		/* We write the internal config file based on the user configuration from
		 * the last load/reload. That is correct, because the intern properties might
		 * be in accordance to what NM thinks is currently configured. Even if the files
		 * on disk changed in the meantime.
		 * But if they changed, on the next reload with might throw away our just
		 * written data. That is correct, because from NM's point of view, those
		 * changes on disk happened in any case *after* now. */
		if (*priv->intern_config_file) {
			keyfile_user = _nm_config_data_get_keyfile_user (priv->config_data);
			if (!intern_config_write (priv->intern_config_file, keyfile_new, keyfile_user, &local)) {
				nm_log_warn (LOGD_CORE, "error saving internal configuration \"%s\": %s", priv->intern_config_file, local->message);
				g_clear_error (&local);
			}
		} else
			nm_log_dbg (LOGD_CORE, "don't persistate internal configuration (no file set, use --intern-config?)");
	}
	if (new_data)
		_set_config_data (self, new_data);

	g_key_file_unref (keyfile_new);
}

/************************************************************************/

void
nm_config_reload (NMConfig *self)
{
	NMConfigPrivate *priv;
	GError *error = NULL;
	GKeyFile *keyfile, *keyfile_intern;
	NMConfigData *new_data = NULL;
	char *config_main_file = NULL;
	char *config_description = NULL;
	gs_strfreev char **no_auto_default = NULL;
	gboolean intern_config_needs_rewrite;

	g_return_if_fail (NM_IS_CONFIG (self));

	priv = NM_CONFIG_GET_PRIVATE (self);

	/* pass on the original command line options. This means, that
	 * options specified at command line cannot ever be reloaded from
	 * file. That seems desirable.
	 */
	keyfile = read_entire_config (&priv->cli,
	                              priv->config_dir,
	                              priv->system_config_dir,
	                              &config_main_file,
	                              &config_description,
	                              &error);
	if (!keyfile) {
		nm_log_err (LOGD_CORE, "Failed to reload the configuration: %s", error->message);
		g_clear_error (&error);
		return;
	}

	no_auto_default = no_auto_default_from_file (priv->no_auto_default_file);

	keyfile_intern = intern_config_read (priv->intern_config_file, keyfile, &intern_config_needs_rewrite);
	if (intern_config_needs_rewrite)
		intern_config_write (priv->intern_config_file, keyfile_intern, keyfile, NULL);

	new_data = nm_config_data_new (config_main_file, config_description, (const char *const*) no_auto_default, keyfile, keyfile_intern);
	g_free (config_main_file);
	g_free (config_description);
	g_key_file_unref (keyfile);
	if (keyfile_intern)
		g_key_file_unref (keyfile_intern);

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
	case NM_CONFIG_CHANGE_VALUES_USER:
		return "values-user";
	case NM_CONFIG_CHANGE_VALUES_INTERN:
		return "values-intern";
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
	if (NM_FLAGS_HAS (changes, NM_CONFIG_CHANGE_VALUES))
		nm_config_data_log (new_data, "CONFIG: ");
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
	GKeyFile *keyfile, *keyfile_intern;
	char *config_main_file = NULL;
	char *config_description = NULL;
	gs_strfreev char **no_auto_default = NULL;
	gboolean intern_config_needs_rewrite;

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
		priv->config_dir = g_strdup (DEFAULT_CONFIG_DIR);

	if (priv->cli.system_config_dir)
		priv->system_config_dir = g_strdup (priv->cli.system_config_dir);
	else
		priv->system_config_dir = g_strdup (DEFAULT_SYSTEM_CONFIG_DIR);

	if (strcmp (priv->config_dir, priv->system_config_dir) == 0) {
		/* having the same directory twice makes no sense. In that case, clear
		 * @system_config_dir. */
		g_free (priv->system_config_dir);
		priv->system_config_dir = g_strdup ("");
	}

	if (priv->cli.intern_config_file)
		priv->intern_config_file = g_strdup (priv->cli.intern_config_file);
	else
		priv->intern_config_file = g_strdup (DEFAULT_INTERN_CONFIG_FILE);

	keyfile = read_entire_config (&priv->cli,
	                              priv->config_dir,
	                              priv->system_config_dir,
	                              &config_main_file,
	                              &config_description,
	                              error);
	if (!keyfile)
		return FALSE;

	/* Initialize read only private members */

	if (priv->cli.no_auto_default_file)
		priv->no_auto_default_file = g_strdup (priv->cli.no_auto_default_file);
	else
		priv->no_auto_default_file = g_strdup (DEFAULT_NO_AUTO_DEFAULT_FILE);

	priv->plugins = g_key_file_get_string_list (keyfile, NM_CONFIG_KEYFILE_GROUP_MAIN, "plugins", NULL, NULL);
	if (!priv->plugins)
		priv->plugins = g_new0 (char *, 1);

	priv->monitor_connection_files = nm_config_keyfile_get_boolean (keyfile, NM_CONFIG_KEYFILE_GROUP_MAIN, "monitor-connection-files", FALSE);

	priv->auth_polkit = nm_config_keyfile_get_boolean (keyfile, NM_CONFIG_KEYFILE_GROUP_MAIN, "auth-polkit", NM_CONFIG_DEFAULT_AUTH_POLKIT);

	priv->dhcp_client = g_key_file_get_value (keyfile, NM_CONFIG_KEYFILE_GROUP_MAIN, "dhcp", NULL);

	priv->log_level = g_key_file_get_value (keyfile, NM_CONFIG_KEYFILE_GROUP_LOGGING, "level", NULL);
	priv->log_domains = g_key_file_get_value (keyfile, NM_CONFIG_KEYFILE_GROUP_LOGGING, "domains", NULL);

	priv->debug = g_key_file_get_value (keyfile, NM_CONFIG_KEYFILE_GROUP_MAIN, "debug", NULL);

	priv->configure_and_quit = nm_config_keyfile_get_boolean (keyfile, NM_CONFIG_KEYFILE_GROUP_MAIN, "configure-and-quit", FALSE);

	no_auto_default = no_auto_default_from_file (priv->no_auto_default_file);

	keyfile_intern = intern_config_read (priv->intern_config_file, keyfile, &intern_config_needs_rewrite);
	if (intern_config_needs_rewrite)
		intern_config_write (priv->intern_config_file, keyfile_intern, keyfile, NULL);

	priv->config_data_orig = nm_config_data_new (config_main_file, config_description, (const char *const*) no_auto_default, keyfile, keyfile_intern);

	priv->config_data = g_object_ref (priv->config_data_orig);

	g_free (config_main_file);
	g_free (config_description);
	g_key_file_unref (keyfile);
	if (keyfile_intern)
		g_key_file_unref (keyfile_intern);
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
	g_free (priv->system_config_dir);
	g_free (priv->no_auto_default_file);
	g_free (priv->intern_config_file);
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

