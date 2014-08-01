/*
 * Copyright (C) 2009, Nokia <ivan.frade@nokia.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */

#include "config.h"

#include <string.h>

#include <glib.h>

#include <libtracker-common/tracker-file-utils.h>
#include <libtracker-common/tracker-date-time.h>

#include "tracker-guarantee.h"

static gchar *
get_title_from_file (const gchar *uri)
{
	gchar *filename;
	gchar *basename;
	gchar *p;

	filename = g_filename_from_uri (uri, NULL, NULL);
	basename = g_filename_display_basename (filename);
	g_free (filename);

	p = strrchr (basename, '.');
	if (p) {
                if (p == basename) {
                        p = g_strdup (&basename[1]);
                        g_free (basename);
                        basename = p;
                } else {
                        *p = '\0';
                }
	}

	return g_strdelimit (basename, "_", ' ');
}

static gchar *
get_date_from_file_mtime (const gchar *uri)
{
	gchar *date;
	guint64 mtime;

	mtime = tracker_file_get_mtime_uri (uri);

	date = tracker_date_to_string ((time_t) mtime);

	return date;
}

/**
 * tracker_guarantee_title_from_file:
 * @metadata: the metadata object to insert the data into
 * @key: the key to insert into @metadata
 * @current_value: the current data to check before looking at @uri.
 * @uri: a string representing a URI to use
 * @p_new_value: pointer to a string which receives the new title, or
 *             %NULL
 *
 * Checks @current_value to make sure it is sane (i.e. not %NULL or an
 * empty string). If it is, then @uri is parsed to guarantee a
 * metadata value for @key.
 *
 * Parses the file pointed to by @uri and uses the basename
 * (before the "." and extension of the file) as the title. If the
 * title has any "_" characters, they are also converted into spaces.
 *
 * Returns: %TRUE on success, otherwise %FALSE.
 *
 * Since: 0.10
 **/
gboolean
tracker_guarantee_title_from_file (TrackerSparqlBuilder  *metadata,
                                   const gchar           *key,
                                   const gchar           *current_value,
                                   const gchar           *uri,
                                   gchar                **p_new_value)
{
	g_return_val_if_fail (metadata != NULL, FALSE);
	g_return_val_if_fail (key != NULL, FALSE);
	g_return_val_if_fail (uri != NULL, FALSE);

	tracker_sparql_builder_predicate (metadata, key);

	if (current_value && *current_value != '\0') {
		tracker_sparql_builder_object_unvalidated (metadata, current_value);

		if (p_new_value != NULL) {
			*p_new_value = g_strdup (current_value);
		}
	} else {
		gchar *value;

		value = get_title_from_file (uri);
		tracker_sparql_builder_object_unvalidated (metadata, value);

		if (p_new_value != NULL) {
			*p_new_value = value;
		} else {
			g_free (value);
		}
	}

	return TRUE;
}

/**
 * tracker_guarantee_date_from_file_mtime:
 * @metadata: the metadata object to insert the data into
 * @key: the key to insert into @metadata
 * @current_value: the current data to check before looking at @uri
 * @uri: a string representing a URI to use
 *
 * Checks @current_value to make sure it is sane (i.e. not %NULL or an
 * empty string). If it is, then @uri is parsed to guarantee a
 * metadata value for @key.
 *
 * When parsing @uri, stat() is called on the file to create a
 * date based on the file's mtime.
 *
 * Returns: %TRUE on success, otherwise %FALSE.
 *
 * Since: 0.10
 **/
gboolean
tracker_guarantee_date_from_file_mtime (TrackerSparqlBuilder *metadata,
                                        const gchar          *key,
                                        const gchar          *current_value,
                                        const gchar          *uri)
{
	g_return_val_if_fail (metadata != NULL, FALSE);
	g_return_val_if_fail (key != NULL, FALSE);
	g_return_val_if_fail (uri != NULL, FALSE);

	tracker_sparql_builder_predicate (metadata, key);

	if (current_value && *current_value != '\0') {
		tracker_sparql_builder_object_unvalidated (metadata, current_value);
	} else {
		gchar *value;

		value = get_date_from_file_mtime (uri);
		tracker_sparql_builder_object_unvalidated (metadata, value);
		g_free (value);
	}

	return TRUE;
}
