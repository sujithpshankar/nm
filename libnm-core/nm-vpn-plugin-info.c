/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/*
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA.
 *
 * Copyright 2015 Red Hat, Inc.
 */

#include "config.h"

#include "nm-vpn-plugin-info.h"

#include <gio/gio.h>
#include <glib/gi18n-lib.h>
#include <string.h>
#include <errno.h>

#include "gsystem-local-alloc.h"
#include "nm-errors.h"
#include "nm-utils-internal.h"
#include "nm-core-internal.h"

#define DEFAULT_DIR     NMCONFDIR"/VPN"

enum {
	PROP_0,
	PROP_NAME,
	PROP_FILENAME,
	PROP_KEYFILE,

	LAST_PROP,
};

typedef struct {
	char *filename;
	char *name;
	GKeyFile *keyfile;

	/* It is convenient for nm_vpn_plugin_info_lookup_property() to return a const char *,
	 * contrary to what g_key_file_get_string() does. Hence we must cache the returned
	 * value somewhere... let's put it in an internal hash table.
	 * This contains a clone of all the strings in keyfile. */
	GHashTable *keys;
} NMVpnPluginInfoPrivate;

static void nm_vpn_plugin_info_initable_iface_init (GInitableIface *iface);

G_DEFINE_TYPE_WITH_CODE (NMVpnPluginInfo, nm_vpn_plugin_info, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE, nm_vpn_plugin_info_initable_iface_init);
                         )

#define NM_VPN_PLUGIN_INFO_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), NM_TYPE_VPN_PLUGIN_INFO, NMVpnPluginInfoPrivate))

/*********************************************************************/

gboolean
nm_vpn_plugin_info_validate_filename (const char *filename)
{
	if (!filename || !g_str_has_suffix (filename, ".name"))
		return FALSE;

	/* originally, we didn't do further checks... but here we go. */
	if (filename[0] == '.') {
		/* this also rejects name ".name" alone. */
		return FALSE;
	}
	return TRUE;
}

static gboolean
_check_file (const char *filename,
             int check_owner,
             NMVpnPluginInfoCheckFile check_file,
             gpointer user_data,
             struct stat *out_st,
             GError **error)
{
	struct stat st_backup;

	if (!out_st)
		out_st = &st_backup;

	if (stat (filename, out_st) != 0) {
		int errsv = errno;

		g_set_error (error,
		             NM_VPN_PLUGIN_ERROR,
		             NM_VPN_PLUGIN_ERROR_FAILED,
		             _("failed stat file %s: %s"), filename, strerror (errsv));
		return FALSE;
	}

	/* ignore non-files. */
	if (!S_ISREG (out_st->st_mode)) {
		g_set_error (error,
		             NM_VPN_PLUGIN_ERROR,
		             NM_VPN_PLUGIN_ERROR_FAILED,
		             _("not a file (%s)"), filename);
		return FALSE;
	}

	/* with check_owner enabled, check that the file belongs to the
	 * owner or root. */
	if (   check_owner >= 0
	    && (out_st->st_uid != 0 && out_st->st_uid != check_owner)) {
		g_set_error (error,
		             NM_VPN_PLUGIN_ERROR,
		             NM_VPN_PLUGIN_ERROR_FAILED,
		             _("invalid file owner %d for %s"), out_st->st_uid, filename);
		return FALSE;
	}

	/* with check_owner enabled, check that the file cannot be modified
	 * by other users (except root). */
	if (   check_owner >= 0
	    && NM_FLAGS_ANY (out_st->st_mode, S_IWGRP | S_IWOTH | S_ISUID)) {
		g_set_error (error,
		             NM_VPN_PLUGIN_ERROR,
		             NM_VPN_PLUGIN_ERROR_FAILED,
		             _("file permissions for %s"), filename);
		return FALSE;
	}

	if (    check_file
	    && !check_file (filename, out_st, user_data, error)) {
		if (error && !*error) {
			g_set_error (error,
			             NM_VPN_PLUGIN_ERROR,
			             NM_VPN_PLUGIN_ERROR_FAILED,
			             _("reject %s"), filename);
		}
		return FALSE;
	}

	return TRUE;
}

static gboolean
nm_vpn_plugin_info_check_file_full (const char *filename,
                                    gboolean check_absolute,
                                    gboolean do_validate_filename,
                                    int check_owner,
                                    NMVpnPluginInfoCheckFile check_file,
                                    gpointer user_data,
                                    struct stat *out_st,
                                    GError **error)
{
	if (!filename || !*filename) {
		g_set_error (error,
		             NM_VPN_PLUGIN_ERROR,
		             NM_VPN_PLUGIN_ERROR_FAILED,
		             _("missing filename"));
		return FALSE;
	}

	if (check_absolute && !g_path_is_absolute (filename)) {
		g_set_error (error,
		             NM_VPN_PLUGIN_ERROR,
		             NM_VPN_PLUGIN_ERROR_FAILED,
		             _("filename must be an absolute path (%s)"), filename);
		return FALSE;
	}

	if (   do_validate_filename
	    && !nm_vpn_plugin_info_validate_filename (filename)) {
		g_set_error (error,
		             NM_VPN_PLUGIN_ERROR,
		             NM_VPN_PLUGIN_ERROR_FAILED,
		             _("filename has invalid format (%s)"), filename);
		return FALSE;
	}

	return _check_file (filename,
	                    check_owner,
	                    check_file,
	                    user_data,
	                    out_st,
	                    error);
}

gboolean
nm_vpn_plugin_info_check_file (const char *filename,
                               gboolean check_absolute,
                               gboolean do_validate_filename,
                               int check_owner,
                               NMVpnPluginInfoCheckFile check_file,
                               gpointer user_data,
                               GError **error)
{
	return nm_vpn_plugin_info_check_file_full (filename, check_absolute, do_validate_filename, check_owner, check_file, user_data, NULL, error);
}

typedef struct {
	char *filename;
	NMVpnPluginInfo *plugin_info;
	struct stat stat;
} LoadDirInfo;

static int
_sort_files (LoadDirInfo *a, LoadDirInfo *b)
{
	time_t ta, tb;

	ta = MAX (a->stat.st_mtime, a->stat.st_ctime);
	tb = MAX (b->stat.st_mtime, b->stat.st_ctime);
	if (ta < tb)
		return 1;
	if (ta > tb)
		return -1;
	return g_strcmp0 (a->filename, b->filename);
}

const char *
nm_vpn_plugin_info_get_default_dir ()
{
	static const char *dir = NULL;

	if (G_UNLIKELY (!dir)) {
		dir = g_getenv ("NM_VPN_PLUGIN_DIR");
		if (!dir || !*dir)
			dir = DEFAULT_DIR;
	}
	return dir;
}

/**
 * nm_vpn_plugin_info_load_dir:
 * @dirname:
 * @do_validate_filename: only consider filenames that have a certain
 *   pattern (i.e. end with ".name").
 * @check_owner: if set to a non-negative number, check that the file
 *   owner is either the same uid or 0. In that case, also check
 *   that the file is not writable by group or other.
 * @check_file: (allow-none): callback to check whether the file is valid.
 * @user_data: data for @check_file
 *
 * Returns: (transfer-full): list of loaded plugin infos.
 */
GSList *
nm_vpn_plugin_info_load_dir (const char *dirname,
                             gboolean do_validate_filename,
                             int check_owner,
                             NMVpnPluginInfoCheckFile check_file,
                             gpointer user_data)
{
	GDir *dir;
	const char *fn;
	GArray *array;
	GSList *res = NULL;
	guint i;

	if (!dirname)
		dirname = nm_vpn_plugin_info_get_default_dir ();

	dir = g_dir_open (dirname, 0, NULL);
	if (!dir)
		return NULL;

	array = g_array_new (FALSE, FALSE, sizeof (LoadDirInfo));

	while ((fn = g_dir_read_name (dir))) {
		LoadDirInfo info = {
		    .filename = g_build_filename (dirname, fn, NULL),
		};

		if (nm_vpn_plugin_info_check_file_full (info.filename,
		                                        FALSE,
		                                        do_validate_filename,
		                                        check_owner,
		                                        check_file,
		                                        user_data,
		                                        &info.stat,
		                                        NULL)) {
			info.plugin_info = nm_vpn_plugin_info_new_from_file (info.filename, NULL);
			if (info.plugin_info) {
				g_array_append_val (array, info);
				continue;
			}
		}
		g_free (info.filename);
	}
	g_dir_close (dir);

	/* sort the files so that we have a stable behavior. The directory might contain
	 * duplicate VPNs, so while nm_vpn_plugin_info_load_dir() would load them all, the
	 * caller probably wants to reject duplicates. Having a stable order means we always
	 * reject the same files in face of duplicates. */
	g_array_sort (array, (GCompareFunc) _sort_files);

	for (i = 0; i < array->len; i++) {
		LoadDirInfo *info = &g_array_index (array, LoadDirInfo, i);

		res = g_slist_prepend (res, info->plugin_info);
		g_free (info->filename);
	}
	g_array_free (array, TRUE);

	return g_slist_reverse (res);
}

/*********************************************************************/

static gboolean
_check_no_conflict (NMVpnPluginInfo *i1, NMVpnPluginInfo *i2, GError **error)
{
	NMVpnPluginInfoPrivate *priv1, *priv2;
	uint i;
	struct {
		const char *group;
		const char *key;
	} check_list[] = {
		{ NM_VPN_PLUGIN_INFO_KF_GROUP_CONNECTION, "service" },
		{ NM_VPN_PLUGIN_INFO_KF_GROUP_CONNECTION, "plugin" },
		{ NM_VPN_PLUGIN_INFO_KF_GROUP_GNOME, "properties" },
	};

	priv1 = NM_VPN_PLUGIN_INFO_GET_PRIVATE (i1);
	priv2 = NM_VPN_PLUGIN_INFO_GET_PRIVATE (i2);

	for (i = 0; i < G_N_ELEMENTS (check_list); i++) {
		gs_free NMUtilsStrStrDictKey *k = NULL;
		const char *s1, *s2;

		k = _nm_utils_strstrdictkey_create (check_list[i].group, check_list[i].key);
		s1 = g_hash_table_lookup (priv1->keys, k);
		if (!s1)
			continue;
		s2 = g_hash_table_lookup (priv2->keys, k);
		if (!s2)
			continue;

		if (strcmp (s1, s2) == 0) {
			g_set_error (error,
			             NM_VPN_PLUGIN_ERROR,
			             NM_VPN_PLUGIN_ERROR_FAILED,
			             _("there exists a conflicting plugin (%s) that has the same %s.%s value"),
			             priv2->name,
			             check_list[i].group, check_list[i].key);
			return FALSE;
		}
	}
	return TRUE;
}

gboolean
nm_vpn_plugin_info_list_add (GSList **list, NMVpnPluginInfo *plugin_info, GError **error)
{
	GSList *iter;
	const char *name;

	g_return_val_if_fail (list, FALSE);
	g_return_val_if_fail (NM_IS_VPN_PLUGIN_INFO (plugin_info), FALSE);

	name = nm_vpn_plugin_info_get_name (plugin_info);
	for (iter = *list; iter; iter = iter->next) {
		if (iter->data == plugin_info)
			return TRUE;

		if (strcmp (nm_vpn_plugin_info_get_name (iter->data), name) == 0) {
			g_set_error (error,
			             NM_VPN_PLUGIN_ERROR,
			             NM_VPN_PLUGIN_ERROR_FAILED,
			             _("there exists a conflicting plugin with the same name (%s)"),
			             name);
			return FALSE;
		}

		/* the plugin must have unique values for certain properties. E.g. two different
		 * plugins cannot share the same D-Bus service name. */
		if (!_check_no_conflict (plugin_info, iter->data, error))
			return FALSE;
	}

	*list = g_slist_append (*list, g_object_ref (plugin_info));
	return TRUE;
}

gboolean
nm_vpn_plugin_info_list_remove (GSList **list, NMVpnPluginInfo *plugin_info)
{
	if (!plugin_info)
		return FALSE;

	g_return_val_if_fail (list, FALSE);
	g_return_val_if_fail (NM_IS_VPN_PLUGIN_INFO (plugin_info), FALSE);

	if (!g_slist_find (*list, plugin_info))
		return FALSE;

	*list = g_slist_remove (*list, plugin_info);
	g_object_unref (plugin_info);
	return TRUE;
}

NMVpnPluginInfo *
nm_vpn_plugin_info_list_find_by_name (GSList *list, const char *name)
{
	GSList *iter;

	if (!name)
		return NULL;

	for (iter = list; iter; iter = iter->next) {
		if (strcmp (nm_vpn_plugin_info_get_name (iter->data), name) == 0)
			return iter->data;
	}
	return NULL;
}

NMVpnPluginInfo *
nm_vpn_plugin_info_list_find_by_filename (GSList *list, const char *filename)
{
	GSList *iter;

	if (!filename)
		return NULL;

	for (iter = list; iter; iter = iter->next) {
		if (g_strcmp0 (nm_vpn_plugin_info_get_filename (iter->data), filename) == 0)
			return iter->data;
	}
	return NULL;
}

NMVpnPluginInfo *
nm_vpn_plugin_info_list_find_by_service (GSList *list, const char *service)
{
	GSList *iter;

	if (!service)
		return NULL;

	for (iter = list; iter; iter = iter->next) {
		if (g_strcmp0 (nm_vpn_plugin_info_get_service (iter->data), service) == 0)
			return iter->data;
	}
	return NULL;
}

/*********************************************************************/

const char *
nm_vpn_plugin_info_get_filename (NMVpnPluginInfo *self)
{
	g_return_val_if_fail (NM_IS_VPN_PLUGIN_INFO (self), NULL);

	return NM_VPN_PLUGIN_INFO_GET_PRIVATE (self)->filename;
}

const char *
nm_vpn_plugin_info_get_name (NMVpnPluginInfo *self)
{
	g_return_val_if_fail (NM_IS_VPN_PLUGIN_INFO (self), NULL);

	return NM_VPN_PLUGIN_INFO_GET_PRIVATE (self)->name;
}

const char *
nm_vpn_plugin_info_get_service (NMVpnPluginInfo *self)
{
	g_return_val_if_fail (NM_IS_VPN_PLUGIN_INFO (self), NULL);

	return g_hash_table_lookup (NM_VPN_PLUGIN_INFO_GET_PRIVATE (self)->keys,
	                            _nm_utils_strstrdictkey_static (NM_VPN_PLUGIN_INFO_KF_GROUP_CONNECTION, "service"));
}

const char *
nm_vpn_plugin_info_get_plugin (NMVpnPluginInfo *self)
{
	g_return_val_if_fail (NM_IS_VPN_PLUGIN_INFO (self), NULL);

	return g_hash_table_lookup (NM_VPN_PLUGIN_INFO_GET_PRIVATE (self)->keys,
	                            _nm_utils_strstrdictkey_static (NM_VPN_PLUGIN_INFO_KF_GROUP_CONNECTION, "plugin"));
}

const char *
nm_vpn_plugin_info_get_program (NMVpnPluginInfo *self)
{
	g_return_val_if_fail (NM_IS_VPN_PLUGIN_INFO (self), NULL);

	return g_hash_table_lookup (NM_VPN_PLUGIN_INFO_GET_PRIVATE (self)->keys,
	                            _nm_utils_strstrdictkey_static (NM_VPN_PLUGIN_INFO_KF_GROUP_CONNECTION, "program"));
}

const char *
nm_vpn_plugin_info_lookup_property (NMVpnPluginInfo *self, const char *group, const char *key)
{
	NMVpnPluginInfoPrivate *priv;
	gs_free NMUtilsStrStrDictKey *k = NULL;

	g_return_val_if_fail (NM_IS_VPN_PLUGIN_INFO (self), NULL);
	g_return_val_if_fail (group, NULL);
	g_return_val_if_fail (key, NULL);

	priv = NM_VPN_PLUGIN_INFO_GET_PRIVATE (self);

	k = _nm_utils_strstrdictkey_create (group, key);
	return g_hash_table_lookup (priv->keys, k);
}

/*********************************************************************/

NMVpnPluginInfo *
nm_vpn_plugin_info_new_from_file (const char *filename,
                                  GError **error)
{
	g_return_val_if_fail (filename, NULL);

	return NM_VPN_PLUGIN_INFO (g_initable_new (NM_TYPE_VPN_PLUGIN_INFO,
	                                           NULL,
	                                           error,
	                                           NM_VPN_PLUGIN_INFO_FILENAME, filename,
	                                           NULL));
}

NMVpnPluginInfo *
nm_vpn_plugin_info_new_with_data (const char *filename,
                                  GKeyFile *keyfile)
{
	g_return_val_if_fail (keyfile, NULL);

	return NM_VPN_PLUGIN_INFO (g_initable_new (NM_TYPE_VPN_PLUGIN_INFO,
	                                           NULL,
	                                           NULL,
	                                           NM_VPN_PLUGIN_INFO_FILENAME, filename,
	                                           NM_VPN_PLUGIN_INFO_KEYFILE, keyfile,
	                                           NULL));
}

/*********************************************************************/

static void
nm_vpn_plugin_info_init (NMVpnPluginInfo *plugin)
{
}

static gboolean
init_sync (GInitable *initable, GCancellable *cancellable, GError **error)
{
	NMVpnPluginInfo *self = NM_VPN_PLUGIN_INFO (initable);
	NMVpnPluginInfoPrivate *priv = NM_VPN_PLUGIN_INFO_GET_PRIVATE (self);
	gs_strfreev char **groups = NULL;
	guint i, j;

	if (!priv->keyfile) {
		if (!priv->filename) {
			g_set_error_literal (error,
			                     NM_VPN_PLUGIN_ERROR,
			                     NM_VPN_PLUGIN_ERROR_BAD_ARGUMENTS,
			                     _("missing filename to load VPN plugin info"));
			return FALSE;
		}
		priv->keyfile = g_key_file_new ();
		if (!g_key_file_load_from_file (priv->keyfile, priv->filename, G_KEY_FILE_NONE, error))
			return FALSE;
	}

	/* we reqire at least a "name" */
	priv->name = g_key_file_get_string (priv->keyfile, NM_VPN_PLUGIN_INFO_KF_GROUP_CONNECTION, "name", NULL);
	if (!priv->name || !priv->name[0]) {
		g_set_error_literal (error, NM_VPN_PLUGIN_ERROR, NM_VPN_PLUGIN_ERROR_BAD_ARGUMENTS,
		                     _("missing name for VPN plugin info"));
		return FALSE;
	}

	priv->keys = g_hash_table_new_full (_nm_utils_strstrdictkey_hash,
	                                    _nm_utils_strstrdictkey_equal,
	                                    g_free, g_free);
	groups = g_key_file_get_groups (priv->keyfile, NULL);
	for (i = 0; groups && groups[i]; i++) {
		gs_strfreev char **keys = NULL;

		keys = g_key_file_get_keys (priv->keyfile, groups[i], NULL, NULL);
		for (j = 0; keys && keys[j]; j++) {
			char *s;

			/* Lookup the value via get_string(). We want that behavior.
			 * You could still lookup the original values via g_key_file_get_value()
			 * based on priv->keyfile. */
			s = g_key_file_get_string (priv->keyfile, groups[i], keys[j], NULL);
			if (s)
				g_hash_table_insert (priv->keys, _nm_utils_strstrdictkey_create (groups[i], keys[j]), s);
		}
	}

	return TRUE;
}

static void
set_property (GObject *object, guint prop_id,
              const GValue *value, GParamSpec *pspec)
{
	NMVpnPluginInfoPrivate *priv = NM_VPN_PLUGIN_INFO_GET_PRIVATE (object);

	switch (prop_id) {
	case PROP_FILENAME:
		priv->filename = g_value_dup_string (value);
		break;
	case PROP_KEYFILE:
		priv->keyfile = g_value_dup_boxed (value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
get_property (GObject *object, guint prop_id,
              GValue *value, GParamSpec *pspec)
{
	NMVpnPluginInfoPrivate *priv = NM_VPN_PLUGIN_INFO_GET_PRIVATE (object);

	switch (prop_id) {
	case PROP_NAME:
		g_value_set_string (value, priv->name);
		break;
	case PROP_FILENAME:
		g_value_set_string (value, priv->filename);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
finalize (GObject *object)
{
	NMVpnPluginInfo *self = NM_VPN_PLUGIN_INFO (object);
	NMVpnPluginInfoPrivate *priv = NM_VPN_PLUGIN_INFO_GET_PRIVATE (self);

	g_free (priv->name);
	g_free (priv->filename);
	g_key_file_unref (priv->keyfile);
	g_hash_table_unref (priv->keys);

	G_OBJECT_CLASS (nm_vpn_plugin_info_parent_class)->finalize (object);
}

static void
nm_vpn_plugin_info_class_init (NMVpnPluginInfoClass *plugin_class)
{
	GObjectClass *object_class = G_OBJECT_CLASS (plugin_class);

	g_type_class_add_private (object_class, sizeof (NMVpnPluginInfoPrivate));

	/* virtual methods */
	object_class->set_property = set_property;
	object_class->get_property = get_property;
	object_class->finalize     = finalize;

	/* properties */

	/**
	 * NMVpnPluginInfo:name:
	 *
	 * The name of the VPN plugin.
	 */
	g_object_class_install_property
	    (object_class, PROP_NAME,
	     g_param_spec_string (NM_VPN_PLUGIN_INFO_NAME, "", "",
	                          NULL,
	                          G_PARAM_READABLE |
	                          G_PARAM_STATIC_STRINGS));

	/**
	 * NMVpnPluginInfo:filename:
	 *
	 * The filename from which the info was loaded.
	 */
	g_object_class_install_property
	    (object_class, PROP_FILENAME,
	     g_param_spec_string (NM_VPN_PLUGIN_INFO_FILENAME, "", "",
	                          NULL,
	                          G_PARAM_READWRITE |
	                          G_PARAM_CONSTRUCT_ONLY |
	                          G_PARAM_STATIC_STRINGS));

	g_object_class_install_property
	    (object_class, PROP_KEYFILE,
	     g_param_spec_boxed (NM_VPN_PLUGIN_INFO_KEYFILE, "", "",
	                         G_TYPE_KEY_FILE,
	                         G_PARAM_WRITABLE |
	                         G_PARAM_CONSTRUCT_ONLY |
	                         G_PARAM_STATIC_STRINGS));
}

static void
nm_vpn_plugin_info_initable_iface_init (GInitableIface *iface)
{
	iface->init = init_sync;
}

