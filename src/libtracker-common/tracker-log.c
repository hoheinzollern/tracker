/*
 * Copyright (C) 2008, Nokia <ivan.frade@nokia.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.          See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */

#include "config.h"

#include <sys/time.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include <glib/gstdio.h>

#include "tracker-log.h"
#include "tracker-file-utils.h"

static gboolean  initialized;
static FILE     *fd;
static gint      verbosity;
static guint     log_handler_id;

#if GLIB_CHECK_VERSION (2,31,0)
static GMutex    mutex;
#else
static GMutex   *mutex;
#endif

static inline void
log_output (const gchar    *domain,
            GLogLevelFlags  log_level,
            const gchar    *message)
{
	time_t        now;
	gchar         time_str[64];
	gchar        *output;
	struct tm    *local_time;
	const gchar  *log_level_str;
	static gsize  size = 0;

	g_return_if_fail (initialized == TRUE);
	g_return_if_fail (message != NULL && message[0] != '\0');

	/* Ensure file logging is thread safe */
#if GLIB_CHECK_VERSION (2,31,0)
	g_mutex_lock (&mutex);
#else
	g_mutex_lock (mutex);
#endif

	/* Check log size, 10MiB limit */
	if (size > (10 << 20) && fd) {
		rewind (fd);

		if (ftruncate (fileno (fd), 0) != 0) {
			/* FIXME: What should we do if this fails? */
		}

		size = 0;
	}

	now = time ((time_t *) NULL);
	local_time = localtime (&now);
	strftime (time_str, 64, "%d %b %Y, %H:%M:%S:", local_time);

	switch (log_level) {
	case G_LOG_LEVEL_WARNING:
		log_level_str = "-Warning **";
		break;

	case G_LOG_LEVEL_CRITICAL:
		log_level_str = "-Critical **";
		break;

	case G_LOG_LEVEL_ERROR:
		log_level_str = "-Error **";
		break;
	case G_LOG_FLAG_RECURSION:
	case G_LOG_FLAG_FATAL:
	case G_LOG_LEVEL_MESSAGE:
	case G_LOG_LEVEL_INFO:
	case G_LOG_LEVEL_DEBUG:
	case G_LOG_LEVEL_MASK:
	default:
		log_level_str = NULL;
		break;
	}

	output = g_strdup_printf ("%s%s %s%s: %s",
	                          log_level_str ? "\n" : "",
	                          time_str,
	                          domain,
	                          log_level_str ? log_level_str : "",
	                          message);

	if (G_UNLIKELY (fd == NULL)) {
		g_fprintf (stderr, "%s\n", output);
		fflush (stderr);
	} else {
		size += g_fprintf (fd, "%s\n", output);
		fflush (fd);
	}

	g_free (output);

#if GLIB_CHECK_VERSION (2,31,0)
	g_mutex_unlock (&mutex);
#else
	g_mutex_unlock (mutex);
#endif
}

static void
tracker_log_handler (const gchar    *domain,
                     GLogLevelFlags  log_level,
                     const gchar    *message,
                     gpointer        user_data)
{
	log_output (domain, log_level, message);

	/* Now show the message through stdout/stderr as usual */
	g_log_default_handler (domain, log_level, message, user_data);
}

static void
hide_log_handler (const gchar    *domain,
                  GLogLevelFlags  log_level,
                  const gchar    *message,
                  gpointer        user_data)
{
	/* do nothing */
}

gboolean
tracker_log_init (gint    this_verbosity,
                  gchar **used_filename)
{
	gchar *filename;
	gchar *basename;
	const gchar *env_verbosity;
	GLogLevelFlags hide_levels = 0;

	if (initialized) {
		return TRUE;
	}

	env_verbosity = g_getenv ("TRACKER_VERBOSITY");
	if (env_verbosity != NULL) {
		this_verbosity = atoi (env_verbosity);
	} else {
		gchar *verbosity_string;

		/* make sure libtracker-sparql uses the same verbosity setting */

		verbosity_string = g_strdup_printf ("%d", this_verbosity);
		g_setenv ("TRACKER_VERBOSITY", verbosity_string, FALSE);
		g_free (verbosity_string);
	}

	basename = g_strdup_printf ("%s.log", g_get_application_name ());
	filename = g_build_filename (g_get_user_data_dir (),
	                             "tracker",
	                             basename,
	                             NULL);
	g_free (basename);

	/* Open file */
	fd = g_fopen (filename, "a");
	if (!fd) {
		const gchar *error_string;

		error_string = g_strerror (errno);
		g_fprintf (stderr,
		           "Could not open log:'%s', %s\n",
		           filename,
		           error_string);
		g_fprintf (stderr,
		           "All logging will go to stderr\n");
	}

	verbosity = CLAMP (this_verbosity, 0, 3);

#if GLIB_CHECK_VERSION (2,31,0)
	g_mutex_init (&mutex);
#else
	mutex = g_mutex_new ();
#endif

	switch (this_verbosity) {
		/* Log level 3: EVERYTHING */
	case 3:
		break;

		/* Log level 2: CRITICAL/ERROR/WARNING/INFO/MESSAGE only */
	case 2:
		hide_levels = G_LOG_LEVEL_DEBUG;
		break;

		/* Log level 1: CRITICAL/ERROR/WARNING/INFO only */
	case 1:
		hide_levels = G_LOG_LEVEL_DEBUG |
		              G_LOG_LEVEL_MESSAGE;
		break;

		/* Log level 0: CRITICAL/ERROR/WARNING only (default) */
	default:
	case 0:
		hide_levels = G_LOG_LEVEL_DEBUG |
		              G_LOG_LEVEL_MESSAGE |
		              G_LOG_LEVEL_INFO;
		break;
	}

	if (hide_levels) {
		/* Hide log levels according to configuration */
		log_handler_id = g_log_set_handler (G_LOG_DOMAIN,
			                            hide_levels,
			                            hide_log_handler,
			                            NULL);
	}

	/* Set log handler function for the rest */
	g_log_set_default_handler (tracker_log_handler, NULL);

	if (used_filename) {
		*used_filename = filename;
	} else {
		g_free (filename);
	}

	initialized = TRUE;

	/* log binary name and version */
	g_message ("%s %s", g_get_application_name (), PACKAGE_VERSION);

	return TRUE;
}

void
tracker_log_shutdown (void)
{
	if (!initialized) {
		return;
	}

	/* Reset default log handler */
	g_log_set_default_handler (g_log_default_handler, NULL);

	if (log_handler_id) {
		g_log_remove_handler (G_LOG_DOMAIN, log_handler_id);
		log_handler_id = 0;
	}

	if (fd) {
		fclose (fd);
	}

#if GLIB_CHECK_VERSION (2,31,0)
	g_mutex_clear (&mutex);
#else
	g_mutex_free (mutex);
#endif

	initialized = FALSE;
}
