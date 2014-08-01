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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301  USA
 */

#include "config.h"

#include <string.h>

#include <libtracker-common/tracker-dbus.h>

#include "tracker-thumbnailer.h"

/**
 * SECTION:tracker-thumbnailer
 * @short_description: Request the thumbnailer service creates or
 * updates thumbnails.
 * @include: libtracker-miner/tracker-miner.h
 *
 * This is a convenience API using D-Bus for creating and updating
 * thumbnails for files being mined. It is also used to create
 * thumbnails for album art found embedded in some medias.
 *
 * This follows the thumbnailer specification:
 * http://live.gnome.org/ThumbnailerSpec
 **/

#define THUMBCACHE_SERVICE      "org.freedesktop.thumbnails.Cache1"
#define THUMBCACHE_PATH         "/org/freedesktop/thumbnails/Cache1"
#define THUMBCACHE_INTERFACE    "org.freedesktop.thumbnails.Cache1"

#define THUMBMAN_SERVICE        "org.freedesktop.thumbnails.Thumbnailer1"
#define THUMBMAN_PATH           "/org/freedesktop/thumbnails/Thumbnailer1"
#define THUMBMAN_INTERFACE      "org.freedesktop.thumbnails.Thumbnailer1"

typedef struct {
	GDBusProxy *cache_proxy;
	GDBusProxy *manager_proxy;
	GDBusConnection *connection;

	GStrv supported_mime_types;

	GSList *removes;
	GSList *moves_to;
	GSList *moves_from;

	guint request_id;
	gboolean service_is_available;
} TrackerThumbnailerPrivate;

static void tracker_thumbnailer_initable_iface_init (GInitableIface *iface);

G_DEFINE_TYPE_WITH_CODE (TrackerThumbnailer, tracker_thumbnailer, G_TYPE_OBJECT,
			 G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE,
						tracker_thumbnailer_initable_iface_init)
			 G_ADD_PRIVATE (TrackerThumbnailer))

static void
tracker_thumbnailer_finalize (GObject *object)
{
	TrackerThumbnailerPrivate *private;
	TrackerThumbnailer *thumbnailer;

	thumbnailer = TRACKER_THUMBNAILER (object);
	private = tracker_thumbnailer_get_instance_private (thumbnailer);

	if (private->cache_proxy) {
		g_object_unref (private->cache_proxy);
	}

	if (private->manager_proxy) {
		g_object_unref (private->manager_proxy);
	}

	if (private->connection) {
		g_object_unref (private->connection);
	}

	g_strfreev (private->supported_mime_types);

	g_slist_foreach (private->removes, (GFunc) g_free, NULL);
	g_slist_free (private->removes);

	g_slist_foreach (private->moves_to, (GFunc) g_free, NULL);
	g_slist_free (private->moves_to);

	g_slist_foreach (private->moves_from, (GFunc) g_free, NULL);
	g_slist_free (private->moves_from);

	G_OBJECT_CLASS (tracker_thumbnailer_parent_class)->finalize (object);
}

inline static gboolean
should_be_thumbnailed (GStrv        list,
                       const gchar *mime)
{
	gboolean should_thumbnail;
	guint i;

	if (!list) {
		return TRUE;
	}

	for (should_thumbnail = FALSE, i = 0;
	     should_thumbnail == FALSE && list[i] != NULL;
	     i++) {
		if (g_ascii_strcasecmp (list[i], mime) == 0) {
			should_thumbnail = TRUE;
		}
	}

	return should_thumbnail;
}

static gboolean
tracker_thumbnailer_initable_init (GInitable     *initable,
				   GCancellable  *cancellable,
				   GError       **error)
{
	TrackerThumbnailerPrivate *private;
	TrackerThumbnailer *thumbnailer;
	GVariant *v;

	thumbnailer = TRACKER_THUMBNAILER (initable);
	private = tracker_thumbnailer_get_instance_private (thumbnailer);

	/* Don't start at 0, start at 1. */
	private->request_id = 1;
	private->service_is_available = FALSE;

	g_message ("Thumbnailer connections being set up... (using same bus as Tracker, i.e. session or system)");

	private->connection = g_bus_get_sync (TRACKER_IPC_BUS, NULL, error);

	if (!private->connection)
		return FALSE;

	private->cache_proxy = g_dbus_proxy_new_sync (private->connection,
	                                              G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
	                                              NULL,
	                                              THUMBCACHE_SERVICE,
	                                              THUMBCACHE_PATH,
	                                              THUMBCACHE_INTERFACE,
	                                              NULL,
	                                              error);
	if (!private->cache_proxy) {
		g_clear_object (&private->connection);
		return FALSE;
	}

	private->manager_proxy = g_dbus_proxy_new_sync (private->connection,
	                                                G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
	                                                NULL,
	                                                THUMBMAN_SERVICE,
	                                                THUMBMAN_PATH,
	                                                THUMBMAN_INTERFACE,
	                                                NULL,
	                                                error);

	if (!private->manager_proxy) {
		g_clear_object (&private->connection);
		g_clear_object (&private->cache_proxy);
		return FALSE;
	}

	v = g_dbus_proxy_call_sync (private->manager_proxy,
	                            "GetSupported",
	                            NULL,
	                            G_DBUS_CALL_FLAGS_NONE,
	                            -1,
	                            NULL,
	                            error);

	if (!v) {
		g_clear_object (&private->connection);
		g_clear_object (&private->cache_proxy);
		g_clear_object (&private->manager_proxy);
		return FALSE;
	} else {
		GStrv mime_types = NULL;
		GStrv uri_schemes = NULL;

		g_variant_get (v, "(^a&s^a&s)", &uri_schemes, &mime_types);

		if (mime_types) {
			GHashTable *hash;
			GHashTableIter iter;
			gpointer key, value;
			guint i;

			/* The table that you receive may contain duplicate mime-types, because
			 * they are grouped against the uri_schemes table */

			hash = g_hash_table_new (g_str_hash, g_str_equal);

			for (i = 0; mime_types[i] != NULL; i++) {
				g_hash_table_insert (hash, mime_types[i], NULL);
			}

			i = g_hash_table_size (hash);
			g_message ("Thumbnailer supports %d mime types", i);

			g_hash_table_iter_init (&iter, hash);
			private->supported_mime_types = (GStrv) g_new0 (gchar *, i + 1);

			i = 0;
			while (g_hash_table_iter_next (&iter, &key, &value)) {
				private->supported_mime_types[i] = g_strdup (key);
				i++;
			}

			g_hash_table_unref (hash);

			private->service_is_available = TRUE;
		}

		g_free (mime_types);
		g_free (uri_schemes);

		g_variant_unref (v);
	}

	return TRUE;
}

static void
tracker_thumbnailer_initable_iface_init (GInitableIface *iface)
{
	iface->init = tracker_thumbnailer_initable_init;
}

static void
tracker_thumbnailer_class_init (TrackerThumbnailerClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	object_class->finalize = tracker_thumbnailer_finalize;
}

static void
tracker_thumbnailer_init (TrackerThumbnailer *thumbnailer)
{
}

TrackerThumbnailer *
tracker_thumbnailer_new (void)
{
	return g_initable_new (TRACKER_TYPE_THUMBNAILER, NULL, NULL, NULL);
}

/**
 * tracker_thumbnailer_move_add:
 * @thumbnailer: Thumbnailer object
 * @from_uri: URI of the file before the move
 * @mime_type: mime-type of the file
 * @to_uri: URI of the file after the move
 *
 * Adds a new request to tell the thumbnailer that @from_uri was moved to
 * @to_uri. Stored requests can be sent with tracker_thumbnailer_send().
 *
 * Returns: #TRUE if successfully stored to be reported, #FALSE otherwise.
 *
 * Since: 0.8
 */
gboolean
tracker_thumbnailer_move_add (TrackerThumbnailer *thumbnailer,
			      const gchar        *from_uri,
                              const gchar        *mime_type,
                              const gchar        *to_uri)
{
	TrackerThumbnailerPrivate *private;

	/* mime_type can be NULL */
	g_return_val_if_fail (TRACKER_IS_THUMBNAILER (thumbnailer), FALSE);
	g_return_val_if_fail (from_uri != NULL, FALSE);
	g_return_val_if_fail (to_uri != NULL, FALSE);

	private = tracker_thumbnailer_get_instance_private (thumbnailer);

	if (!private->service_is_available) {
		return FALSE;
	}

	if (mime_type && !should_be_thumbnailed (private->supported_mime_types, mime_type)) {
		return FALSE;
	}

	private->moves_from = g_slist_prepend (private->moves_from, g_strdup (from_uri));
	private->moves_to = g_slist_prepend (private->moves_to, g_strdup (to_uri));

	g_debug ("Thumbnailer request to move uri from:'%s' to:'%s' queued",
	         from_uri,
	         to_uri);

	return TRUE;
}

/**
 * tracker_thumbnailer_remove_add:
 * @thumbnailer: Thumbnailer object
 * @uri: URI of the file
 * @mime_type: mime-type of the file
 *
 * Adds a new request to tell the thumbnailer that @uri was removed.
 * Stored requests can be sent with tracker_thumbnailer_send().
 *
 * Returns: #TRUE if successfully stored to be reported, #FALSE otherwise.
 *
 * Since: 0.8
 */
gboolean
tracker_thumbnailer_remove_add (TrackerThumbnailer *thumbnailer,
				const gchar        *uri,
                                const gchar        *mime_type)
{
	TrackerThumbnailerPrivate *private;

	g_return_val_if_fail (TRACKER_IS_THUMBNAILER (thumbnailer), FALSE);
	/* mime_type can be NULL */
	g_return_val_if_fail (uri != NULL, FALSE);

	private = tracker_thumbnailer_get_instance_private (thumbnailer);

	if (!private->service_is_available) {
		return FALSE;
	}

	if (mime_type && !should_be_thumbnailed (private->supported_mime_types, mime_type)) {
		return FALSE;
	}

	private->removes = g_slist_prepend (private->removes, g_strdup (uri));

	g_debug ("Thumbnailer request to remove uri:'%s', appended to queue", uri);

	return TRUE;
}

/**
 * tracker_thumbnailer_cleanup:
 * @thumbnailer: Thumbnailer object
 * @uri_prefix: URI prefix
 *
 * Tells thumbnailer to cleanup all thumbnails under @uri_prefix.
 *
 * Returns: #TRUE if successfully reported, #FALSE otherwise.
 *
 * Since: 0.8
 */
gboolean
tracker_thumbnailer_cleanup (TrackerThumbnailer *thumbnailer,
			     const gchar        *uri_prefix)
{
	TrackerThumbnailerPrivate *private;

	g_return_val_if_fail (TRACKER_IS_THUMBNAILER (thumbnailer), FALSE);
	g_return_val_if_fail (uri_prefix != NULL, FALSE);

	private = tracker_thumbnailer_get_instance_private (thumbnailer);

	if (!private->service_is_available) {
		return FALSE;
	}

	private->request_id++;

	g_debug ("Thumbnailer cleaning up uri:'%s', request_id:%d...",
	         uri_prefix,
	         private->request_id);

	g_dbus_proxy_call (private->cache_proxy,
	                   "Cleanup",
	                   g_variant_new ("(s)", uri_prefix),
	                   G_DBUS_CALL_FLAGS_NONE,
	                   -1,
	                   NULL,
	                   NULL,
	                   NULL);

	return TRUE;
}

/**
 * tracker_thumbnailer_send:
 * @thumbnailer: Thumbnailer object
 *
 * Sends to the thumbnailer all stored requests.
 *
 * Since: 0.8
 */
void
tracker_thumbnailer_send (TrackerThumbnailer *thumbnailer)
{
	TrackerThumbnailerPrivate *private;
	guint list_len;

	g_return_if_fail (TRACKER_IS_THUMBNAILER (thumbnailer));

	private = tracker_thumbnailer_get_instance_private (thumbnailer);

	if (!private->service_is_available) {
		return;
	}

	list_len = g_slist_length (private->removes);

	if (list_len > 0) {
		GStrv uri_strv;

		uri_strv = tracker_dbus_slist_to_strv (private->removes);

		g_dbus_proxy_call (private->cache_proxy,
		                   "Delete",
		                   g_variant_new ("(^as)", uri_strv),
		                   G_DBUS_CALL_FLAGS_NONE,
		                   -1,
		                   NULL,
		                   NULL,
		                   NULL);

		g_message ("Thumbnailer removes queue sent with %d items to thumbnailer daemon, request ID:%d...",
		           list_len,
		           private->request_id++);

		/* Clean up newly created GStrv */
		g_strfreev (uri_strv);

		/* Clean up privately held data */
		g_slist_foreach (private->removes, (GFunc) g_free, NULL);
		g_slist_free (private->removes);
		private->removes = NULL;
	}

	list_len = g_slist_length (private->moves_from);

	if (list_len > 0) {
		GStrv from_strv, to_strv;

		g_assert (list_len == g_slist_length (private->moves_to));

		from_strv = tracker_dbus_slist_to_strv (private->moves_from);
		to_strv = tracker_dbus_slist_to_strv (private->moves_to);

		g_dbus_proxy_call (private->cache_proxy,
		                   "Move",
		                   g_variant_new ("(^as^as)", from_strv, to_strv),
		                   G_DBUS_CALL_FLAGS_NONE,
		                   -1,
		                   NULL,
		                   NULL,
		                   NULL);

		g_message ("Thumbnailer moves queue sent with %d items to thumbnailer daemon, request ID:%d...",
		           list_len,
		           private->request_id++);

		/* Clean up newly created GStrv */
		g_strfreev (from_strv);
		g_strfreev (to_strv);

		/* Clean up privately held data */
		g_slist_foreach (private->moves_from, (GFunc) g_free, NULL);
		g_slist_free (private->moves_from);
		private->moves_from = NULL;

		g_slist_foreach (private->moves_to, (GFunc) g_free, NULL);
		g_slist_free (private->moves_to);
		private->moves_to = NULL;
	}
}
