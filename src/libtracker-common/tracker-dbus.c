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
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */

#include "config.h"

#include <gio/gio.h>
#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>

#include <dbus/dbus-glib-bindings.h>

#include "tracker-dbus.h"
#include "tracker-log.h"

/* How long clients can exist since last D-Bus call before being
 * cleaned up.
 */
#define CLIENT_CLEAN_UP_TIME  300

typedef struct {
	gchar *sender;
	gchar *binary;
	gulong pid;
	guint clean_up_id;
	gint n_active_requests;
} ClientData;

struct _TrackerDBusRequest {
	guint request_id;
	ClientData *cd;
};

typedef struct {
	GInputStream *unix_input_stream;
	GInputStream *buffered_input_stream;
	GOutputStream *output_stream;
	TrackerDBusSendAndSpliceCallback callback;
	GCancellable *cancellable;
	gpointer user_data;
} SendAndSpliceData;

static gboolean client_lookup_enabled;
static DBusGConnection *freedesktop_connection;
static DBusGProxy *freedesktop_proxy;
static GHashTable *clients;

static void     client_data_free    (gpointer data);
static gboolean client_clean_up_cb (gpointer data);

static gboolean
clients_init (void)
{
	GError *error = NULL;
	DBusGConnection *conn;

	conn = dbus_g_bus_get (DBUS_BUS_SESSION, &error);

	if (!conn) {
		g_critical ("Could not connect to the D-Bus session bus, %s",
		            error ? error->message : "no error given.");
		g_error_free (error);
		return FALSE;
	}

	freedesktop_connection = dbus_g_connection_ref (conn);

	freedesktop_proxy =
		dbus_g_proxy_new_for_name (freedesktop_connection,
		                           DBUS_SERVICE_DBUS,
		                           DBUS_PATH_DBUS,
		                           DBUS_INTERFACE_DBUS);

	if (!freedesktop_proxy) {
		g_critical ("Could not create a proxy for the Freedesktop service, %s",
		            error ? error->message : "no error given.");
		g_error_free (error);
		return FALSE;
	}

	clients = g_hash_table_new_full (g_str_hash,
	                                 g_str_equal,
	                                 NULL,
	                                 client_data_free);

	return TRUE;
}

static gboolean
clients_shutdown (void)
{
	if (freedesktop_proxy) {
		g_object_unref (freedesktop_proxy);
		freedesktop_proxy = NULL;
	}

	if (freedesktop_connection) {
		dbus_g_connection_unref (freedesktop_connection);
		freedesktop_connection = NULL;
	}

	if (clients) {
		g_hash_table_unref (clients);
		clients = NULL;
	}

	return TRUE;
}

static void
client_data_free (gpointer data)
{
	ClientData *cd = data;

	if (!cd) {
		return;
	}

	g_source_remove (cd->clean_up_id);

	g_free (cd->sender);
	g_free (cd->binary);

	g_slice_free (ClientData, cd);
}

static ClientData *
client_data_new (gchar *sender)
{
	ClientData *cd;
	GError *error = NULL;
	guint pid;
	gboolean get_binary = TRUE;

	cd = g_slice_new0 (ClientData);
	cd->sender = sender;

	if (org_freedesktop_DBus_get_connection_unix_process_id (freedesktop_proxy,
	                                                         sender,
	                                                         &pid,
	                                                         &error)) {
		cd->pid = pid;
	}

	if (get_binary) {
		gchar *filename;
		gchar *pid_str;
		gchar *contents = NULL;
		GError *error = NULL;
		gchar **strv;

		pid_str = g_strdup_printf ("%ld", cd->pid);
		filename = g_build_filename (G_DIR_SEPARATOR_S,
		                             "proc",
		                             pid_str,
		                             "cmdline",
		                             NULL);
		g_free (pid_str);

		if (!g_file_get_contents (filename, &contents, NULL, &error)) {
			g_warning ("Could not get process name from id %ld, %s",
			           cd->pid,
			           error ? error->message : "no error given");
			g_clear_error (&error);
			g_free (filename);
			return cd;
		}

		g_free (filename);

		strv = g_strsplit (contents, "^@", 2);
		if (strv && strv[0]) {
			cd->binary = g_path_get_basename (strv[0]);
		}

		g_strfreev (strv);
		g_free (contents);
	}

	return cd;
}

static gboolean
client_clean_up_cb (gpointer data)
{
	ClientData *cd;

	cd = data;

	g_debug ("Removing D-Bus client data for '%s' (pid: %lu) with id:'%s'",
	         cd->binary, cd->pid, cd->sender);
	g_hash_table_remove (clients, cd->sender);

	if (g_hash_table_size (clients) < 1) {
		/* Clean everything up. */
		clients_shutdown ();
	}

	return FALSE;
}

static ClientData *
client_get_for_sender (const gchar *sender)
{
	ClientData *cd;

	if (!client_lookup_enabled) {
		return NULL;
	}

	/* Only really done with tracker-extract where we use
	 * functions from the command line with dbus code in them.
	 */
	if (!sender) {
		return NULL;
	}

	if (G_UNLIKELY (!clients)) {
		clients_init ();
	}

	cd = g_hash_table_lookup (clients, sender);
	if (!cd) {
		gchar *sender_dup;

		sender_dup = g_strdup (sender);
		cd = client_data_new (sender_dup);
		g_hash_table_insert (clients, sender_dup, cd);
	}

	if (cd->clean_up_id) {
		g_source_remove (cd->clean_up_id);
		cd->clean_up_id = 0;
	}

	cd->n_active_requests++;

	return cd;
}

GQuark
tracker_dbus_error_quark (void)
{
	return g_quark_from_static_string (TRACKER_DBUS_ERROR_DOMAIN);
}

gchar **
tracker_dbus_slist_to_strv (GSList *list)
{
	GSList  *l;
	gchar  **strv;
	gint     i = 0;

	strv = g_new0 (gchar*, g_slist_length (list) + 1);

	for (l = list; l != NULL; l = l->next) {
		if (!g_utf8_validate (l->data, -1, NULL)) {
			g_message ("Could not add string:'%s' to GStrv, invalid UTF-8",
			           (gchar*) l->data);
			continue;
		}

		strv[i++] = g_strdup (l->data);
	}

	strv[i] = NULL;

	return strv;
}

static guint
get_next_request_id (void)
{
	static guint request_id = 1;

	return request_id++;
}

TrackerDBusRequest *
tracker_dbus_request_begin (const gchar *sender,
                            const gchar *format,
                            ...)
{
	TrackerDBusRequest *request;
	gchar *str;
	va_list args;

	va_start (args, format);
	str = g_strdup_vprintf (format, args);
	va_end (args);

	request = g_slice_new (TrackerDBusRequest);
	request->request_id = get_next_request_id ();
	request->cd = client_get_for_sender (sender);

	g_debug ("<--- [%d%s%s|%lu] %s",
	         request->request_id,
	         request->cd ? "|" : "",
	         request->cd ? request->cd->binary : "",
	         request->cd ? request->cd->pid : 0,
	         str);

	g_free (str);

	return request;
}

void
tracker_dbus_request_end (TrackerDBusRequest *request,
                          GError             *error)
{
	if (!error) {
		g_debug ("---> [%d%s%s|%lu] Success, no error given",
			 request->request_id,
			 request->cd ? "|" : "",
			 request->cd ? request->cd->binary : "",
			 request->cd ? request->cd->pid : 0);
	} else {
		g_message ("---> [%d%s%s|%lu] Failed, %s",
			   request->request_id,
			   request->cd ? "|" : "",
			   request->cd ? request->cd->binary : "",
			   request->cd ? request->cd->pid : 0,
			   error->message);
	}

	if (request->cd) {
		request->cd->n_active_requests--;

		if (request->cd->n_active_requests == 0) {
			request->cd->clean_up_id = g_timeout_add_seconds (CLIENT_CLEAN_UP_TIME, client_clean_up_cb, request->cd);
		}
	}

	g_slice_free (TrackerDBusRequest, request);
}

void
tracker_dbus_request_info (TrackerDBusRequest    *request,
                           const gchar           *format,
                           ...)
{
	gchar *str;
	va_list args;

	va_start (args, format);
	str = g_strdup_vprintf (format, args);
	va_end (args);

	tracker_info ("---- [%d%s%s|%lu] %s",
	              request->request_id,
	              request->cd ? "|" : "",
	              request->cd ? request->cd->binary : "",
	              request->cd ? request->cd->pid : 0,
	              str);
	g_free (str);
}

void
tracker_dbus_request_comment (TrackerDBusRequest    *request,
                              const gchar           *format,
                              ...)
{
	gchar *str;
	va_list args;

	va_start (args, format);
	str = g_strdup_vprintf (format, args);
	va_end (args);

	g_message ("---- [%d%s%s|%lu] %s",
	           request->request_id,
	           request->cd ? "|" : "",
	           request->cd ? request->cd->binary : "",
	           request->cd ? request->cd->pid : 0,
	           str);
	g_free (str);
}

void
tracker_dbus_request_debug (TrackerDBusRequest    *request,
                            const gchar           *format,
                            ...)
{
	gchar *str;
	va_list args;

	va_start (args, format);
	str = g_strdup_vprintf (format, args);
	va_end (args);

	g_debug ("---- [%d%s%s|%lu] %s",
	         request->request_id,
	         request->cd ? "|" : "",
	         request->cd ? request->cd->binary : "",
	         request->cd ? request->cd->pid : 0,
	         str);
	g_free (str);
}

void
tracker_dbus_enable_client_lookup (gboolean enabled)
{
	/* If this changed and we disabled everything, simply shut it
	 * all down.
	 */
	if (client_lookup_enabled != enabled && !enabled) {
		clients_shutdown ();
	}

	client_lookup_enabled = enabled;
}

TrackerDBusRequest *
tracker_g_dbus_request_begin (GDBusMethodInvocation *invocation,
                              const gchar           *format,
                              ...)
{
	TrackerDBusRequest *request;
	gchar *str;
	const gchar *sender;
	va_list args;

	va_start (args, format);
	str = g_strdup_vprintf (format, args);
	va_end (args);

	sender = g_dbus_method_invocation_get_sender (invocation);
	request = tracker_dbus_request_begin (sender, "%s", str);

	g_free (str);

	return request;
}

TrackerDBusRequest *
tracker_dbus_g_request_begin (DBusGMethodInvocation *context,
                              const gchar           *format,
                              ...)
{
	TrackerDBusRequest *request;
	gchar *str, *sender;
	va_list args;

	va_start (args, format);
	str = g_strdup_vprintf (format, args);
	va_end (args);

	sender = dbus_g_method_get_sender (context);

	request = tracker_dbus_request_begin (sender, "%s", str);

	g_free (sender);

	g_free (str);

	return request;
}

static SendAndSpliceData *
send_and_splice_data_new (GInputStream                     *unix_input_stream,
                          GInputStream                     *buffered_input_stream,
                          GOutputStream                    *output_stream,
                          GCancellable                     *cancellable,
                          TrackerDBusSendAndSpliceCallback  callback,
                          gpointer                          user_data)
{
	SendAndSpliceData *data;

	data = g_slice_new0 (SendAndSpliceData);
	data->unix_input_stream = unix_input_stream;
	data->buffered_input_stream = buffered_input_stream;
	data->output_stream = output_stream;
	if (cancellable) {
		data->cancellable = g_object_ref (cancellable);
	}
	data->callback = callback;
	data->user_data = user_data;

	return data;
}

static void
send_and_splice_data_free (SendAndSpliceData *data)
{
	g_object_unref (data->unix_input_stream);
	g_object_unref (data->buffered_input_stream);
	g_object_unref (data->output_stream);
	if (data->cancellable) {
		g_object_unref (data->cancellable);
	}
	g_slice_free (SendAndSpliceData, data);
}

static void
send_and_splice_async_callback (GObject      *source,
                                GAsyncResult *result,
                                gpointer      user_data)
{
	GError *error = NULL;

	g_output_stream_splice_finish (G_OUTPUT_STREAM (source), result, &error);

	if (error) {
		g_critical ("Error while splicing: %s",
		            error ? error->message : "Error not specified");
		g_error_free (error);
	}
}

static void
tracker_dbus_send_and_splice_async_finish (GObject      *source,
                                           GAsyncResult *result,
                                           gpointer      user_data)
{
	SendAndSpliceData *data = user_data;
	GDBusMessage *reply;
	GError *error = NULL;

	reply = g_dbus_connection_send_message_with_reply_finish (G_DBUS_CONNECTION (source),
	                                                          result, &error);

	if (!error) {
		if (g_dbus_message_get_message_type (reply) == G_DBUS_MESSAGE_TYPE_ERROR) {

			g_dbus_message_to_gerror (reply, &error);
			g_free (g_memory_output_stream_get_data (G_MEMORY_OUTPUT_STREAM (data->output_stream)));
			(* data->callback) (NULL, -1, error, data->user_data);
			g_error_free (error);
		} else {
			(* data->callback) (g_memory_output_stream_get_data (G_MEMORY_OUTPUT_STREAM (data->output_stream)),
			                    g_memory_output_stream_get_data_size (G_MEMORY_OUTPUT_STREAM (data->output_stream)),
			                    NULL,
			                    data->user_data);
		}
	} else {
		g_free (g_memory_output_stream_get_data (G_MEMORY_OUTPUT_STREAM (data->output_stream)));
		(* data->callback) (NULL, -1, error, data->user_data);
		g_error_free (error);
	}

	if (reply) {
		g_object_unref (reply);
	}

	send_and_splice_data_free (data);
}

gboolean
tracker_dbus_send_and_splice_async (GDBusConnection                  *connection,
                                    GDBusMessage                     *message,
                                    int                               fd,
                                    GCancellable                     *cancellable,
                                    TrackerDBusSendAndSpliceCallback  callback,
                                    gpointer                          user_data)
{
	SendAndSpliceData *data;
	GInputStream *unix_input_stream;
	GInputStream *buffered_input_stream;
	GOutputStream *output_stream;

	g_return_val_if_fail (connection != NULL, FALSE);
	g_return_val_if_fail (message != NULL, FALSE);
	g_return_val_if_fail (fd > 0, FALSE);
	g_return_val_if_fail (callback != NULL, FALSE);

	unix_input_stream = g_unix_input_stream_new (fd, TRUE);
	buffered_input_stream = g_buffered_input_stream_new_sized (unix_input_stream,
	                                                           TRACKER_DBUS_PIPE_BUFFER_SIZE);
	output_stream = g_memory_output_stream_new (NULL, 0, g_realloc, NULL);

	data = send_and_splice_data_new (unix_input_stream,
	                                 buffered_input_stream,
	                                 output_stream,
	                                 cancellable,
	                                 callback,
	                                 user_data);

	g_dbus_connection_send_message_with_reply (connection,
	                                           message,
	                                           G_DBUS_SEND_MESSAGE_FLAGS_NONE,
	                                           -1,
	                                           NULL,
	                                           cancellable,
	                                           tracker_dbus_send_and_splice_async_finish,
	                                           data);

	g_output_stream_splice_async (data->output_stream,
	                              data->buffered_input_stream,
	                              G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE |
	                              G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET,
	                              0,
	                              data->cancellable,
	                              send_and_splice_async_callback,
	                              data);

	return TRUE;
}
