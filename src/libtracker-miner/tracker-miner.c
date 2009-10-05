/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * Copyright (C) 2009, Nokia (urho.konttori@nokia.com)
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

#include <stdlib.h>

#include <libtracker-common/tracker-dbus.h>
#include <libtracker-common/tracker-type-utils.h>

#include "tracker-marshal.h"
#include "tracker-miner.h"
#include "tracker-miner-dbus.h"
#include "tracker-miner-glue.h"

#define TRACKER_MINER_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), TRACKER_TYPE_MINER, TrackerMinerPrivate))

struct TrackerMinerPrivate {
	TrackerClient *client;
	
	GHashTable *pauses;

	gboolean started;
	
	gchar *name;
	gchar *status;
	gdouble progress;

	gint availability_cookie;
};

typedef struct {
	DBusGConnection *connection;
	DBusGProxy *gproxy;
	GHashTable *name_monitors;
} DBusData;

typedef struct {
	gint cookie;
	gchar *application;
	gchar *reason;	
} PauseData;

enum {
	PROP_0,
	PROP_NAME,
	PROP_STATUS,
	PROP_PROGRESS
};

enum {
	STARTED,
	STOPPED,
	PAUSED,
	RESUMED,
	TERMINATED,
	PROGRESS,
	ERROR,
	LAST_SIGNAL
};

static GQuark dbus_data = 0;

static guint signals[LAST_SIGNAL] = { 0 };

static void       miner_set_property (GObject      *object,
				      guint         param_id,
				      const GValue *value,
				      GParamSpec   *pspec);
static void       miner_get_property (GObject      *object,
				      guint         param_id,
				      GValue       *value,
				      GParamSpec   *pspec);
static void       miner_finalize     (GObject      *object);
static void       miner_constructed  (GObject      *object);
static void       dbus_data_destroy  (gpointer      data);
static DBusData * dbus_data_create   (TrackerMiner *miner,
				      const gchar  *name);
static void       pause_data_destroy (gpointer      data);
static PauseData *pause_data_new     (const gchar  *application,
				      const gchar  *reason);

G_DEFINE_ABSTRACT_TYPE (TrackerMiner, tracker_miner, G_TYPE_OBJECT)

static void
tracker_miner_class_init (TrackerMinerClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	object_class->set_property = miner_set_property;
	object_class->get_property = miner_get_property;
	object_class->finalize     = miner_finalize;
	object_class->constructed  = miner_constructed;

	/**
	 * TrackerMiner::started:
	 * @miner: the #TrackerMiner
	 *
	 * the ::started signal is emitted in the miner
	 * right after it has been started through
	 * tracker_miner_start().
	 **/
	signals[STARTED] =
		g_signal_new ("started",
			      G_OBJECT_CLASS_TYPE (object_class),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (TrackerMinerClass, started),
			      NULL, NULL,
			      g_cclosure_marshal_VOID__VOID,
			      G_TYPE_NONE, 0);
	/**
	 * TrackerMiner::stopped:
	 * @miner: the #TrackerMiner
	 *
	 * the ::stopped signal is emitted in the miner
	 * right after it has been stopped through
	 * tracker_miner_stop().
	 **/
	signals[STOPPED] =
		g_signal_new ("stopped",
			      G_OBJECT_CLASS_TYPE (object_class),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (TrackerMinerClass, stopped),
			      NULL, NULL,
			      g_cclosure_marshal_VOID__VOID,
			      G_TYPE_NONE, 0);
	/**
	 * TrackerMiner::paused:
	 * @miner: the #TrackerMiner
	 *
	 * the ::paused signal is emitted whenever
	 * there is any reason to pause, either
	 * internal (through tracker_miner_pause()) or
	 * external (through DBus, see #TrackerMinerManager).
	 **/
	signals[PAUSED] =
		g_signal_new ("paused",
			      G_OBJECT_CLASS_TYPE (object_class),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (TrackerMinerClass, paused),
			      NULL, NULL,
			      g_cclosure_marshal_VOID__VOID,
			      G_TYPE_NONE, 0);
	/**
	 * TrackerMiner::resumed:
	 * @miner: the #TrackerMiner
	 *
	 * the ::resumed signal is emitted whenever
	 * all reasons to pause have disappeared, see
	 * tracker_miner_resume() and #TrackerMinerManager.
	 **/
	signals[RESUMED] =
		g_signal_new ("resumed",
			      G_OBJECT_CLASS_TYPE (object_class),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (TrackerMinerClass, resumed),
			      NULL, NULL,
			      g_cclosure_marshal_VOID__VOID,
			      G_TYPE_NONE, 0);
	signals[TERMINATED] =
		g_signal_new ("terminated",
			      G_OBJECT_CLASS_TYPE (object_class),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (TrackerMinerClass, terminated),
			      NULL, NULL,
			      g_cclosure_marshal_VOID__VOID,
			      G_TYPE_NONE, 0);
	/**
	 * TrackerMiner::progress:
	 * @miner: the #TrackerMiner
	 * @status: 
	 * @progress: a #gdouble indicating miner progress, from 0 to 1.
	 *
	 * the ::progress signal will be emitted by TrackerMiner implementations
	 * to indicate progress about the data mining process. @status will
	 * contain a translated string with the current miner status and @progress
	 * will indicate how much has been processed so far.
	 **/
	signals[PROGRESS] =
		g_signal_new ("progress",
			      G_OBJECT_CLASS_TYPE (object_class),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (TrackerMinerClass, progress),
			      NULL, NULL,
			      tracker_marshal_VOID__STRING_DOUBLE,
			      G_TYPE_NONE, 2,
			      G_TYPE_STRING,
			      G_TYPE_DOUBLE);
	signals[ERROR] =
		g_signal_new ("error",
			      G_OBJECT_CLASS_TYPE (object_class),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (TrackerMinerClass, error),
			      NULL, NULL,
			      g_cclosure_marshal_VOID__POINTER,
			      G_TYPE_NONE, 1,
			      G_TYPE_POINTER);

	g_object_class_install_property (object_class,
					 PROP_NAME,
					 g_param_spec_string ("name",
							      "Name",
							      "Name",
							      NULL,
							      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
	g_object_class_install_property (object_class,
					 PROP_STATUS,
					 g_param_spec_string ("status",
							      "Status",
							      "Status (unique to each miner)",
							      NULL,
							      G_PARAM_READWRITE));
	g_object_class_install_property (object_class,
					 PROP_PROGRESS,
					 g_param_spec_double ("progress",
							      "Progress",
							      "Progress (between 0 and 1)",
							      0.0, 
							      1.0,
							      0.0,
							      G_PARAM_READWRITE));

	g_type_class_add_private (object_class, sizeof (TrackerMinerPrivate));
}

static void
tracker_miner_init (TrackerMiner *miner)
{
	TrackerMinerPrivate *priv;

	miner->private = priv = TRACKER_MINER_GET_PRIVATE (miner);

	priv->client = tracker_connect (TRUE, G_MAXINT);

	priv->pauses = g_hash_table_new_full (g_direct_hash,
					      g_direct_equal,
					      NULL,
					      pause_data_destroy);
}

static void
miner_update_progress (TrackerMiner *miner)
{
	g_signal_emit (miner, signals[PROGRESS], 0,
		       miner->private->status,
		       miner->private->progress);
}

static void
miner_set_property (GObject      *object,
		    guint         prop_id,
		    const GValue *value,
		    GParamSpec   *pspec)
{
	TrackerMiner *miner = TRACKER_MINER (object);

	switch (prop_id) {
	case PROP_NAME:
		g_free (miner->private->name);
		miner->private->name = g_value_dup_string (value);
		break;
	case PROP_STATUS: {
		const gchar *new_status;

		new_status = g_value_get_string (value);
		if (miner->private->status && new_status &&
		    strcmp (miner->private->status, new_status) == 0) {
			/* Same, do nothing */
			break;
		}

		g_free (miner->private->status);
		miner->private->status = g_strdup (new_status);
		miner_update_progress (miner);
		break;
	}
	case PROP_PROGRESS: {
		gdouble new_progress;

		new_progress = g_value_get_double (value);

		/* Only notify 1% changes */
		if ((gint) (miner->private->progress * 100) == (gint) (new_progress * 100)) {
			/* Same, do nothing */
			break;
		}

		miner->private->progress = new_progress;
		miner_update_progress (miner);
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
		g_value_set_string (value, miner->private->name);
		break;
	case PROP_STATUS:
		g_value_set_string (value, miner->private->status);
		break;
	case PROP_PROGRESS:
		g_value_set_double (value, miner->private->progress);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
miner_finalize (GObject *object)
{
	TrackerMiner *miner = TRACKER_MINER (object);

	g_free (miner->private->status);
	g_free (miner->private->name);

	if (miner->private->client) {
		tracker_disconnect (miner->private->client);
	}

	if (dbus_data != 0) {
		g_object_set_qdata (G_OBJECT (miner), dbus_data, NULL);
	}

	g_hash_table_unref (miner->private->pauses);

	G_OBJECT_CLASS (tracker_miner_parent_class)->finalize (object);
}

static void
miner_constructed (GObject *object)
{
	TrackerMiner *miner;
	DBusData *data;

	miner = TRACKER_MINER (object);

	if (!miner->private->name) {
		g_critical ("Miner should have been given a name, bailing out");
		g_assert_not_reached ();
	}

	if (G_UNLIKELY (dbus_data == 0)) {
		dbus_data = g_quark_from_static_string ("tracker-miner-dbus-data");
	}

	data = g_object_get_qdata (G_OBJECT (miner), dbus_data);

	if (G_LIKELY (!data)) {
		data = dbus_data_create (miner, miner->private->name);
	}

	if (G_UNLIKELY (!data)) {
		g_critical ("Miner could not register object on D-Bus session");
		exit (EXIT_FAILURE);
		return;
	}

	g_object_set_qdata_full (G_OBJECT (miner), 
				 dbus_data, 
				 data,
				 dbus_data_destroy);
}

/**
 * tracker_miner_error_quark:
 *
 * Returns the #GQuark used to identify miner errors in GError structures.
 *
 * Returns: the error #GQuark
 **/
GQuark
tracker_miner_error_quark (void)
{
	return g_quark_from_static_string (TRACKER_MINER_ERROR_DOMAIN);
}

static gboolean
dbus_register_service (DBusGProxy  *proxy,
		       const gchar *name)
{
	GError *error = NULL;
	guint	result;

	g_message ("Registering D-Bus service...\n"
		   "  Name:'%s'",
		   name);

	if (!org_freedesktop_DBus_request_name (proxy,
						name,
						DBUS_NAME_FLAG_DO_NOT_QUEUE,
						&result, &error)) {
		g_critical ("Could not acquire name:'%s', %s",
			    name,
			    error ? error->message : "no error given");
		g_error_free (error);

		return FALSE;
	}

	if (result != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
		g_critical ("D-Bus service name:'%s' is already taken, "
			    "perhaps the application is already running?",
			    name);
		return FALSE;
	}

	return TRUE;
}

static gboolean
dbus_register_object (GObject		    *object,
		      DBusGConnection	    *connection,
		      DBusGProxy	    *proxy,
		      const DBusGObjectInfo *info,
		      const gchar	    *path)
{
	g_message ("Registering D-Bus object...");
	g_message ("  Path:'%s'", path);
	g_message ("  Object Type:'%s'", G_OBJECT_TYPE_NAME (object));

	dbus_g_object_type_install_info (G_OBJECT_TYPE (object), info);
	dbus_g_connection_register_g_object (connection, path, object);

	return TRUE;
}

static void
name_owner_changed_cb (DBusGProxy *proxy,
		       gchar	  *name,
		       gchar	  *old_owner,
		       gchar	  *new_owner,
		       gpointer    user_data)
{
	TrackerMiner *miner;
	gboolean available;
	GError *error = NULL;

	if (!name || !*name ||
	    strcmp (name, "org.freedesktop.Tracker1") != 0) {
		return;
	}

	miner = user_data;
	available = (new_owner && *new_owner);

	g_debug ("Tracker-store availability has changed to %d", available);

	if (available && miner->private->availability_cookie != 0) {
		tracker_miner_resume (miner,
				      miner->private->availability_cookie,
				      &error);

		if (error) {
			g_warning ("Error happened resuming miner: %s\n", error->message);
			g_error_free (error);
		}

		miner->private->availability_cookie = 0;
	} else if (!available && miner->private->availability_cookie == 0) {
		gint cookie_id;

		cookie_id = tracker_miner_pause (miner,
						 g_get_application_name (),
						 _("Data store is not available"),
						 &error);

		if (error) {
			g_warning ("Could not pause: %s", error->message);
			g_error_free (error);
		} else {
			miner->private->availability_cookie = cookie_id;
		}
	}
}

static void
dbus_set_name_monitor (TrackerMiner *miner,
		       DBusGProxy   *proxy)
{
	dbus_g_proxy_add_signal (proxy, "NameOwnerChanged",
				 G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
				 G_TYPE_INVALID);

	dbus_g_proxy_connect_signal (proxy, "NameOwnerChanged",
				     G_CALLBACK (name_owner_changed_cb),
				     miner, NULL);
}

static void
dbus_data_destroy (gpointer data)
{
	DBusData *dd;

	dd = data;

	if (dd->gproxy) {
		g_object_unref (dd->gproxy);
	}

	if (dd->connection) {
		dbus_g_connection_unref (dd->connection);
	}

	if (dd->name_monitors) {
		g_hash_table_unref (dd->name_monitors);
	}

	g_slice_free (DBusData, dd);
}

static DBusData *
dbus_data_create (TrackerMiner *miner,
		  const gchar  *name)
{
	DBusData *data;
	DBusGConnection *connection;
	DBusGProxy *gproxy;
	GError *error = NULL;
	gchar *full_name, *full_path;

	connection = dbus_g_bus_get (DBUS_BUS_SESSION, &error);

	if (!connection) {
		g_critical ("Could not connect to the D-Bus session bus, %s",
			    error ? error->message : "no error given.");
		g_error_free (error);
		return NULL;
	}

	gproxy = dbus_g_proxy_new_for_name (connection,
					    DBUS_SERVICE_DBUS,
					    DBUS_PATH_DBUS,
					    DBUS_INTERFACE_DBUS);

	/* Register the service name for the miner */
	full_name = g_strconcat (TRACKER_MINER_DBUS_NAME_PREFIX, name, NULL);

	if (!dbus_register_service (gproxy, full_name)) {
		g_object_unref (gproxy);
		g_free (full_name);
		return NULL;
	}

	g_free (full_name);

	full_path = g_strconcat (TRACKER_MINER_DBUS_PATH_PREFIX, name, NULL);

	if (!dbus_register_object (G_OBJECT (miner),
				   connection, gproxy,
				   &dbus_glib_tracker_miner_object_info,
				   full_path)) {
		g_object_unref (gproxy);
		g_free (full_path);
		return NULL;
	}

	dbus_set_name_monitor (miner, gproxy);

	g_free (full_path);

	/* Now we're successfully connected and registered, create the data */
	data = g_slice_new0 (DBusData);
	data->connection = dbus_g_connection_ref (connection);
	data->gproxy = g_object_ref (gproxy);

	return data;
}

static PauseData *
pause_data_new (const gchar *application,
		const gchar *reason)
{
	PauseData *data;
	static gint cookie = 1;

	data = g_slice_new0 (PauseData);

	data->cookie = cookie++;
	data->application = g_strdup (application);
	data->reason = g_strdup (reason);

	return data;
}

static void
pause_data_destroy (gpointer data)
{
	PauseData *pd;
	
	pd = data;

	g_free (pd->reason);
	g_free (pd->application);

	g_slice_free (PauseData, pd);
}

/**
 * tracker_miner_start:
 * @miner: a #TrackerMiner
 *
 * Tells the miner to start processing data.
 **/
void
tracker_miner_start (TrackerMiner *miner)
{
	g_return_if_fail (TRACKER_IS_MINER (miner));
	g_return_if_fail (miner->private->started == FALSE);

	miner->private->started = TRUE;

	g_signal_emit (miner, signals[STARTED], 0);
}

/**
 * tracker_miner_stop:
 * @miner: a #TrackerMiner
 *
 * Tells the miner to stop processing data.
 **/
void
tracker_miner_stop (TrackerMiner *miner)
{
	g_return_if_fail (TRACKER_IS_MINER (miner));
	g_return_if_fail (miner->private->started == TRUE);

	miner->private->started = FALSE;

	g_signal_emit (miner, signals[STOPPED], 0);
}

/**
 * tracker_miner_is_started:
 * @miner: a #TrackerMiner
 *
 * Returns #TRUE if the miner has been started.
 *
 * Returns: #TRUE if the miner is already started.
 **/
gboolean
tracker_miner_is_started (TrackerMiner  *miner)
{
	g_return_val_if_fail (TRACKER_IS_MINER (miner), TRUE);

	return miner->private->started;
}

/**
 * tracker_miner_execute_update:
 * @miner: a #TrackerMiner
 * @sparql: a SPARQL query
 * @error: return location for errors
 *
 * Executes an update SPARQL query on tracker-store, use this
 * whenever you want to perform data insertions or modifications.
 *
 * Returns: #TRUE if the SPARQL query was executed successfully.
 **/
gboolean
tracker_miner_execute_update (TrackerMiner  *miner,
			      const gchar   *sparql,
			      GError       **error)
{
	GError *internal_error = NULL;

	g_return_val_if_fail (TRACKER_IS_MINER (miner), FALSE);

	tracker_resources_sparql_update (miner->private->client,
					 sparql, 
					 &internal_error);

	if (!internal_error) {
		return TRUE;
	}

	if (error) {
		g_propagate_error (error, internal_error);
	} else {
		g_warning ("Error running sparql queries: %s", internal_error->message);
		g_error_free (internal_error);
	}

	return FALSE;
}

/**
 * tracker_miner_execute_sparql:
 * @miner: a #TrackerMiner
 * @sparql: a SPARQL query
 * @error: return location for errors
 *
 * Executes the SPARQL query on tracker-store and returns the
 * queried data. Use this whenever you need to get data from
 * already stored information.
 *
 * Returns: a #GPtrArray with the returned data.
 **/
GPtrArray *
tracker_miner_execute_sparql (TrackerMiner  *miner,
			      const gchar   *sparql,
			      GError       **error)
{
	GError *internal_error = NULL;
	GPtrArray *res;

	g_return_val_if_fail (TRACKER_IS_MINER (miner), FALSE);

	res = tracker_resources_sparql_query (miner->private->client,
					      sparql, 
					      &internal_error);

	if (!internal_error) {
		return res;
	}

	if (error) {
		g_propagate_error (error, internal_error);
	} else {
		g_warning ("Error running sparql queries: %s", internal_error->message);
		g_error_free (internal_error);
	}

	return res;
}

/**
 * tracker_miner_execute_batch_update:
 * @miner: a #TrackerMiner
 * @sparql: a set of SPARQL updates
 * @error: return location for errors
 *
 * Executes a batch of update SPARQL queries on tracker-store, use this
 * whenever you want to perform data insertions or modifications in
 * batches.
 *
 * Returns: #TRUE if the SPARQL query was executed successfully.
 **/
gboolean
tracker_miner_execute_batch_update (TrackerMiner  *miner,
				    const gchar   *sparql,
				    GError       **error)
{
	GError *internal_error = NULL;

	g_return_val_if_fail (TRACKER_IS_MINER (miner), FALSE);

	tracker_resources_batch_sparql_update (miner->private->client,
					       sparql, 
					       &internal_error);
	if (!internal_error) {
		return TRUE;
	}

	if (error) {
		g_propagate_error (error, internal_error);
	} else {
		g_warning ("Error running sparql queries: %s", internal_error->message);
		g_error_free (internal_error);
	}

	return FALSE;
}

/**
 * tracker_miner_commit:
 * @miner: a #TrackerMiner
 *
 * Commits all pending batch updates. see tracker_miner_execute_batch_update()
 *
 * Returns: #TRUE if the data was committed successfully.
 **/
gboolean
tracker_miner_commit (TrackerMiner *miner)
{
	GError *error = NULL;

	g_return_val_if_fail (TRACKER_IS_MINER (miner), FALSE);

	if (g_hash_table_size (miner->private->pauses) > 0) {
		g_warning ("Can not commit while miner is paused");
		return FALSE;
	}

	tracker_resources_batch_commit (miner->private->client, &error);

	if (error) {
		g_critical ("Could not commit: %s", error->message);
		g_error_free (error);
		return FALSE;
	}

	return TRUE;
}

gint
tracker_miner_pause (TrackerMiner  *miner,
		     const gchar   *application,
		     const gchar   *reason,
		     GError       **error)
{
	PauseData *pd;
	GHashTableIter iter;
	gpointer key, value;

	g_return_val_if_fail (TRACKER_IS_MINER (miner), -1);
	g_return_val_if_fail (application != NULL, -1);
	g_return_val_if_fail (reason != NULL, -1);

	/* Check this is not a duplicate pause */
	g_hash_table_iter_init (&iter, miner->private->pauses);
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

	pd = pause_data_new (application, reason);

	g_hash_table_insert (miner->private->pauses, 
			     GINT_TO_POINTER (pd->cookie),
			     pd);

	if (g_hash_table_size (miner->private->pauses) == 1) {
		/* Pause */
		g_message ("Miner is pausing");
		g_signal_emit (miner, signals[PAUSED], 0);
	}

	return pd->cookie;
}

/**
 * tracker_miner_resume:
 * @miner: a #TrackerMiner
 * @cookie: pause cookie
 * @error: return location for errors
 *
 * Asks the miner to resume processing. The cookie must be something
 * returned by tracker_miner_pause(). The miner won't actually resume
 * operations until all pause requests have been resumed.
 *
 * Returns: #TRUE if the cookie was valid.
 **/
gboolean 
tracker_miner_resume (TrackerMiner  *miner,
		      gint           cookie,
		      GError       **error)
{
	g_return_val_if_fail (TRACKER_IS_MINER (miner), FALSE);

	if (!g_hash_table_remove (miner->private->pauses, GINT_TO_POINTER (cookie))) {
		g_set_error_literal (error, TRACKER_MINER_ERROR, 0,
				     _("Cookie not recognized to resume paused miner"));
		return FALSE;
	}

	if (g_hash_table_size (miner->private->pauses) == 0) {
		/* Resume */
		g_message ("Miner is resuming");
		g_signal_emit (miner, signals[RESUMED], 0);
	}

	return TRUE;
}

/* DBus methods */
void
tracker_miner_dbus_get_status (TrackerMiner           *miner,
			       DBusGMethodInvocation  *context,
			       GError                **error)
{
	guint request_id;

	request_id = tracker_dbus_get_next_request_id ();

	tracker_dbus_async_return_if_fail (miner != NULL, context);

	tracker_dbus_request_new (request_id, "%s()", __PRETTY_FUNCTION__);

	dbus_g_method_return (context, miner->private->status);

	tracker_dbus_request_success (request_id);
}

void
tracker_miner_dbus_get_progress (TrackerMiner           *miner,
				 DBusGMethodInvocation  *context,
				 GError                **error)
{
	guint request_id;

	request_id = tracker_dbus_get_next_request_id ();

	tracker_dbus_async_return_if_fail (miner != NULL, context);

	tracker_dbus_request_new (request_id, "%s()", __PRETTY_FUNCTION__);

	dbus_g_method_return (context, miner->private->progress);

	tracker_dbus_request_success (request_id);
}

void
tracker_miner_dbus_get_pause_details (TrackerMiner           *miner,
				      DBusGMethodInvocation  *context,
				      GError                **error)
{
	GSList *applications, *reasons;
	GStrv applications_strv, reasons_strv;
	GHashTableIter iter;
	gpointer key, value;
	guint request_id;

	request_id = tracker_dbus_get_next_request_id ();

	tracker_dbus_async_return_if_fail (miner != NULL, context);

	tracker_dbus_request_new (request_id, "%s()", __PRETTY_FUNCTION__);

	applications = NULL;
	reasons = NULL;

	g_hash_table_iter_init (&iter, miner->private->pauses);
	while (g_hash_table_iter_next (&iter, &key, &value)) {
		PauseData *pd = value;

		applications = g_slist_prepend (applications, pd->application);
		reasons = g_slist_prepend (reasons, pd->reason);
	}

	applications = g_slist_reverse (applications);
	reasons = g_slist_reverse (reasons);

	applications_strv = tracker_gslist_to_string_list (applications);
	reasons_strv = tracker_gslist_to_string_list (reasons);

	dbus_g_method_return (context, applications_strv, reasons_strv);

	tracker_dbus_request_success (request_id);

	g_strfreev (applications_strv);
	g_strfreev (reasons_strv);
	
	g_slist_free (applications);
	g_slist_free (reasons);
}

void
tracker_miner_dbus_pause (TrackerMiner           *miner,
			  const gchar            *application,
			  const gchar            *reason,
			  DBusGMethodInvocation  *context,
			  GError                **error)
{
	GError *local_error = NULL;
	guint request_id;
	gint cookie;

	request_id = tracker_dbus_get_next_request_id ();

	tracker_dbus_async_return_if_fail (miner != NULL, context);
	tracker_dbus_async_return_if_fail (application != NULL, context);
	tracker_dbus_async_return_if_fail (reason != NULL, context);

	tracker_dbus_request_new (request_id, "%s(application:'%s', reason:'%s')",
				  __PRETTY_FUNCTION__,
				  application,
				  reason);

	cookie = tracker_miner_pause (miner, application, reason, &local_error);
	if (cookie == -1) {
		GError *actual_error = NULL;

		tracker_dbus_request_failed (request_id,
		                             &actual_error,
		                             local_error ? local_error->message : NULL);
		dbus_g_method_return_error (context, actual_error);
		g_error_free (actual_error);
		g_error_free (local_error);

		return;
	}

	dbus_g_method_return (context, cookie);

	tracker_dbus_request_success (request_id);
}

void
tracker_miner_dbus_resume (TrackerMiner           *miner,
			   gint                    cookie,
			   DBusGMethodInvocation  *context,
			   GError                **error)
{
	GError *local_error = NULL;
	guint request_id;

	request_id = tracker_dbus_get_next_request_id ();

	tracker_dbus_async_return_if_fail (miner != NULL, context);

	tracker_dbus_request_new (request_id, "%s(cookie:%d)", 
				  __PRETTY_FUNCTION__,
				  cookie);

	if (!tracker_miner_resume (miner, cookie, &local_error)) {
		GError *actual_error = NULL;

		tracker_dbus_request_failed (request_id,
		                             &actual_error,
		                             local_error ? local_error->message : NULL);
		dbus_g_method_return_error (context, actual_error);
		g_error_free (actual_error);
		g_error_free (local_error);

		return;
	}

	dbus_g_method_return (context);

	tracker_dbus_request_success (request_id);
}
