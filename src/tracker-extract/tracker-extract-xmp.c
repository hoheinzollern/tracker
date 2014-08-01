/*
 * Copyright (C) 2007, Jason Kivlighn <jkivlighn@gmail.com>
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

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/mman.h>

#include <glib.h>
#include <glib/gstdio.h>

#include <gio/gio.h>

#include <libtracker-common/tracker-file-utils.h>

#include <libtracker-extract/tracker-extract.h>

/* This function is used to find the URI for a file.xmp file. The point here is
 * that the URI for file.xmp is not file:///file.xmp but instead for example
 * file:///file.jpeg or file:///file.png. The reason is that file.xmp is a
 * sidekick, and a sidekick doesn't describe itself, it describes another file. */

static gchar *
find_orig_uri (const gchar *xmp_filename)
{
	GFile *file;
	GFile *dir;
	GFileEnumerator *iter;
	GFileInfo *orig_info;
	const gchar *filename_a;
	gchar *found_file = NULL;

	file = g_file_new_for_path (xmp_filename);
	dir = g_file_get_parent (file);

	orig_info = g_file_query_info (file, G_FILE_ATTRIBUTE_STANDARD_NAME,
	                               G_FILE_QUERY_INFO_NONE,
	                               NULL, NULL);

	filename_a = g_file_info_get_name (orig_info);

	iter = g_file_enumerate_children (dir, G_FILE_ATTRIBUTE_STANDARD_NAME,
	                                  G_FILE_QUERY_INFO_NONE,
	                                  NULL, NULL);

	if (iter) {
		GFileInfo *info;

		while ((info = g_file_enumerator_next_file (iter, NULL, NULL)) && !found_file) {
			const gchar *filename_b;
			const gchar *ext_a, *ext_b;
			gchar *casefold_a, *casefold_b;

			/* OK, important:
			 * 1. Files can't be the same.
			 * 2. File names (without extension) must match
			 * 3. Something else? */

			filename_b = g_file_info_get_name (info);

			ext_a = g_utf8_strrchr (filename_a, -1, '.');
			ext_b = g_utf8_strrchr (filename_b, -1, '.');

			/* Look for extension */
			if (!ext_a || !ext_b) {
				g_object_unref (info);
				continue;
			}

			/* Name part is the same length */
			if ((ext_a - filename_a) != (ext_b - filename_b)) {
				g_object_unref (info);
				continue;
			}

			/* Check extensions are not the same (i.e. same len and ext) */
			if (g_strcmp0 (ext_a, ext_b) == 0) {
				g_object_unref (info);
				continue;
			}

			/* Don't compare the ".xmp" with ".jpeg" and don't match the same file */

			/* Now compare name (without ext) and make
			 * sure they are the same in a caseless
			 * compare. */

			casefold_a = g_utf8_casefold (filename_a, (ext_a - filename_a));
			casefold_b = g_utf8_casefold (filename_b, (ext_b - filename_b));

			if (g_strcmp0 (casefold_a, casefold_b) == 0) {
				GFile *found;

				found = g_file_get_child (dir, filename_b);
				found_file = g_file_get_uri (found);
				g_object_unref (found);
			}

			g_free (casefold_a);
			g_free (casefold_b);
			g_object_unref (info);
		}

		g_object_unref (iter);
	}

	g_object_unref (orig_info);
	g_object_unref (file);
	g_object_unref (dir);

	return found_file;
}

G_MODULE_EXPORT gboolean
tracker_extract_get_metadata (TrackerExtractInfo *info)
{
	TrackerSparqlBuilder *metadata, *preupdate;
	TrackerXmpData *xd = NULL;
	gchar *filename, *uri;
	gchar *contents;
	gsize length = 0;
	GFile *file;
	const gchar *graph;
	int fd;
	struct stat st;

	file = tracker_extract_info_get_file (info);
	filename = g_file_get_path (file);

	graph = tracker_extract_info_get_graph (info);
	preupdate = tracker_extract_info_get_preupdate_builder (info);
	metadata = tracker_extract_info_get_metadata_builder (info);

	fd = tracker_file_open_fd (filename);

	if (fd == -1) {
		g_warning ("Could not open xmp file '%s': %s\n",
		           filename,
		           g_strerror (errno));
		g_free (filename);
		return FALSE;
	}

	if (fstat (fd, &st) == -1) {
		g_warning ("Could not fstat xmp file '%s': %s\n",
		           filename,
		           g_strerror (errno));
		close (fd);
		g_free (filename);
		return FALSE;
	}

	if (st.st_size == 0) {
		contents = NULL;
		length = 0;
	} else {
		contents = (gchar *) mmap (NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
		if (contents == NULL) {
			g_warning ("Could not mmap xmp file '%s': %s\n",
			           filename,
			           g_strerror (errno));
			close (fd);
			g_free (filename);
			return FALSE;
		}
		length = st.st_size;
	}

	uri = g_file_get_uri (file);

	if (contents) {
		gchar *original_uri;

		original_uri = find_orig_uri (filename);

		/* If no orig file is found for the sidekick, we use the sidekick to
		 * describe itself instead, falling back to uri 
		 */
		xd = tracker_xmp_new (contents,
		                      length,
		                      original_uri ? original_uri : uri);

		if (xd) {
			GString *where;

			where = g_string_new ("");
			tracker_xmp_apply (preupdate, metadata, graph, where, uri, xd);
			tracker_extract_info_set_where_clause (info, where->str);
			g_string_free (where, TRUE);
		}

		g_free (original_uri);
		tracker_xmp_free (xd);
		g_free (filename);
		g_free (uri);

		munmap (contents, length);

		close (fd);

		return TRUE;
	}

	close (fd);
	g_free (filename);
	g_free (uri);

	return FALSE;
}
