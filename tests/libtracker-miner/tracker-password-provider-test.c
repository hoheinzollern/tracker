/*
 * Copyright (C) 2010, Nokia <ivan.frade@nokia.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

#include "config.h"

#include <stdlib.h>

#include <libtracker-miner/tracker-miner.h>

#define SERVICE_NAME  "TestService"
#define TEST_USERNAME "test-user"
#define TEST_PASSWORD "s3cr3t"

static TrackerPasswordProvider *provider;

static void
test_password_provider_setting (void)
{
	GError *error = NULL;
	gboolean success;

	g_print ("Storing password '%s' for user '%s'\n",
	         TEST_PASSWORD,
	         TEST_USERNAME);

	success = tracker_password_provider_store_password (provider,
	                                                    SERVICE_NAME,
	                                                    "This is the test service",
	                                                    TEST_USERNAME,
	                                                    TEST_PASSWORD,
	                                                    &error);

	g_assert_cmpint (success, ==, TRUE);
}

static void
test_password_provider_getting (void)
{
	gchar *username = NULL;
	gchar *password = NULL;
	gboolean success;
	GError *error = NULL;

	password = tracker_password_provider_get_password (provider,
	                                                   SERVICE_NAME,
	                                                   &username,
	                                                   &error);

	g_assert_cmpstr (username, ==, TEST_USERNAME);
	g_assert_cmpstr (password, ==, TEST_PASSWORD);

	g_print ("Found password is '%s' for username '%s'\n", 
	         password,
	         username);

	g_free (username);

	success = tracker_password_provider_unlock_password (password);
	g_assert_cmpint (success, ==, TRUE);

	/* Also test without getting the username */
	password = tracker_password_provider_get_password (provider,
	                                                   SERVICE_NAME,
	                                                   NULL,
	                                                   &error);

	g_assert_cmpstr (password, ==, TEST_PASSWORD);

	g_print ("Found password is '%s' for NULL username\n", password);

	success = tracker_password_provider_unlock_password (password);
	g_assert_cmpint (success, ==, TRUE);
}

static gboolean
test_log_failure_cb (const gchar    *log_domain,
                     GLogLevelFlags  log_level,
                     const gchar    *message,
                     gpointer        user_data)
{
	/* Don't abort, we expect failure */
	return FALSE;
}

int 
main (int argc, char **argv)
{
	const gchar *current_dir;
	gint retval;

	g_type_init ();

	g_test_init (&argc, &argv, NULL);

	/* Set test environment up */
	current_dir = g_get_current_dir ();
	g_setenv ("XDG_CONFIG_HOME", current_dir, TRUE);

	g_test_add_func ("/libtracker-miner/tracker-password-provider/setting",
	                 test_password_provider_setting);
	g_test_add_func ("/libtracker-miner/tracker-password-provider/getting",
	                 test_password_provider_getting);

	g_test_log_set_fatal_handler (test_log_failure_cb, NULL);
	provider = tracker_password_provider_get ();
	g_print ("Not aborting here because we expect no filename to exist yet\n");
	g_assert (provider);

	/* g_object_unref (provider); */

	retval = g_test_run ();

        /* clean up */
        g_print ("Removing temporary data\n");
        g_spawn_command_line_sync ("rm -R tracker/", NULL, NULL, NULL, NULL);

        return retval;
}
