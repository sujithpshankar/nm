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
#include <gio/gio.h>

#include "nm-core-internal.h"

typedef struct {
	char *signal_name;
	const GVariantType *signature;
	GClosure *closure;
} NMDBusSignalData;

static void
free_signal_data (gpointer data, GClosure *closure)
{
	NMDBusSignalData *sd = data;

	g_free (sd->signal_name);
	g_closure_unref (sd->closure);
	g_slice_free (NMDBusSignalData, sd);
}

static void
nm_dbus_signal_handler (GDBusProxy *proxy,
                        const char *sender_name,
                        const char *signal_name,
                        GVariant   *parameters,
                        gpointer    user_data)
{
	NMDBusSignalData *sd = user_data;
	GValue *closure_params;
	gsize n_params, i;
	GVariant *param;

	if (strcmp (signal_name, sd->signal_name) != 0)
		return;

	if (sd->signature) {
		if (!g_variant_is_of_type (parameters, sd->signature)) {
			g_warning ("%p: got signal '%s' but parameters were of type '%s', not '%s'",
			           proxy, signal_name, g_variant_get_type_string (parameters),
			           g_variant_type_peek_string (sd->signature));
			return;
		}

		n_params = g_variant_n_children (parameters) + 1;
	} else
		n_params = 1;

	closure_params = g_new0 (GValue, n_params);
	g_value_init (&closure_params[0], G_TYPE_OBJECT);
	g_value_set_object (&closure_params[0], proxy);

	for (i = 1; i < n_params; i++) {
		param = g_variant_get_child_value (parameters, i - 1);
		if (   g_variant_is_of_type (param, G_VARIANT_TYPE ("ay"))
		    || g_variant_is_of_type (param, G_VARIANT_TYPE ("aay"))) {
			/* g_dbus_gvariant_to_gvalue() thinks 'ay' means "non-UTF-8 NUL-terminated string" */
			g_value_init (&closure_params[i], G_TYPE_VARIANT);
			g_value_set_variant (&closure_params[i], param);
		} else
			g_dbus_gvariant_to_gvalue (param, &closure_params[i]);
		g_variant_unref (param);
	}

	g_closure_invoke (sd->closure,
	                  NULL,
	                  n_params,
	                  closure_params,
	                  g_signal_get_invocation_hint (proxy));

	for (i = 0; i < n_params; i++)
		g_value_unset (&closure_params[i]);
	g_free (closure_params);
}

/**
 * _nm_dbus_signal_connect_data:
 * @proxy: a #GDBusProxy
 * @signal_name: the D-Bus signal to connect to
 * @signature: (allow-none): the signal's type signature (must be a tuple)
 * @c_handler: the signal handler function
 * @data: (allow-none): data to pass to @c_handler
 * @destroy_data: (allow-none): closure destroy notify for @data
 * @connect_flags: connection flags
 *
 * Connects to the D-Bus signal @signal_name on @proxy. @c_handler must be a
 * void function whose first argument is a #GDBusProxy, followed by arguments
 * for each element of @signature, ending with a #gpointer argument for @data.
 *
 * The argument types in @c_handler correspond to the types output by
 * g_dbus_gvariant_to_gvalue(), except for 'ay' and 'aay'. In particular:
 * - both 16-bit and 32-bit integers are passed as #gint/#guint
 * - 'as' values are passed as #GStrv (char **)
 * - all other array, tuple, and dict types are passed as #GVariant
 *
 * If @signature is %NULL, then the signal's parameters will be ignored, and
 * @c_handler should take only the #GDBusProxy and #gpointer arguments.
 *
 * Returns: the signal handler ID, which can be used with
 *   g_signal_handler_remove(). Note that other functions like
 *   g_signal_handlers_remove_matched() cannot be used with handlers connected
 *   via this function, since @c_handler and @data are not the actual handler
 *   and data passed to g_signal_connect_data().
 */
gulong
_nm_dbus_signal_connect_data (GDBusProxy *proxy,
                              const char *signal_name,
                              const GVariantType *signature,
                              GCallback c_handler,
                              gpointer data,
                              GClosureNotify destroy_data,
                              GConnectFlags connect_flags)
{
	NMDBusSignalData *sd;

	g_return_val_if_fail (G_IS_DBUS_PROXY (proxy), 0);
	g_return_val_if_fail (signal_name != NULL, 0);
	g_return_val_if_fail (signature == NULL || g_variant_type_is_tuple (signature), 0);
	g_return_val_if_fail (c_handler != NULL, 0);

	sd = g_slice_new (NMDBusSignalData);
	sd->signal_name = g_strdup (signal_name);
	sd->signature = signature;
	sd->closure = g_cclosure_new (c_handler, data, destroy_data);
	g_closure_sink (g_closure_ref (sd->closure));
	g_closure_set_marshal (sd->closure, g_cclosure_marshal_generic);

	return g_signal_connect_data (proxy, "g-signal",
	                              G_CALLBACK (nm_dbus_signal_handler), sd,
	                              free_signal_data, connect_flags);
}	

/**
 * _nm_dbus_signal_connect:
 * @proxy: a #GDBusProxy
 * @signal_name: the D-Bus signal to connect to
 * @signature: the signal's type signature (must be a tuple)
 * @c_handler: the signal handler function
 * @data: (allow-none): data to pass to @c_handler
 *
 * Simplified version of _nm_dbus_signal_connect_data() with fewer arguments.
 *
 * Returns: the signal handler ID, as with _nm_signal_connect_data().
 */
