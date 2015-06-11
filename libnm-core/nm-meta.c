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

#include <string.h>
#include <glib/gi18n-lib.h>

#include "gsystem-local-alloc.h"

#include "nm-meta.h"
#include "nm-core-internal.h"
#include "nm-setting-wired.h"
#include "nm-macros-internal.h"

static const NMMetaFlag _nm_meta_flags_wake_on_lan[] = {
	{ "phy",            NM_SETTING_WIRED_WAKE_ON_LAN_PHY },
	{ "unicast",        NM_SETTING_WIRED_WAKE_ON_LAN_UNICAST },
	{ "multicast",      NM_SETTING_WIRED_WAKE_ON_LAN_MULTICAST },
	{ "broadcast",      NM_SETTING_WIRED_WAKE_ON_LAN_BROADCAST },
	{ "arp",            NM_SETTING_WIRED_WAKE_ON_LAN_ARP },
	{ "magic",          NM_SETTING_WIRED_WAKE_ON_LAN_MAGIC },
	{ }
};

const NMMetaFlag *nm_setting_wired_wake_on_lan_get_meta_flags (void)
{
	return _nm_meta_flags_wake_on_lan;
}

/**
 * nm_meta_flag_to_str
 * @flags: a %NMMetaFlag array to be used for the conversion
 * @value: the value to be translated
 *
 * Converts a number representing the logical OR of multiple flags
 * to a textual description using the @flags conversion table.
 *
 * Returns: a newly allocated string
 *
 * Since: 1.2
 */
char *nm_meta_flag_to_str (const NMMetaFlag *flags, int value)
{
	GString *str;
	const NMMetaFlag *ptr;

	g_return_val_if_fail (flags, strdup (""));

	str = g_string_new (NULL);

	for (ptr = flags; ptr->name; ptr++) {
		if (NM_FLAGS_HAS (value, ptr->value)) {
			g_string_append (str, ptr->name);
			g_string_append (str, ",");
		}
	}

	if (str->len > 0)
		g_string_truncate (str, str->len - 1);

	return g_string_free (str, FALSE);
}

/**
 * nm_meta_flag_from_str
 * @flags: a %NMMetaFlag array to be used for the conversion
 * @str: the input string
 * @out_value: (out) (allow-none) the output value
 * @err_token: (out) (allow-none) location to store the first unrecognized token
 *
 * Parses the flags contained in a string.
 *
 * Returns: %TRUE if the conversion was successful, %FALSE otherwise
 *
 * Since: 1.2
 */
gboolean nm_meta_flag_from_str (const NMMetaFlag *flags, const char *str,
                                int *out_value, char **err_token)
{
	const NMMetaFlag *ptr;
	int res = 0;
	gs_strfreev char **strv = NULL;
	char **iter;
	gboolean found;

	g_return_val_if_fail (str, FALSE);
	g_return_val_if_fail (flags, FALSE);

	if (out_value)
		*out_value = 0;

	strv = g_strsplit_set (str, " \t,", 0);

	for (iter = strv; iter && *iter; iter++) {
		if (!*iter[0])
			continue;
		for (ptr = flags, found = FALSE; ptr->name; ptr++) {
			if (!strcmp (*iter, ptr->name)) {
				res |= ptr->value;
				found = TRUE;
				break;
			}
		}
		if (!found) {
			if (err_token)
				*err_token = g_strdup (*iter);
			return FALSE;
		}
	}

	if (out_value)
		*out_value = res;
	if (err_token)
		*err_token = NULL;
	return TRUE;
}
