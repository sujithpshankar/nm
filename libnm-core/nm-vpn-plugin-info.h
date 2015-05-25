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

#ifndef __NM_VPN_PLUGIN_INFO_H__
#define __NM_VPN_PLUGIN_INFO_H__

#include <glib.h>
#include <glib-object.h>
#include <sys/stat.h>

#include "nm-vpn-editor-plugin.h"

G_BEGIN_DECLS

#define NM_TYPE_VPN_PLUGIN_INFO            (nm_vpn_plugin_info_get_type ())
#define NM_VPN_PLUGIN_INFO(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), NM_TYPE_VPN_PLUGIN_INFO, NMVpnPluginInfo))
#define NM_VPN_PLUGIN_INFO_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass), NM_TYPE_VPN_PLUGIN_INFO, NMVpnPluginInfoClass))
#define NM_IS_VPN_PLUGIN_INFO(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), NM_TYPE_VPN_PLUGIN_INFO))
#define NM_IS_VPN_PLUGIN_INFO_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass), NM_TYPE_VPN_PLUGIN_INFO))
#define NM_VPN_PLUGIN_INFO_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj), NM_TYPE_VPN_PLUGIN_INFO, NMVpnPluginInfoClass))

#define NM_VPN_PLUGIN_INFO_NAME        "name"
#define NM_VPN_PLUGIN_INFO_FILENAME    "filename"
#define NM_VPN_PLUGIN_INFO_KEYFILE     "keyfile"

#define NM_VPN_PLUGIN_INFO_KF_GROUP_CONNECTION   "VPN Connection"
#define NM_VPN_PLUGIN_INFO_KF_GROUP_GNOME        "GNOME"

typedef gboolean (*NMVpnPluginInfoCheckFile) (const char *filename, const struct stat *stat, gpointer user_data, GError **error);

typedef struct {
	GObject parent;
} NMVpnPluginInfo;

typedef struct {
	GObjectClass parent;

	/*< private >*/
	gpointer padding[8];
} NMVpnPluginInfoClass;

GType  nm_vpn_plugin_info_get_type       (void);

NMVpnPluginInfo *nm_vpn_plugin_info_new_from_file (const char *filename,
                                                   GError **error);

NMVpnPluginInfo *nm_vpn_plugin_info_new_with_data (const char *filename,
                                                   GKeyFile *keyfile);

const char *nm_vpn_plugin_info_get_name        (NMVpnPluginInfo *self);
const char *nm_vpn_plugin_info_get_filename    (NMVpnPluginInfo *self);
const char *nm_vpn_plugin_info_get_service     (NMVpnPluginInfo *self);
const char *nm_vpn_plugin_info_get_plugin      (NMVpnPluginInfo *self);
const char *nm_vpn_plugin_info_get_program     (NMVpnPluginInfo *self);
const char *nm_vpn_plugin_info_lookup_property (NMVpnPluginInfo *self, const char *group, const char *key);

gboolean nm_vpn_plugin_info_validate_filename (const char *filename);
gboolean nm_vpn_plugin_info_check_file (const char *filename,
                                        gboolean check_absolute,
                                        gboolean do_validate_filename,
                                        int check_owner,
                                        NMVpnPluginInfoCheckFile check_file,
                                        gpointer user_data,
                                        GError **error);

const char *nm_vpn_plugin_info_get_default_dir (void);
GSList *nm_vpn_plugin_info_load_dir (const char *dirname,
                                     gboolean do_validate_filename,
                                     int check_owner,
                                     NMVpnPluginInfoCheckFile check_file,
                                     gpointer user_data);

gboolean         nm_vpn_plugin_info_list_add              (GSList **list, NMVpnPluginInfo *plugin_info, GError **error);
gboolean         nm_vpn_plugin_info_list_remove           (GSList **list, NMVpnPluginInfo *plugin_info);
NMVpnPluginInfo *nm_vpn_plugin_info_list_find_by_name     (GSList *list, const char *name);
NMVpnPluginInfo *nm_vpn_plugin_info_list_find_by_filename (GSList *list, const char *filename);
NMVpnPluginInfo *nm_vpn_plugin_info_list_find_by_service  (GSList *list, const char *service);


NMVpnEditorPlugin *nm_vpn_plugin_info_get_editor_plugin   (NMVpnPluginInfo *plugin_info);
void               nm_vpn_plugin_info_set_editor_plugin    (NMVpnPluginInfo *self, NMVpnEditorPlugin *plugin);
NMVpnEditorPlugin *nm_vpn_plugin_info_load_editor_plugin  (NMVpnPluginInfo *plugin_info,
                                                           gboolean force_retry,
                                                           int check_owner,
                                                           NMVpnPluginInfoCheckFile check_file,
                                                           gpointer user_data,
                                                           GError **error);


G_END_DECLS

#endif /* __NM_VPN_PLUGIN_INFO_H__ */
