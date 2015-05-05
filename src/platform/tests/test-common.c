#include "config.h"

#include "test-common.h"

#include "nm-test-utils.h"


gboolean
nmtst_platform_is_root_test ()
{
	NM_PRAGMA_WARNING_DISABLE("-Wtautological-compare")
	return (SETUP == nm_linux_platform_setup);
	NM_PRAGMA_WARNING_REENABLE
}

SignalData *
add_signal_full (const char *name, NMPlatformSignalChangeType change_type, GCallback callback, int ifindex, const char *ifname)
{
	SignalData *data = g_new0 (SignalData, 1);

	data->name = name;
	data->change_type = change_type;
	data->received_count = 0;
	data->handler_id = g_signal_connect (nm_platform_get (), name, callback, data);
	data->ifindex = ifindex;
	data->ifname = ifname;

	g_assert (data->handler_id >= 0);

	return data;
}

static const char *
_change_type_to_string (NMPlatformSignalChangeType change_type)
{
    switch (change_type) {
    case NM_PLATFORM_SIGNAL_ADDED:
        return "added";
    case NM_PLATFORM_SIGNAL_CHANGED:
        return "changed";
    case NM_PLATFORM_SIGNAL_REMOVED:
        return "removed";
    default:
        g_return_val_if_reached ("UNKNOWN");
    }
}

void
accept_signal (SignalData *data)
{
	debug ("Accepting signal '%s-%s' ifindex %d ifname %s.", data->name, _change_type_to_string (data->change_type), data->ifindex, data->ifname);
	if (data->received_count == 0)
		g_error ("Attemted to accept a non-received signal '%s-%s'.", data->name, _change_type_to_string (data->change_type));
	if (data->received_count != 1)
		g_error ("Signal already received %d times: '%s-%s'.", data->received_count, data->name, _change_type_to_string (data->change_type));

	data->received_count = 0;
}

void
accept_signals (SignalData *data, int min, int max)
{
	if (data->received_count < min || data->received_count > max)
		g_error ("Expect [%d,%d] signals, but %s signals queued -- '%s-%s' ifindex %d ifname %s.", min, max, data->received_count, data->name, _change_type_to_string (data->change_type), data->ifindex, data->ifname);
	data->received_count = 0;
}

void
ensure_no_signal (SignalData *data)
{
	if (data->received_count > 0)
		g_error ("Unepexted signal '%s-%s'.", data->name, _change_type_to_string (data->change_type));
}

void
wait_signal (SignalData *data)
{
	if (data->received_count)
		g_error ("Signal '%s' received before waiting for it.", data->name);

	data->loop = g_main_loop_new (NULL, FALSE);
	g_main_loop_run (data->loop);
	g_clear_pointer (&data->loop, g_main_loop_unref);

	accept_signal (data);
}

void
free_signal (SignalData *data)
{
	if (data->received_count != 0)
		g_error ("Attempted to free received but not accepted signal '%s-%s'.", data->name, _change_type_to_string (data->change_type));

	g_signal_handler_disconnect (nm_platform_get (), data->handler_id);
	g_free (data);
}

void
link_callback (NMPlatform *platform, int ifindex, NMPlatformLink *received, NMPlatformSignalChangeType change_type, NMPlatformReason reason, SignalData *data)
{
	
	GArray *links;
	NMPlatformLink *cached;
	int i;

	g_assert (received);
	g_assert_cmpint (received->ifindex, ==, ifindex);
	g_assert (data && data->name);
	g_assert_cmpstr (data->name, ==, NM_PLATFORM_SIGNAL_LINK_CHANGED);

	if (data->ifindex && data->ifindex != received->ifindex)
		return;
	if (data->ifname && g_strcmp0 (data->ifname, nm_platform_link_get_name (NM_PLATFORM_GET, ifindex)) != 0)
		return;
	if (change_type != data->change_type)
		return;

	if (data->loop) {
		debug ("Quitting main loop.");
		g_main_loop_quit (data->loop);
	}

	data->received_count++;
	debug ("Received signal '%s-%s' ifindex %d ifname '%s' %dth time.", data->name, _change_type_to_string (data->change_type), ifindex, received->name, data->received_count);

	if (change_type == NM_PLATFORM_SIGNAL_REMOVED)
		g_assert (!nm_platform_link_get_name (NM_PLATFORM_GET, ifindex));
	else
		g_assert (nm_platform_link_get_name (NM_PLATFORM_GET, ifindex));

	/* Check the data */
	g_assert (received->ifindex > 0);
	links = nm_platform_link_get_all (NM_PLATFORM_GET);
	for (i = 0; i < links->len; i++) {
		cached = &g_array_index (links, NMPlatformLink, i);
		if (cached->ifindex == received->ifindex) {
			g_assert_cmpint (nm_platform_link_cmp (cached, received), ==, 0);
			g_assert (!memcmp (cached, received, sizeof (*cached)));
			if (data->change_type == NM_PLATFORM_SIGNAL_REMOVED)
				g_error ("Deleted link still found in the local cache.");
			g_array_unref (links);
			return;
		}
	}
	g_array_unref (links);

	if (data->change_type != NM_PLATFORM_SIGNAL_REMOVED)
		g_error ("Added/changed link not found in the local cache.");
}

gboolean
ip4_route_exists (const char *ifname, guint32 network, int plen, guint32 metric)
{
	gs_free char *arg_network = NULL;
	const char *argv[] = {
		NULL,
		"route",
		"list",
		"dev",
		ifname,
		"exact",
		NULL,
		NULL,
	};
	int exit_status;
	gs_free char *std_out = NULL, *std_err = NULL;
	char *out;
	gboolean success;
	gs_free_error GError *error = NULL;
	gs_free char *metric_pattern = NULL;

	g_assert (ifname && nm_utils_iface_valid_name (ifname));
	g_assert (!strstr (ifname, " metric "));
	g_assert (plen >= 0 && plen <= 32);

	if (!NM_IS_LINUX_PLATFORM (nm_platform_get ())) {
		/* If we don't test against linux-platform, we don't actually configure any
		 * routes in the system. */
		return -1;
	}

	argv[0] = nm_utils_file_search_in_paths ("ip", NULL,
	                                         (const char *[]) { "/sbin", "/usr/sbin", NULL },
	                                         G_FILE_TEST_IS_EXECUTABLE, NULL, NULL, NULL);
	argv[6] = arg_network = g_strdup_printf ("%s/%d", nm_utils_inet4_ntop (network, NULL), plen);

	if (!argv[0]) {
		/* Hm. There is no 'ip' binary. Return *unknown* */
		return -1;
	}

	success = g_spawn_sync (NULL,
	                        (char **) argv,
	                        (char *[]) { NULL },
	                        0,
	                        NULL,
	                        NULL,
	                        &std_out,
	                        &std_err,
	                        &exit_status,
	                        &error);
	g_assert_no_error (error);
	g_assert (success);
	g_assert_cmpstr (std_err, ==, "");
	g_assert (std_out);

	metric_pattern = g_strdup_printf (" metric %u", metric);
	out = std_out;
	while (out) {
		char *eol = strchr (out, '\n');
		gs_free char *line = eol ? g_strndup (out, eol - out) : g_strdup (out);
		const char *p;

		out = eol ? &eol[1] : NULL;
		if (!line[0])
			continue;

		if (metric == 0) {
			if (!strstr (line, " metric "))
				return TRUE;
		}
		p = strstr (line, metric_pattern);
		if (p && NM_IN_SET (p[strlen (metric_pattern)], ' ', '\0'))
			return TRUE;
	}
	return FALSE;
}

void
_assert_ip4_route_exists (const char *file, guint line, const char *func, gboolean exists, const char *ifname, guint32 network, int plen, guint32 metric)
{
	int ifindex;
	gboolean exists_checked;

	/* Check for existance of the route by spawning iproute2. Do this because platform
	 * code might be entirely borked, but we expect ip-route to give a correct result.
	 * If the ip command cannot be found, we accept this as success. */
	exists_checked = ip4_route_exists (ifname, network, plen, metric);
	if (exists_checked != -1 && !exists_checked != !exists) {
		g_error ("[%s:%u] %s(): We expect the ip4 route %s/%d metric %u %s, but it %s",
		         file, line, func,
		         nm_utils_inet4_ntop (network, NULL), plen, metric,
		         exists ? "to exist" : "not to exist",
		         exists ? "doesn't" : "does");
	}

	ifindex = nm_platform_link_get_ifindex (NM_PLATFORM_GET, ifname);
	g_assert (ifindex > 0);
	if (!nm_platform_ip4_route_exists (NM_PLATFORM_GET, ifindex, network, plen, metric) != !exists) {
		g_error ("[%s:%u] %s(): The ip4 route %s/%d metric %u %s, but platform thinks %s",
		         file, line, func,
		         nm_utils_inet4_ntop (network, NULL), plen, metric,
		         exists ? "exists" : "does not exist",
		         exists ? "it doesn't" : "it does");
	}
}

void
run_command (const char *format, ...)
{
	char *command;
	va_list ap;

	va_start (ap, format);
	command = g_strdup_vprintf (format, ap);
	va_end (ap);
	debug ("Running command: %s", command);
	g_assert (!system (command));
	debug ("Command finished.");
	g_free (command);
}

NMTST_DEFINE();

int
main (int argc, char **argv)
{
	int result;
	const char *program = *argv;

	init_tests (&argc, &argv);

	if (nmtst_platform_is_root_test ()  && getuid() != 0) {
		/* Try to exec as sudo, this function does not return, if a sudo-cmd is set. */
		nmtst_reexec_sudo ();

#ifdef REQUIRE_ROOT_TESTS
		g_print ("Fail test: requires root privileges (%s)\n", program);
		return EXIT_FAILURE;
#else
		g_print ("Skipping test: requires root privileges (%s)\n", program);
		return EXIT_SKIP;
#endif
	}

	SETUP ();

	setup_tests ();

	result = g_test_run ();

	nm_platform_link_delete (NM_PLATFORM_GET, nm_platform_link_get_ifindex (NM_PLATFORM_GET, DEVICE_NAME));

	g_object_unref (nm_platform_get ());
	return result;
}
