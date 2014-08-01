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

#include <math.h>

#include <glib/gi18n.h>

#include <libtracker-common/tracker-dbus.h>
#include <libtracker-common/tracker-type-utils.h>

#include "tracker-miner-object.h"
#include "tracker-miner-dbus.h"

/* Here we use ceil() to eliminate decimal points beyond what we're
 * interested in, which is 2 decimal places for the progress. The
 * ceil() call will also round up the last decimal place.
 *
 * The 0.49 value is used for rounding correctness, because ceil()
 * rounds up if the number is > 0.0.
 */
#define PROGRESS_ROUNDED(x) ((x) < 0.01 ? 0.00 : (ceil (((x) * 100) - 0.49) / 100))

#define TRACKER_SERVICE "org.freedesktop.Tracker1"

#ifdef MINER_STATUS_ENABLE_TRACE
#warning Miner status traces are enabled
#define trace(message, ...) g_debug (message, ##__VA_ARGS__)
#else
#define trace(...)
#endif /* MINER_STATUS_ENABLE_TRACE */

/**
 * SECTION:tracker-miner-object
 * @short_description: Abstract base class for data miners
 * @include: libtracker-miner/tracker-miner.h
 *
 * #TrackerMiner is an abstract base class to help developing data miners
 * for tracker-store, being an abstract class it doesn't do much by itself,
 * but provides the basic signaling and operation control so the miners
 * implementing this class are properly recognized by Tracker, and can be
 * controlled properly by external means such as #TrackerMinerManager.
 *
 * #TrackerMiner implements the #GInitable interface, and thus, all objects of
 * types inheriting from #TrackerMiner must be initialized with g_initable_init()
 * just after creation (or directly created with g_initable_new()).
 **/

#define TRACKER_MINER_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), TRACKER_TYPE_MINER, TrackerMinerPrivate))

static GQuark miner_error_quark = 0;

/* Introspection data for the service we are exporting */
static const gchar introspection_xml[] =
  "<node>"
  "  <interface name='org.freedesktop.Tracker1.Miner'>"
  "    <method name='GetStatus'>"
  "      <arg type='s' name='status' direction='out' />"
  "    </method>"
  "    <method name='GetProgress'>"
  "      <arg type='d' name='progress' direction='out' />"
  "    </method>"
  "    <method name='GetRemainingTime'>"
  "      <arg type='i' name='remaining_time' direction='out' />"
  "    </method>"
  "    <method name='GetPauseDetails'>"
  "      <arg type='as' name='pause_applications' direction='out' />"
  "      <arg type='as' name='pause_reasons' direction='out' />"
  "    </method>"
  "    <method name='Pause'>"
  "      <arg type='s' name='application' direction='in' />"
  "      <arg type='s' name='reason' direction='in' />"
  "      <arg type='i' name='cookie' direction='out' />"
  "    </method>"
  "    <method name='PauseForProcess'>"
  "      <arg type='s' name='application' direction='in' />"
  "      <arg type='s' name='reason' direction='in' />"
  "      <arg type='i' name='cookie' direction='out' />"
  "    </method>"
  "    <method name='Resume'>"
  "      <arg type='i' name='cookie' direction='in' />"
  "    </method>"
  "    <method name='IgnoreNextUpdate'>"
  "      <arg type='as' name='urls' direction='in' />"
  "    </method>"
  "    <signal name='Started' />"
  "    <signal name='Stopped' />"
  "    <signal name='Paused' />"
  "    <signal name='Resumed' />"
  "    <signal name='Progress'>"
  "      <arg type='s' name='status' />"
  "      <arg type='d' name='progress' />"
  "      <arg type='i' name='remaining_time' />"
  "    </signal>"
  "  </interface>"
  "</node>";

struct _TrackerMinerPrivate {
	TrackerSparqlConnection *connection;
	GHashTable *pauses;
	gboolean started;
	gchar *name;
	gchar *status;
	gdouble progress;
	gint remaining_time;
	gint availability_cookie;
	GDBusConnection *d_connection;
	GDBusNodeInfo *introspection_data;
	guint watch_name_id;
	guint registration_id;
	gchar *full_name;
	gchar *full_path;
	guint update_id;
};

typedef struct {
	gint cookie;
	gchar *application;
	gchar *reason;
	gchar *watch_name;
	guint watch_name_id;
} PauseData;

enum {
	PROP_0,
	PROP_NAME,
	PROP_STATUS,
	PROP_PROGRESS,
	PROP_REMAINING_TIME
};

enum {
	STARTED,
	STOPPED,
	PAUSED,
	RESUMED,
	PROGRESS,
	IGNORE_NEXT_UPDATE,
	LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

static void       miner_set_property           (GObject                *object,
                                                guint                   param_id,
                                                const GValue           *value,
                                                GParamSpec             *pspec);
static void       miner_get_property           (GObject                *object,
                                                guint                   param_id,
                                                GValue                 *value,
                                                GParamSpec             *pspec);
static void       miner_finalize               (GObject                *object);
static void       miner_initable_iface_init    (GInitableIface         *iface);
static gboolean   miner_initable_init          (GInitable              *initable,
                                                GCancellable           *cancellable,
                                                GError                **error);
static void       pause_data_destroy           (gpointer                data);
static PauseData *pause_data_new               (const gchar            *application,
                                                const gchar            *reason,
                                                const gchar            *watch_name,
                                                guint                   watch_name_id);
static void       handle_method_call           (GDBusConnection        *connection,
                                                const gchar            *sender,
                                                const gchar            *object_path,
                                                const gchar            *interface_name,
                                                const gchar            *method_name,
                                                GVariant               *parameters,
                                                GDBusMethodInvocation  *invocation,
                                                gpointer                user_data);
static GVariant  *handle_get_property          (GDBusConnection        *connection,
                                                const gchar            *sender,
                                                const gchar            *object_path,
                                                const gchar            *interface_name,
                                                const gchar            *property_name,
                                                GError                **error,
                                                gpointer                user_data);
static gboolean   handle_set_property          (GDBusConnection        *connection,
                                                const gchar            *sender,
                                                const gchar            *object_path,
                                                const gchar            *interface_name,
                                                const gchar            *property_name,
                                                GVariant               *value,
                                                GError                **error,
                                                gpointer                user_data);
static void       on_tracker_store_appeared    (GDBusConnection        *connection,
                                                const gchar            *name,
                                                const gchar            *name_owner,
                                                gpointer                user_data);
static void       on_tracker_store_disappeared (GDBusConnection        *connection,
                                                const gchar            *name,
                                                gpointer                user_data);

G_DEFINE_ABSTRACT_TYPE_WITH_CODE (TrackerMiner, tracker_miner, G_TYPE_OBJECT,
                                  G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE,
                                                         miner_initable_iface_init));

static void
tracker_miner_class_init (TrackerMinerClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	object_class->set_property = miner_set_property;
	object_class->get_property = miner_get_property;
	object_class->finalize     = miner_finalize;

	/**
	 * TrackerMiner::started:
	 * @miner: the #TrackerMiner
	 *
	 * the ::started signal is emitted in the miner
	 * right after it has been started through
	 * tracker_miner_start().
	 *
	 * Since: 0.8
	 **/
	signals[STARTED] =
		g_signal_new ("started",
		              G_OBJECT_CLASS_TYPE (object_class),
		              G_SIGNAL_RUN_LAST,
		              G_STRUCT_OFFSET (TrackerMinerClass, started),
		              NULL, NULL,
		              NULL,
		              G_TYPE_NONE, 0);
	/**
	 * TrackerMiner::stopped:
	 * @miner: the #TrackerMiner
	 *
	 * the ::stopped signal is emitted in the miner
	 * right after it has been stopped through
	 * tracker_miner_stop().
	 *
	 * Since: 0.8
	 **/
	signals[STOPPED] =
		g_signal_new ("stopped",
		              G_OBJECT_CLASS_TYPE (object_class),
		              G_SIGNAL_RUN_LAST,
		              G_STRUCT_OFFSET (TrackerMinerClass, stopped),
		              NULL, NULL,
		              NULL,
		              G_TYPE_NONE, 0);
	/**
	 * TrackerMiner::paused:
	 * @miner: the #TrackerMiner
	 *
	 * the ::paused signal is emitted whenever
	 * there is any reason to pause, either
	 * internal (through tracker_miner_pause()) or
	 * external (through DBus, see #TrackerMinerManager).
	 *
	 * Since: 0.8
	 **/
	signals[PAUSED] =
		g_signal_new ("paused",
		              G_OBJECT_CLASS_TYPE (object_class),
		              G_SIGNAL_RUN_LAST,
		              G_STRUCT_OFFSET (TrackerMinerClass, paused),
		              NULL, NULL,
		              NULL,
		              G_TYPE_NONE, 0);
	/**
	 * TrackerMiner::resumed:
	 * @miner: the #TrackerMiner
	 *
	 * the ::resumed signal is emitted whenever
	 * all reasons to pause have disappeared, see
	 * tracker_miner_resume() and #TrackerMinerManager.
	 *
	 * Since: 0.8
	 **/
	signals[RESUMED] =
		g_signal_new ("resumed",
		              G_OBJECT_CLASS_TYPE (object_class),
		              G_SIGNAL_RUN_LAST,
		              G_STRUCT_OFFSET (TrackerMinerClass, resumed),
		              NULL, NULL,
		              NULL,
		              G_TYPE_NONE, 0);
	/**
	 * TrackerMiner::progress:
	 * @miner: the #TrackerMiner
	 * @status: miner status
	 * @progress: a #gdouble indicating miner progress, from 0 to 1.
	 * @remaining_time: a #gint indicating the reamaining processing time, in
	 * seconds.
	 *
	 * the ::progress signal will be emitted by TrackerMiner implementations
	 * to indicate progress about the data mining process. @status will
	 * contain a translated string with the current miner status and @progress
	 * will indicate how much has been processed so far. @remaining_time will
	 * give the number expected of seconds to finish processing, 0 if the
	 * value cannot be estimated, and -1 if its not applicable.
	 *
	 * Since: 0.12
	 **/
	signals[PROGRESS] =
		g_signal_new ("progress",
		              G_OBJECT_CLASS_TYPE (object_class),
		              G_SIGNAL_RUN_LAST,
		              G_STRUCT_OFFSET (TrackerMinerClass, progress),
		              NULL, NULL,
		              NULL,
		              G_TYPE_NONE, 3,
		              G_TYPE_STRING,
		              G_TYPE_DOUBLE,
		              G_TYPE_INT);

	/**
	 * TrackerMiner::ignore-next-update:
	 * @miner: the #TrackerMiner
	 * @urls: the urls to mark as ignore on next update
	 *
	 * the ::ignore-next-update signal is emitted in the miner
	 * right after it has been asked to mark @urls as to ignore on next update
	 * through tracker_miner_ignore_next_update().
	 *
	 * Since: 0.8
	 **/
	signals[IGNORE_NEXT_UPDATE] =
		g_signal_new ("ignore-next-update",
		              G_OBJECT_CLASS_TYPE (object_class),
		              G_SIGNAL_RUN_LAST,
		              G_STRUCT_OFFSET (TrackerMinerClass, ignore_next_update),
		              NULL, NULL,
		              NULL,
		              G_TYPE_NONE, 1,
		              G_TYPE_STRV);

	g_object_class_install_property (object_class,
	                                 PROP_NAME,
	                                 g_param_spec_string ("name",
	                                                      "Miner name",
	                                                      "Miner name",
	                                                      NULL,
	                                                      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
	g_object_class_install_property (object_class,
	                                 PROP_STATUS,
	                                 g_param_spec_string ("status",
	                                                      "Status",
	                                                      "Translatable string with status description",
	                                                      "Idle",
	                                                      G_PARAM_READWRITE | G_PARAM_CONSTRUCT));
	g_object_class_install_property (object_class,
	                                 PROP_PROGRESS,
	                                 g_param_spec_double ("progress",
	                                                      "Progress",
	                                                      "Miner progress",
	                                                      0.0,
	                                                      1.0,
	                                                      0.0,
	                                                      G_PARAM_READWRITE | G_PARAM_CONSTRUCT));

	g_object_class_install_property (object_class,
	                                 PROP_REMAINING_TIME,
	                                 g_param_spec_int ("remaining-time",
	                                                   "Remaining time",
	                                                   "Estimated remaining time to finish processing",
	                                                   -1,
	                                                   G_MAXINT,
	                                                   -1,
	                                                   G_PARAM_READWRITE | G_PARAM_CONSTRUCT));

	g_type_class_add_private (object_class, sizeof (TrackerMinerPrivate));

	miner_error_quark = g_quark_from_static_string ("TrackerMiner");
}

static void
miner_initable_iface_init (GInitableIface *iface)
{
	iface->init = miner_initable_init;
}

static gboolean
miner_initable_init (GInitable     *initable,
                     GCancellable  *cancellable,
                     GError       **error)
{
	TrackerMiner *miner = TRACKER_MINER (initable);
	GError *inner_error = NULL;
	GVariant *reply;
	guint32 rval;
	GDBusInterfaceVTable interface_vtable = {
		handle_method_call,
		handle_get_property,
		handle_set_property
	};

	/* Try to get SPARQL connection... */
	miner->priv->connection = tracker_sparql_connection_get (NULL, &inner_error);
	if (!miner->priv->connection) {
		g_propagate_error (error, inner_error);
		return FALSE;
	}

	/* Try to get DBus connection... */
	miner->priv->d_connection = g_bus_get_sync (TRACKER_IPC_BUS, NULL, &inner_error);
	if (!miner->priv->d_connection) {
		g_propagate_error (error, inner_error);
		return FALSE;
	}

	/* Setup introspection data */
	miner->priv->introspection_data = g_dbus_node_info_new_for_xml (introspection_xml, &inner_error);
	if (!miner->priv->introspection_data) {
		g_propagate_error (error, inner_error);
		return FALSE;
	}

	/* Check miner has a proper name */
	if (!miner->priv->name) {
		g_set_error (error,
		             TRACKER_MINER_ERROR,
		             0,
		             "Miner '%s' should have been given a name, bailing out",
		             G_OBJECT_TYPE_NAME (miner));
		return FALSE;
	}

	/* Setup full name */
	miner->priv->full_name = g_strconcat (TRACKER_MINER_DBUS_NAME_PREFIX,
	                                         miner->priv->name,
	                                         NULL);

	/* Register the D-Bus object */
	miner->priv->full_path = g_strconcat (TRACKER_MINER_DBUS_PATH_PREFIX,
	                                         miner->priv->name,
	                                         NULL);

	g_message ("Registering D-Bus object...");
	g_message ("  Path:'%s'", miner->priv->full_path);
	g_message ("  Object Type:'%s'", G_OBJECT_TYPE_NAME (miner));

	miner->priv->registration_id =
		g_dbus_connection_register_object (miner->priv->d_connection,
		                                   miner->priv->full_path,
	                                       miner->priv->introspection_data->interfaces[0],
	                                       &interface_vtable,
	                                       miner,
	                                       NULL,
		                                   &inner_error);
	if (inner_error) {
		g_propagate_error (error, inner_error);
		g_prefix_error (error,
		                "Could not register the D-Bus object '%s'. ",
		                miner->priv->full_path);
		return FALSE;
	}

	/* Request the D-Bus name */
	reply = g_dbus_connection_call_sync (miner->priv->d_connection,
	                                     "org.freedesktop.DBus",
	                                     "/org/freedesktop/DBus",
	                                     "org.freedesktop.DBus",
	                                     "RequestName",
	                                     g_variant_new ("(su)",
	                                                    miner->priv->full_name,
	                                                    0x4 /* DBUS_NAME_FLAG_DO_NOT_QUEUE */),
	                                     G_VARIANT_TYPE ("(u)"),
	                                     0, -1, NULL, &inner_error);
	if (inner_error) {
		g_propagate_error (error, inner_error);
		g_prefix_error (error,
		                "Could not acquire name:'%s'. ",
		                miner->priv->full_name);
		return FALSE;
	}

	g_variant_get (reply, "(u)", &rval);
	g_variant_unref (reply);

	if (rval != 1 /* DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER */) {
		g_set_error (error,
		             TRACKER_MINER_ERROR,
		             0,
		             "D-Bus service name:'%s' is already taken, "
		             "perhaps the application is already running?",
		             miner->priv->full_name);
		return FALSE;
	}

	miner->priv->watch_name_id = g_bus_watch_name (TRACKER_IPC_BUS,
	                                               TRACKER_SERVICE,
	                                               G_BUS_NAME_WATCHER_FLAGS_NONE,
	                                               on_tracker_store_appeared,
	                                               on_tracker_store_disappeared,
	                                               miner,
	                                               NULL);

	return TRUE;
}

static void
tracker_miner_init (TrackerMiner *miner)
{
	TrackerMinerPrivate *priv;

	miner->priv = priv = TRACKER_MINER_GET_PRIVATE (miner);

	priv->pauses = g_hash_table_new_full (g_direct_hash,
	                                      g_direct_equal,
	                                      NULL,
	                                      pause_data_destroy);
}

static gboolean
miner_update_progress_cb (gpointer data)
{
	TrackerMiner *miner = data;

	trace ("(Miner:'%s') UPDATE PROGRESS SIGNAL", miner->priv->name);

	g_signal_emit (miner, signals[PROGRESS], 0,
	               miner->priv->status,
	               miner->priv->progress,
	               miner->priv->remaining_time);

	if (miner->priv->d_connection) {
		g_dbus_connection_emit_signal (miner->priv->d_connection,
		                               NULL,
		                               miner->priv->full_path,
		                               TRACKER_MINER_DBUS_INTERFACE,
		                               "Progress",
		                               g_variant_new ("(sdi)",
		                                              miner->priv->status,
		                                              miner->priv->progress,
		                                              miner->priv->remaining_time),
		                               NULL);
	}

	miner->priv->update_id = 0;

	return FALSE;
}

static void
miner_set_property (GObject      *object,
                    guint         prop_id,
                    const GValue *value,
                    GParamSpec   *pspec)
{
	TrackerMiner *miner = TRACKER_MINER (object);

	/* Quite often, we see status of 100% and still have
	 * status messages saying Processing... which is not
	 * true. So we use an idle timeout to help that situation.
	 * Additionally we can't force both properties are correct
	 * with the GObject API, so we have to do some checks our
	 * selves. The g_object_bind_property() API also isn't
	 * sufficient here.
	 */

	switch (prop_id) {
	case PROP_NAME:
		g_free (miner->priv->name);
		miner->priv->name = g_value_dup_string (value);
		break;
	case PROP_STATUS: {
		const gchar *new_status;

		new_status = g_value_get_string (value);

		trace ("(Miner:'%s') Set property:'status' to '%s'",
		       miner->priv->name,
		       new_status);

		if (miner->priv->status && new_status &&
		    strcmp (miner->priv->status, new_status) == 0) {
			/* Same, do nothing */
			break;
		}

		g_free (miner->priv->status);
		miner->priv->status = g_strdup (new_status);

		/* Check progress matches special statuses */
		if (new_status != NULL) {
			if (g_ascii_strcasecmp (new_status, "Initializing") == 0 &&
			    miner->priv->progress != 0.0) {
				trace ("(Miner:'%s') Set progress to 0.0 from status:'Initializing'",
				       miner->priv->name);
				miner->priv->progress = 0.0;
			} else if (g_ascii_strcasecmp (new_status, "Idle") == 0 &&
			           miner->priv->progress != 1.0) {
				trace ("(Miner:'%s') Set progress to 1.0 from status:'Idle'",
				       miner->priv->name);
				miner->priv->progress = 1.0;
			}
		}

		if (miner->priv->update_id == 0) {
			miner->priv->update_id = g_idle_add_full (G_PRIORITY_HIGH_IDLE,
			                                          miner_update_progress_cb,
			                                          miner,
			                                          NULL);
		}

		break;
	}
	case PROP_PROGRESS: {
		gdouble new_progress;

		new_progress = PROGRESS_ROUNDED (g_value_get_double (value));
		trace ("(Miner:'%s') Set property:'progress' to '%2.2f' (%2.2f before rounded)",
		         miner->priv->name,
		         new_progress,
		         g_value_get_double (value));

		/* NOTE: We don't round the current progress before
		 * comparison because we use the rounded value when
		 * we set it last.
		 *
		 * Only notify 1% changes
		 */
		if (new_progress == miner->priv->progress) {
			/* Same, do nothing */
			break;
		}

		miner->priv->progress = new_progress;

		/* Check status matches special progress values */
		if (new_progress == 0.0) {
			if (miner->priv->status == NULL ||
			    g_ascii_strcasecmp (miner->priv->status, "Initializing") != 0) {
				trace ("(Miner:'%s') Set status:'Initializing' from progress:0.0",
				       miner->priv->name);
				g_free (miner->priv->status);
				miner->priv->status = g_strdup ("Initializing");
			}
		} else if (new_progress == 1.0) {
			if (miner->priv->status == NULL ||
			    g_ascii_strcasecmp (miner->priv->status, "Idle") != 0) {
				trace ("(Miner:'%s') Set status:'Idle' from progress:1.0",
				       miner->priv->name);
				g_free (miner->priv->status);
				miner->priv->status = g_strdup ("Idle");
			}
		}

		if (miner->priv->update_id == 0) {
			miner->priv->update_id = g_idle_add_full (G_PRIORITY_HIGH_IDLE,
			                                          miner_update_progress_cb,
			                                          miner,
			                                          NULL);
		}

		break;
	}
	case PROP_REMAINING_TIME: {
		gint new_remaining_time;

		new_remaining_time = g_value_get_int (value);
		if (new_remaining_time != miner->priv->remaining_time) {
			/* Just set the new remaining time, don't notify it */
			miner->priv->remaining_time = new_remaining_time;
		}
		break;
	}
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
miner_get_property (GObject    *object,
                    guint       prop_id,
                    GValue     *value,
                    GParamSpec *pspec)
{
	TrackerMiner *miner = TRACKER_MINER (object);

	switch (prop_id) {
	case PROP_NAME:
		g_value_set_string (value, miner->priv->name);
		break;
	case PROP_STATUS:
		g_value_set_string (value, miner->priv->status);
		break;
	case PROP_PROGRESS:
		g_value_set_double (value, miner->priv->progress);
		break;
	case PROP_REMAINING_TIME:
		g_value_set_int (value, miner->priv->remaining_time);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static PauseData *
pause_data_new (const gchar *application,
                const gchar *reason,
                const gchar *watch_name,
                guint        watch_name_id)
{
	PauseData *data;
	static gint cookie = 1;

	data = g_slice_new0 (PauseData);

	data->cookie = cookie++;
	data->application = g_strdup (application);
	data->reason = g_strdup (reason);
	data->watch_name = g_strdup (watch_name);
	data->watch_name_id = watch_name_id;

	return data;
}

static void
pause_data_destroy (gpointer data)
{
	PauseData *pd;

	pd = data;

	if (pd->watch_name_id) {
		g_bus_unwatch_name (pd->watch_name_id);
	}

	g_free (pd->watch_name);

	g_free (pd->reason);
	g_free (pd->application);

	g_slice_free (PauseData, pd);
}

/**
 * tracker_miner_error_quark:
 *
 * Returns the #GQuark used to identify miner errors in GError structures.
 *
 * Returns: the error #GQuark
 *
 * Since: 0.8
 **/
GQuark
tracker_miner_error_quark (void)
{
	return g_quark_from_static_string (TRACKER_MINER_ERROR_DOMAIN);
}

/**
 * tracker_miner_start:
 * @miner: a #TrackerMiner
 *
 * Tells the miner to start processing data.
 *
 * Since: 0.8
 **/
void
tracker_miner_start (TrackerMiner *miner)
{
	g_return_if_fail (TRACKER_IS_MINER (miner));
	g_return_if_fail (miner->priv->started == FALSE);

	miner->priv->started = TRUE;

	g_signal_emit (miner, signals[STARTED], 0);

	if (miner->priv->d_connection) {
		g_dbus_connection_emit_signal (miner->priv->d_connection,
		                               NULL,
		                               miner->priv->full_path,
		                               TRACKER_MINER_DBUS_INTERFACE,
		                               "Started",
		                               NULL,
		                               NULL);
	}
}

/**
 * tracker_miner_stop:
 * @miner: a #TrackerMiner
 *
 * Tells the miner to stop processing data.
 *
 * Since: 0.8
 **/
void
tracker_miner_stop (TrackerMiner *miner)
{
	g_return_if_fail (TRACKER_IS_MINER (miner));
	g_return_if_fail (miner->priv->started == TRUE);

	miner->priv->started = FALSE;

	g_signal_emit (miner, signals[STOPPED], 0);

	if (miner->priv->d_connection) {
		g_dbus_connection_emit_signal (miner->priv->d_connection,
		                               NULL,
		                               miner->priv->full_path,
		                               TRACKER_MINER_DBUS_INTERFACE,
		                               "Stopped",
		                               NULL,
		                               NULL);
	}
}

/**
 * tracker_miner_ignore_next_update:
 * @miner: a #TrackerMiner
 * @urls: (in): the urls to mark as to ignore on next update
 *
 * Tells the miner to mark @urls are to ignore on next update.
 *
 * Since: 0.8
 **/
void
tracker_miner_ignore_next_update (TrackerMiner *miner,
                                  const GStrv   urls)
{
	g_return_if_fail (TRACKER_IS_MINER (miner));

	g_signal_emit (miner, signals[IGNORE_NEXT_UPDATE], 0, urls);
}

/**
 * tracker_miner_is_started:
 * @miner: a #TrackerMiner
 *
 * Returns #TRUE if the miner has been started.
 *
 * Returns: #TRUE if the miner is already started.
 *
 * Since: 0.8
 **/
gboolean
tracker_miner_is_started (TrackerMiner *miner)
{
	g_return_val_if_fail (TRACKER_IS_MINER (miner), TRUE);

	return miner->priv->started;
}

/**
 * tracker_miner_is_paused:
 * @miner: a #TrackerMiner
 *
 * Returns #TRUE if the miner is paused.
 *
 * Returns: #TRUE if the miner is paused.
 *
 * Since: 0.10
 **/
gboolean
tracker_miner_is_paused (TrackerMiner *miner)
{
	g_return_val_if_fail (TRACKER_IS_MINER (miner), TRUE);

	return g_hash_table_size (miner->priv->pauses) > 0 ? TRUE : FALSE;
}

/**
 * tracker_miner_get_n_pause_reasons:
 * @miner: a #TrackerMiner
 *
 * Returns the number of pause reasons holding @miner from
 * indexing contents.
 *
 * Returns: The number of current pause reasons
 *
 * Since: 0.10.5
 **/
guint
tracker_miner_get_n_pause_reasons (TrackerMiner *miner)
{
	g_return_val_if_fail (TRACKER_IS_MINER (miner), 0);

	return g_hash_table_size (miner->priv->pauses);
}

static void
pause_process_disappeared_cb (GDBusConnection *connection,
                              const gchar     *name,
                              gpointer         user_data)
{
	TrackerMiner *miner;
	PauseData *pd = NULL;
	GError *error = NULL;
	GHashTableIter iter;
	gpointer key, value;

	miner = user_data;

	g_message ("Process with name:'%s' has disappeared", name);

	g_hash_table_iter_init (&iter, miner->priv->pauses);
	while (g_hash_table_iter_next (&iter, &key, &value)) {
		PauseData *pd_iter = value;

		if (g_strcmp0 (name, pd_iter->watch_name) == 0) {
			pd = pd_iter;
			break;
		}
	}

	if (!pd) {
		g_critical ("Could not find PauseData for process with name:'%s'", name);
		return;
	}

	/* Resume */
	g_message ("Resuming pause associated with process");

	tracker_miner_resume (miner, pd->cookie, &error);

	if (error) {
		g_warning ("Could not resume miner, %s", error->message);
		g_error_free (error);
	}
}

static gint
miner_pause_internal (TrackerMiner  *miner,
                      const gchar   *application,
                      const gchar   *reason,
                      const gchar   *calling_name,
                      GError       **error)
{
	PauseData *pd;
	GHashTableIter iter;
	gpointer key, value;
	guint watch_name_id = 0;

	/* Check this is not a duplicate pause */
	g_hash_table_iter_init (&iter, miner->priv->pauses);
	while (g_hash_table_iter_next (&iter, &key, &value)) {
		PauseData *pd = value;

		if (g_strcmp0 (application, pd->application) == 0 &&
		    g_strcmp0 (reason, pd->reason) == 0) {
			/* Can't use duplicate pauses */
			g_set_error_literal (error, TRACKER_MINER_ERROR, 0,
			                     _("Pause application and reason match an already existing pause request"));
			return -1;
		}
	}

	if (calling_name) {
		g_message ("Watching process with name:'%s'", calling_name);
		watch_name_id = g_bus_watch_name (TRACKER_IPC_BUS,
		                                  calling_name,
		                                  G_BUS_NAME_WATCHER_FLAGS_NONE,
		                                  NULL,
		                                  pause_process_disappeared_cb,
		                                  miner,
		                                  NULL);
	}

	pd = pause_data_new (application, reason, calling_name, watch_name_id);

	g_hash_table_insert (miner->priv->pauses,
	                     GINT_TO_POINTER (pd->cookie),
	                     pd);

	if (g_hash_table_size (miner->priv->pauses) == 1) {
		/* Pause */
		g_message ("Miner:'%s' is pausing", miner->priv->name);
		g_signal_emit (miner, signals[PAUSED], 0);

		if (miner->priv->d_connection) {
			g_dbus_connection_emit_signal (miner->priv->d_connection,
			                               NULL,
			                               miner->priv->full_path,
			                               TRACKER_MINER_DBUS_INTERFACE,
			                               "Paused",
			                               NULL,
			                               NULL);
		}
	}

	return pd->cookie;
}

/**
 * tracker_miner_pause:
 * @miner: a #TrackerMiner
 * @reason: reason to pause
 * @error: (out callee-allocates) (transfer full) (allow-none): return location for errors
 *
 * Asks @miner to pause. On success the cookie ID is returned,
 * this is what must be used in tracker_miner_resume() to resume
 * operations. On failure @error will be set and -1 will be returned.
 *
 * Returns: The pause cookie ID.
 *
 * Since: 0.8
 **/
gint
tracker_miner_pause (TrackerMiner  *miner,
                     const gchar   *reason,
                     GError       **error)
{
	const gchar *application;

	g_return_val_if_fail (TRACKER_IS_MINER (miner), -1);
	g_return_val_if_fail (reason != NULL, -1);

	application = g_get_application_name ();

	if (!application) {
		application = miner->priv->name;
	}

	return miner_pause_internal (miner, application, reason, NULL, error);
}

/**
 * tracker_miner_resume:
 * @miner: a #TrackerMiner
 * @cookie: pause cookie
 * @error: (out) (transfer full) (allow-none): return location for errors
 *
 * Asks the miner to resume processing. The cookie must be something
 * returned by tracker_miner_pause(). The miner won't actually resume
 * operations until all pause requests have been resumed.
 *
 * Returns: #TRUE if the cookie was valid.
 *
 * Since: 0.8
 **/
gboolean
tracker_miner_resume (TrackerMiner  *miner,
                      gint           cookie,
                      GError       **error)
{
	g_return_val_if_fail (TRACKER_IS_MINER (miner), FALSE);

	if (!g_hash_table_remove (miner->priv->pauses, GINT_TO_POINTER (cookie))) {
		g_set_error_literal (error, TRACKER_MINER_ERROR, 0,
		                     _("Cookie not recognized to resume paused miner"));
		return FALSE;
	}

	if (g_hash_table_size (miner->priv->pauses) == 0) {
		/* Resume */
		g_message ("Miner:'%s' is resuming", miner->priv->name);
		g_signal_emit (miner, signals[RESUMED], 0);

		if (miner->priv->d_connection) {
			g_dbus_connection_emit_signal (miner->priv->d_connection,
			                               NULL,
			                               miner->priv->full_path,
			                               TRACKER_MINER_DBUS_INTERFACE,
			                               "Resumed",
			                               NULL,
			                               NULL);
		}
	}

	return TRUE;
}

/**
 * tracker_miner_get_connection:
 * @miner: a #TrackerMiner
 *
 * Gets the #TrackerSparqlConnection initialized by @miner
 *
 * Returns: (transfer none): a #TrackerSparqlConnection.
 *
 * Since: 0.10
 **/
TrackerSparqlConnection *
tracker_miner_get_connection (TrackerMiner *miner)
{
	return miner->priv->connection;
}

/**
 * tracker_miner_get_dbus_connection:
 * @miner: a #TrackerMiner
 *
 * Gets the #GDBusConnection initialized by @miner
 *
 * Returns: (transfer none): a #GDBusConnection.
 *
 * Since: 0.10
 **/
GDBusConnection *
tracker_miner_get_dbus_connection (TrackerMiner *miner)
{
	return miner->priv->d_connection;
}

/**
 * tracker_miner_get_dbus_full_name:
 * @miner: a #TrackerMiner
 *
 * Gets the DBus name registered by @miner
 *
 * Returns: a constant string which should not be modified by the caller.
 *
 * Since: 0.10
 **/
const gchar *
tracker_miner_get_dbus_full_name (TrackerMiner *miner)
{
	return miner->priv->full_name;
}

/**
 * tracker_miner_get_dbus_full_path:
 * @miner: a #TrackerMiner
 *
 * Gets the DBus path registered by @miner
 *
 * Returns: a constant string which should not be modified by the caller.
 *
 * Since: 0.10
 **/
const gchar *
tracker_miner_get_dbus_full_path (TrackerMiner *miner)
{
	return miner->priv->full_path;
}

static void
miner_finalize (GObject *object)
{
	TrackerMiner *miner = TRACKER_MINER (object);

	if (miner->priv->update_id != 0) {
		g_source_remove (miner->priv->update_id);
	}

	if (miner->priv->watch_name_id != 0) {
		g_bus_unwatch_name (miner->priv->watch_name_id);
	}

	if (miner->priv->registration_id != 0) {
		g_dbus_connection_unregister_object (miner->priv->d_connection,
		                                     miner->priv->registration_id);
	}

	if (miner->priv->introspection_data) {
		g_dbus_node_info_unref (miner->priv->introspection_data);
	}

	if (miner->priv->d_connection) {
		g_object_unref (miner->priv->d_connection);
	}

	g_free (miner->priv->status);
	g_free (miner->priv->name);
	g_free (miner->priv->full_name);
	g_free (miner->priv->full_path);

	if (miner->priv->connection) {
		g_object_unref (miner->priv->connection);
	}

	if (miner->priv->pauses) {
		g_hash_table_unref (miner->priv->pauses);
	}

	G_OBJECT_CLASS (tracker_miner_parent_class)->finalize (object);
}

static void
handle_method_call_ignore_next_update (TrackerMiner          *miner,
                                       GDBusMethodInvocation *invocation,
                                       GVariant              *parameters)
{
	GStrv urls = NULL;
	TrackerDBusRequest *request;

	g_variant_get (parameters, "(^a&s)", &urls);

	request = tracker_g_dbus_request_begin (invocation,
	                                        "%s", __PRETTY_FUNCTION__);

	tracker_miner_ignore_next_update (miner, urls);

	tracker_dbus_request_end (request, NULL);
	g_dbus_method_invocation_return_value (invocation, NULL);
	g_free (urls);
}

static void
handle_method_call_resume (TrackerMiner          *miner,
                           GDBusMethodInvocation *invocation,
                           GVariant              *parameters)
{
	GError *local_error = NULL;
	gint cookie;
	TrackerDBusRequest *request;

	g_variant_get (parameters, "(i)", &cookie);

	request = tracker_g_dbus_request_begin (invocation,
	                                        "%s(cookie:%d)",
	                                        __PRETTY_FUNCTION__,
	                                        cookie);

	if (!tracker_miner_resume (miner, cookie, &local_error)) {
		tracker_dbus_request_end (request, local_error);

		g_dbus_method_invocation_return_gerror (invocation, local_error);

		g_error_free (local_error);
		return;
	}

	tracker_dbus_request_end (request, NULL);
	g_dbus_method_invocation_return_value (invocation, NULL);
}

static void
handle_method_call_pause (TrackerMiner          *miner,
                          GDBusMethodInvocation *invocation,
                          GVariant              *parameters)
{
	GError *local_error = NULL;
	gint cookie;
	const gchar *application = NULL, *reason = NULL;
	TrackerDBusRequest *request;

	g_variant_get (parameters, "(&s&s)", &application, &reason);

	tracker_gdbus_async_return_if_fail (application != NULL, invocation);
	tracker_gdbus_async_return_if_fail (reason != NULL, invocation);

	request = tracker_g_dbus_request_begin (invocation,
	                                        "%s(application:'%s', reason:'%s')",
	                                        __PRETTY_FUNCTION__,
	                                        application,
	                                        reason);

	cookie = miner_pause_internal (miner, application, reason, NULL, &local_error);
	if (cookie == -1) {
		tracker_dbus_request_end (request, local_error);

		g_dbus_method_invocation_return_gerror (invocation, local_error);

		g_error_free (local_error);

		return;
	}

	tracker_dbus_request_end (request, NULL);
	g_dbus_method_invocation_return_value (invocation,
	                                       g_variant_new ("(i)", cookie));
}

static void
handle_method_call_pause_for_process (TrackerMiner          *miner,
                                      GDBusMethodInvocation *invocation,
                                      GVariant              *parameters)
{
	GError *local_error = NULL;
	gint cookie;
	const gchar *application = NULL, *reason = NULL;
	TrackerDBusRequest *request;

	g_variant_get (parameters, "(&s&s)", &application, &reason);

	tracker_gdbus_async_return_if_fail (application != NULL, invocation);
	tracker_gdbus_async_return_if_fail (reason != NULL, invocation);

	request = tracker_g_dbus_request_begin (invocation,
	                                        "%s(application:'%s', reason:'%s')",
	                                        __PRETTY_FUNCTION__,
	                                        application,
	                                        reason);

	cookie = miner_pause_internal (miner,
	                               application,
	                               reason,
	                               g_dbus_method_invocation_get_sender (invocation),
	                               &local_error);
	if (cookie == -1) {
		tracker_dbus_request_end (request, local_error);

		g_dbus_method_invocation_return_gerror (invocation, local_error);

		g_error_free (local_error);

		return;
	}

	tracker_dbus_request_end (request, NULL);
	g_dbus_method_invocation_return_value (invocation,
	                                       g_variant_new ("(i)", cookie));
}

static void
handle_method_call_get_pause_details (TrackerMiner          *miner,
                                      GDBusMethodInvocation *invocation,
                                      GVariant              *parameters)
{
	GSList *applications, *reasons;
	GStrv applications_strv, reasons_strv;
	GHashTableIter iter;
	gpointer key, value;
	TrackerDBusRequest *request;

	request = tracker_g_dbus_request_begin (invocation, "%s()", __PRETTY_FUNCTION__);

	applications = NULL;
	reasons = NULL;
	g_hash_table_iter_init (&iter, miner->priv->pauses);
	while (g_hash_table_iter_next (&iter, &key, &value)) {
		PauseData *pd = value;

		applications = g_slist_prepend (applications, pd->application);
		reasons = g_slist_prepend (reasons, pd->reason);
	}
	applications = g_slist_reverse (applications);
	reasons = g_slist_reverse (reasons);
	applications_strv = tracker_gslist_to_string_list (applications);
	reasons_strv = tracker_gslist_to_string_list (reasons);

	tracker_dbus_request_end (request, NULL);
	g_dbus_method_invocation_return_value (invocation,
	                                       g_variant_new ("(^as^as)",
	                                                      applications_strv,
	                                                      reasons_strv));

	g_strfreev (applications_strv);
	g_strfreev (reasons_strv);
	g_slist_free (applications);
	g_slist_free (reasons);
}

static void
handle_method_call_get_remaining_time (TrackerMiner          *miner,
                                       GDBusMethodInvocation *invocation,
                                       GVariant              *parameters)
{
	TrackerDBusRequest *request;

	request = tracker_g_dbus_request_begin (invocation, "%s()", __PRETTY_FUNCTION__);

	tracker_dbus_request_end (request, NULL);
	g_dbus_method_invocation_return_value (invocation,
	                                       g_variant_new ("(i)",
	                                                      miner->priv->remaining_time));
}

static void
handle_method_call_get_progress (TrackerMiner          *miner,
                                 GDBusMethodInvocation *invocation,
                                 GVariant              *parameters)
{
	TrackerDBusRequest *request;

	request = tracker_g_dbus_request_begin (invocation, "%s()", __PRETTY_FUNCTION__);

	tracker_dbus_request_end (request, NULL);
	g_dbus_method_invocation_return_value (invocation,
	                                       g_variant_new ("(d)",
	                                                      miner->priv->progress));
}

static void
handle_method_call_get_status (TrackerMiner          *miner,
                               GDBusMethodInvocation *invocation,
                               GVariant              *parameters)
{
	TrackerDBusRequest *request;

	request = tracker_g_dbus_request_begin (invocation, "%s()", __PRETTY_FUNCTION__);

	tracker_dbus_request_end (request, NULL);
	g_dbus_method_invocation_return_value (invocation,
	                                       g_variant_new ("(s)",
	                                                      miner->priv->status ? miner->priv->status : ""));

}

static void
handle_method_call (GDBusConnection       *connection,
                    const gchar           *sender,
                    const gchar           *object_path,
                    const gchar           *interface_name,
                    const gchar           *method_name,
                    GVariant              *parameters,
                    GDBusMethodInvocation *invocation,
                    gpointer               user_data)
{
	TrackerMiner *miner = user_data;

	tracker_gdbus_async_return_if_fail (miner != NULL, invocation);

	if (g_strcmp0 (method_name, "IgnoreNextUpdate") == 0) {
		handle_method_call_ignore_next_update (miner, invocation, parameters);
	} else if (g_strcmp0 (method_name, "Resume") == 0) {
		handle_method_call_resume (miner, invocation, parameters);
	} else if (g_strcmp0 (method_name, "Pause") == 0) {
		handle_method_call_pause (miner, invocation, parameters);
	} else if (g_strcmp0 (method_name, "PauseForProcess") == 0) {
		handle_method_call_pause_for_process (miner, invocation, parameters);
	} else if (g_strcmp0 (method_name, "GetPauseDetails") == 0) {
		handle_method_call_get_pause_details (miner, invocation, parameters);
	} else if (g_strcmp0 (method_name, "GetRemainingTime") == 0) {
		handle_method_call_get_remaining_time (miner, invocation, parameters);
	} else if (g_strcmp0 (method_name, "GetProgress") == 0) {
		handle_method_call_get_progress (miner, invocation, parameters);
	} else if (g_strcmp0 (method_name, "GetStatus") == 0) {
		handle_method_call_get_status (miner, invocation, parameters);
	} else {
		g_assert_not_reached ();
	}
}

static GVariant *
handle_get_property (GDBusConnection  *connection,
                     const gchar      *sender,
                     const gchar      *object_path,
                     const gchar      *interface_name,
                     const gchar      *property_name,
                     GError          **error,
                     gpointer          user_data)
{
	g_assert_not_reached ();
	return NULL;
}

static gboolean
handle_set_property (GDBusConnection  *connection,
                     const gchar      *sender,
                     const gchar      *object_path,
                     const gchar      *interface_name,
                     const gchar      *property_name,
                     GVariant         *value,
                     GError          **error,
                     gpointer          user_data)
{
	g_assert_not_reached ();
	return TRUE;
}

static void
on_tracker_store_appeared (GDBusConnection *connection,
                           const gchar     *name,
                           const gchar     *name_owner,
                           gpointer         user_data)

{
	TrackerMiner *miner = user_data;

	g_debug ("Miner:'%s' noticed store availability has changed to AVAILABLE",
	         miner->priv->name);

	if (miner->priv->availability_cookie != 0) {
		GError *error = NULL;

		tracker_miner_resume (miner,
		                      miner->priv->availability_cookie,
		                      &error);

		if (error) {
			g_warning ("Error happened resuming miner, %s", error->message);
			g_error_free (error);
		}

		miner->priv->availability_cookie = 0;
	}
}

static void
on_tracker_store_disappeared (GDBusConnection *connection,
                              const gchar     *name,
                              gpointer         user_data)
{
	TrackerMiner *miner = user_data;

	g_debug ("Miner:'%s' noticed store availability has changed to UNAVAILABLE",
	         miner->priv->name);

	if (miner->priv->availability_cookie == 0) {
		GError *error = NULL;
		gint cookie_id;

		cookie_id = tracker_miner_pause (miner,
		                                 _("Data store is not available"),
		                                 &error);

		if (error) {
			g_warning ("Could not pause, %s", error->message);
			g_error_free (error);
		} else {
			miner->priv->availability_cookie = cookie_id;
		}
	}
}
