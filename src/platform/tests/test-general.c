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

#include "nm-platform-utils.h"

#include <linux/rtnetlink.h>

#include "nm-linux-platform.h"
#include "nm-logging.h"

#include "nm-test-utils.h"


/******************************************************************/

static void
test_nmp_utils_ip_route_scope_native_to_nm ()
{
	int i;

		/* see also /etc/iproute2/rt_scopes */
	G_STATIC_ASSERT (((guint8) RT_SCOPE_LINK) == 253);
	G_STATIC_ASSERT (((guint8) RT_SCOPE_NOWHERE) == 255);

	for (i = 0; i <= 255; i++) {
		guint8 scope = i;

		g_assert_cmpint ((int) scope, ==, i);

		g_assert_cmpint (nmp_utils_ip_route_scope_nm_to_native (nmp_utils_ip_route_scope_native_to_nm (scope)), ==, scope);
		g_assert_cmpint (nmp_utils_ip_route_scope_native_to_nm (nmp_utils_ip_route_scope_nm_to_native (scope)), ==, scope);

		if (scope == 0)
			g_assert_cmpint (scope, ==, nmp_utils_ip_route_scope_native_to_nm (RT_SCOPE_NOWHERE));
		else if (scope == RT_SCOPE_NOWHERE)
			g_assert_cmpint (scope, ==, nmp_utils_ip_route_scope_nm_to_native (0));
	}
}

/******************************************************************/

static void
test_init_linux_platform ()
{
	gs_unref_object NMPlatform *platform = NULL;

	platform = g_object_new (NM_TYPE_LINUX_PLATFORM, NULL);
}

/******************************************************************/

NMTST_DEFINE ();

int
main (int argc, char **argv)
{
	nmtst_init_assert_logging (&argc, &argv, "INFO", "DEFAULT");

	g_test_add_func ("/general/nmp_utils_ip_route_scope_native_to_nm", test_nmp_utils_ip_route_scope_native_to_nm);
	g_test_add_func ("/general/init_linux_platform", test_init_linux_platform);

	return g_test_run ();
}
