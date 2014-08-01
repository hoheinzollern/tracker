/*
 * Copyright (C) 2008, Nokia <ivan.frade@nokia.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */

#include "config.h"

#include <sys/statvfs.h>
#include <fcntl.h>
#ifdef __linux__
#include <sys/ioctl.h>
#include <linux/msdos_fs.h>
#endif /* __linux__ */
#include <unistd.h>

#include <glib/gi18n.h>
#include <glib/gstdio.h>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <gio/gunixinputstream.h>

#include <libtracker-common/tracker-dbus.h>
#include <libtracker-common/tracker-date-time.h>
#include <libtracker-common/tracker-ontologies.h>
#include <libtracker-common/tracker-type-utils.h>
#include <libtracker-common/tracker-utils.h>
#include <libtracker-common/tracker-file-utils.h>
#include <libtracker-common/tracker-storage.h>

#include <libtracker-data/tracker-db-manager.h>

#include <libtracker-extract/tracker-module-manager.h>
#include <libtracker-extract/tracker-extract-client.h>

#include "tracker-power.h"
#include "tracker-miner-files.h"
#include "tracker-config.h"

#define DISK_SPACE_CHECK_FREQUENCY 10
#define SECONDS_PER_DAY 86400

#define TRACKER_MINER_FILES_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), TRACKER_TYPE_MINER_FILES, TrackerMinerFilesPrivate))

static GQuark miner_files_error_quark = 0;

typedef struct ProcessFileData ProcessFileData;

struct ProcessFileData {
	TrackerMinerFiles *miner;
	TrackerSparqlBuilder *sparql;
	GCancellable *cancellable;
	GFile *file;
	gchar *mime_type;
};

struct TrackerMinerFilesPrivate {
	TrackerConfig *config;
	TrackerStorage *storage;

	GVolumeMonitor *volume_monitor;

	GSList *index_recursive_directories;
	GSList *index_single_directories;

	guint disk_space_check_id;
	guint disk_space_pause_cookie;

	guint low_battery_pause_cookie;

#if defined(HAVE_UPOWER) || defined(HAVE_HAL)
	TrackerPower *power;
#endif /* defined(HAVE_UPOWER) || defined(HAVE_HAL) */
	gulong finished_handler;

	GDBusConnection *connection;

	GQuark quark_mount_point_uuid;

	guint force_recheck_id;

	gboolean index_removable_devices;
	gboolean index_optical_discs;
	guint volumes_changed_id;

	gboolean mount_points_initialized;

	guint stale_volumes_check_id;

	GList *extraction_queue;
};

enum {
	VOLUME_MOUNTED_IN_STORE = 1 << 0,
	VOLUME_MOUNTED = 1 << 1
};

enum {
	PROP_0,
	PROP_CONFIG
};

static void        miner_files_set_property             (GObject              *object,
                                                         guint                 param_id,
                                                         const GValue         *value,
                                                         GParamSpec           *pspec);
static void        miner_files_get_property             (GObject              *object,
                                                         guint                 param_id,
                                                         GValue               *value,
                                                         GParamSpec           *pspec);
static void        miner_files_finalize                 (GObject              *object);
static void        miner_files_initable_iface_init      (GInitableIface       *iface);
static gboolean    miner_files_initable_init            (GInitable            *initable,
                                                         GCancellable         *cancellable,
                                                         GError              **error);
static void        mount_pre_unmount_cb                 (GVolumeMonitor       *volume_monitor,
                                                         GMount               *mount,
                                                         TrackerMinerFiles    *mf);

static void        mount_point_added_cb                 (TrackerStorage       *storage,
                                                         const gchar          *uuid,
                                                         const gchar          *mount_point,
                                                         const gchar          *mount_name,
                                                         gboolean              removable,
                                                         gboolean              optical,
                                                         gpointer              user_data);
static void        mount_point_removed_cb               (TrackerStorage       *storage,
                                                         const gchar          *uuid,
                                                         const gchar          *mount_point,
                                                         gpointer              user_data);
#if defined(HAVE_UPOWER) || defined(HAVE_HAL)
static void        check_battery_status                 (TrackerMinerFiles    *fs);
static void        battery_status_cb                    (GObject              *object,
                                                         GParamSpec           *pspec,
                                                         gpointer              user_data);
static void        index_on_battery_cb                  (GObject    *object,
                                                         GParamSpec *pspec,
                                                         gpointer    user_data);
#endif /* defined(HAVE_UPOWER) || defined(HAVE_HAL) */
static void        init_mount_points                    (TrackerMinerFiles    *miner);
static void        init_stale_volume_removal            (TrackerMinerFiles    *miner);
static void        disk_space_check_start               (TrackerMinerFiles    *mf);
static void        disk_space_check_stop                (TrackerMinerFiles    *mf);
static void        low_disk_space_limit_cb              (GObject              *gobject,
                                                         GParamSpec           *arg1,
                                                         gpointer              user_data);
static void        index_recursive_directories_cb       (GObject              *gobject,
                                                         GParamSpec           *arg1,
                                                         gpointer              user_data);
static void        index_single_directories_cb          (GObject              *gobject,
                                                         GParamSpec           *arg1,
                                                         gpointer              user_data);
static gboolean    miner_files_force_recheck_idle       (gpointer user_data);
static void        trigger_recheck_cb                   (GObject              *gobject,
                                                         GParamSpec           *arg1,
                                                         gpointer              user_data);
static void        index_volumes_changed_cb             (GObject              *gobject,
                                                         GParamSpec           *arg1,
                                                         gpointer              user_data);
static gboolean    miner_files_process_file             (TrackerMinerFS       *fs,
                                                         GFile                *file,
                                                         TrackerSparqlBuilder *sparql,
                                                         GCancellable         *cancellable);
static gboolean    miner_files_process_file_attributes  (TrackerMinerFS       *fs,
                                                         GFile                *file,
                                                         TrackerSparqlBuilder *sparql,
                                                         GCancellable         *cancellable);
static gboolean    miner_files_ignore_next_update_file  (TrackerMinerFS       *fs,
                                                         GFile                *file,
                                                         TrackerSparqlBuilder *sparql,
                                                         GCancellable         *cancellable);
static void        miner_files_finished                 (TrackerMinerFS       *fs);

static void        miner_finished_cb                    (TrackerMinerFS *fs,
                                                         gdouble         seconds_elapsed,
                                                         guint           total_directories_found,
                                                         guint           total_directories_ignored,
                                                         guint           total_files_found,
                                                         guint           total_files_ignored,
                                                         gpointer        user_data);

static gboolean    miner_files_in_removable_media_remove_by_type  (TrackerMinerFiles  *miner,
                                                                   TrackerStorageType  type);
static void        miner_files_in_removable_media_remove_by_date  (TrackerMinerFiles  *miner,
                                                                   const gchar        *date);

static void        miner_files_add_removable_or_optical_directory (TrackerMinerFiles *mf,
                                                                   const gchar       *mount_path,
                                                                   const gchar       *uuid);

static void        miner_files_update_filters                     (TrackerMinerFiles *files);


static GInitableIface* miner_files_initable_parent_iface;

G_DEFINE_TYPE_WITH_CODE (TrackerMinerFiles, tracker_miner_files, TRACKER_TYPE_MINER_FS,
                         G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE,
                                                miner_files_initable_iface_init));

static void
tracker_miner_files_class_init (TrackerMinerFilesClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	TrackerMinerFSClass *miner_fs_class = TRACKER_MINER_FS_CLASS (klass);

	object_class->finalize = miner_files_finalize;
	object_class->get_property = miner_files_get_property;
	object_class->set_property = miner_files_set_property;

	miner_fs_class->process_file = miner_files_process_file;
	miner_fs_class->process_file_attributes = miner_files_process_file_attributes;
	miner_fs_class->ignore_next_update_file = miner_files_ignore_next_update_file;
	miner_fs_class->finished = miner_files_finished;

	g_object_class_install_property (object_class,
	                                 PROP_CONFIG,
	                                 g_param_spec_object ("config",
	                                                      "Config",
	                                                      "Config",
	                                                      TRACKER_TYPE_CONFIG,
	                                                      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

	g_type_class_add_private (klass, sizeof (TrackerMinerFilesPrivate));

	miner_files_error_quark = g_quark_from_static_string ("TrackerMinerFiles");
}

static void
tracker_miner_files_init (TrackerMinerFiles *mf)
{
	TrackerMinerFilesPrivate *priv;

	priv = mf->private = TRACKER_MINER_FILES_GET_PRIVATE (mf);

	priv->storage = tracker_storage_new ();

	g_signal_connect (priv->storage, "mount-point-added",
	                  G_CALLBACK (mount_point_added_cb),
	                  mf);

	g_signal_connect (priv->storage, "mount-point-removed",
	                  G_CALLBACK (mount_point_removed_cb),
	                  mf);

#if defined(HAVE_UPOWER) || defined(HAVE_HAL)
	priv->power = tracker_power_new ();

	g_signal_connect (priv->power, "notify::on-low-battery",
	                  G_CALLBACK (battery_status_cb),
	                  mf);
	g_signal_connect (priv->power, "notify::on-battery",
	                  G_CALLBACK (battery_status_cb),
	                  mf);
#endif /* defined(HAVE_UPOWER) || defined(HAVE_HAL) */

	priv->finished_handler = g_signal_connect_after (mf, "finished",
	                                                 G_CALLBACK (miner_finished_cb),
	                                                 NULL);

	priv->volume_monitor = g_volume_monitor_get ();
	g_signal_connect (priv->volume_monitor, "mount-pre-unmount",
	                  G_CALLBACK (mount_pre_unmount_cb),
	                  mf);

	priv->quark_mount_point_uuid = g_quark_from_static_string ("tracker-mount-point-uuid");
}

static void
miner_files_initable_iface_init (GInitableIface *iface)
{
	miner_files_initable_parent_iface = g_type_interface_peek_parent (iface);
	iface->init = miner_files_initable_init;
}

static gboolean
miner_files_initable_init (GInitable     *initable,
                           GCancellable  *cancellable,
                           GError       **error)
{
	TrackerMinerFiles *mf;
	TrackerMinerFS *fs;
	TrackerIndexingTree *indexing_tree;
	TrackerDirectoryFlags flags;
	GError *inner_error = NULL;
	GSList *mounts = NULL;
	GSList *dirs;
	GSList *m;

	mf = TRACKER_MINER_FILES (initable);
	fs = TRACKER_MINER_FS (initable);
	indexing_tree = tracker_miner_fs_get_indexing_tree (fs);
	tracker_indexing_tree_set_filter_hidden (indexing_tree, TRUE);

	miner_files_update_filters (mf);

	/* Chain up parent's initable callback before calling child's one */
	if (!miner_files_initable_parent_iface->init (initable, cancellable, &inner_error)) {
		g_propagate_error (error, inner_error);
		return FALSE;
	}

	/* Set up extractor and signals */
	mf->private->connection =  g_bus_get_sync (TRACKER_IPC_BUS, NULL, &inner_error);
	if (!mf->private->connection) {
		g_propagate_error (error, inner_error);
		g_prefix_error (error,
		                "Could not connect to the D-Bus session bus. ");
		return FALSE;
	}

	/* We must have a configuration setup here */
	if (G_UNLIKELY (!mf->private->config)) {
		g_set_error (error,
		             TRACKER_MINER_ERROR,
		             0,
		             "No config set for miner %s",
		             G_OBJECT_TYPE_NAME (mf));
		return FALSE;
	}

	/* Setup mount points, we MUST have config set up before we
	 * init mount points because the config is used in that
	 * function.
	 */
	mf->private->index_removable_devices = tracker_config_get_index_removable_devices (mf->private->config);

	/* Note that if removable devices not indexed, optical discs
	 * will also never be indexed */
	mf->private->index_optical_discs = (mf->private->index_removable_devices ?
	                                    tracker_config_get_index_optical_discs (mf->private->config) :
	                                    FALSE);

	init_mount_points (mf);

	/* If this happened AFTER we have initialized mount points, initialize
	 * stale volume removal now. */
	if (mf->private->mount_points_initialized) {
		init_stale_volume_removal (mf);
	}

	if (mf->private->index_removable_devices) {
		/* Get list of roots for removable devices (excluding optical) */
		mounts = tracker_storage_get_device_roots (mf->private->storage,
		                                           TRACKER_STORAGE_REMOVABLE,
		                                           TRUE);
	}

	if (mf->private->index_optical_discs) {
		/* Get list of roots for removable+optical devices */
		m = tracker_storage_get_device_roots (mf->private->storage,
		                                      TRACKER_STORAGE_OPTICAL | TRACKER_STORAGE_REMOVABLE,
		                                      TRUE);
		mounts = g_slist_concat (mounts, m);
	}

#if defined(HAVE_UPOWER) || defined(HAVE_HAL)
	check_battery_status (mf);
#endif /* defined(HAVE_UPOWER) || defined(HAVE_HAL) */

	g_message ("Setting up directories to iterate from config (IndexSingleDirectory)");

	/* Fill in directories to inspect */
	dirs = tracker_config_get_index_single_directories (mf->private->config);

	/* Copy in case of config changes */
	mf->private->index_single_directories = tracker_gslist_copy_with_string_data (dirs);

	for (; dirs; dirs = dirs->next) {
		GFile *file;

		/* Do some simple checks for silly locations */
		if (strcmp (dirs->data, "/dev") == 0 ||
		    strcmp (dirs->data, "/lib") == 0 ||
		    strcmp (dirs->data, "/proc") == 0 ||
		    strcmp (dirs->data, "/sys") == 0) {
			continue;
		}

		if (g_str_has_prefix (dirs->data, g_get_tmp_dir ())) {
			continue;
		}

		/* Make sure we don't crawl volumes. */
		if (mounts) {
			gboolean found = FALSE;

			for (m = mounts; m && !found; m = m->next) {
				found = strcmp (m->data, dirs->data) == 0;
			}

			if (found) {
				g_message ("  Duplicate found:'%s' - same as removable device path",
				           (gchar*) dirs->data);
				continue;
			}
		}

		g_message ("  Adding:'%s'", (gchar*) dirs->data);

		file = g_file_new_for_path (dirs->data);

		flags = TRACKER_DIRECTORY_FLAG_NONE;

		if (tracker_config_get_enable_monitors (mf->private->config)) {
			flags |= TRACKER_DIRECTORY_FLAG_MONITOR;
		}

		if (tracker_miner_fs_get_mtime_checking (TRACKER_MINER_FS (mf))) {
			flags |= TRACKER_DIRECTORY_FLAG_CHECK_MTIME;
		}

		tracker_indexing_tree_add (indexing_tree, file, flags);
		g_object_unref (file);
	}

	g_message ("Setting up directories to iterate from config (IndexRecursiveDirectory)");

	dirs = tracker_config_get_index_recursive_directories (mf->private->config);

	/* Copy in case of config changes */
	mf->private->index_recursive_directories = tracker_gslist_copy_with_string_data (dirs);

	for (; dirs; dirs = dirs->next) {
		GFile *file;

		/* Do some simple checks for silly locations */
		if (strcmp (dirs->data, "/dev") == 0 ||
		    strcmp (dirs->data, "/lib") == 0 ||
		    strcmp (dirs->data, "/proc") == 0 ||
		    strcmp (dirs->data, "/sys") == 0) {
			continue;
		}

		if (g_str_has_prefix (dirs->data, g_get_tmp_dir ())) {
			continue;
		}

		/* Make sure we don't crawl volumes. */
		if (mounts) {
			gboolean found = FALSE;

			for (m = mounts; m && !found; m = m->next) {
				found = strcmp (m->data, dirs->data) == 0;
			}

			if (found) {
				g_message ("  Duplicate found:'%s' - same as removable device path",
				           (gchar*) dirs->data);
				continue;
			}
		}

		g_message ("  Adding:'%s'", (gchar*) dirs->data);

		file = g_file_new_for_path (dirs->data);

		flags = TRACKER_DIRECTORY_FLAG_RECURSE;

		if (tracker_config_get_enable_monitors (mf->private->config)) {
			flags |= TRACKER_DIRECTORY_FLAG_MONITOR;
		}

		if (tracker_miner_fs_get_mtime_checking (TRACKER_MINER_FS (mf))) {
			flags |= TRACKER_DIRECTORY_FLAG_CHECK_MTIME;
		}

		tracker_indexing_tree_add (indexing_tree, file, flags);
		g_object_unref (file);
	}

	/* Add mounts */
	g_message ("Setting up directories to iterate from devices/discs");

	if (!mf->private->index_removable_devices) {
		g_message ("  Removable devices are disabled in the config");

		/* Make sure we don't have any resource in a volume of the given type */
		miner_files_in_removable_media_remove_by_type (mf, TRACKER_STORAGE_REMOVABLE);
	}

	if (!mf->private->index_optical_discs) {
		g_message ("  Optical discs are disabled in the config");

		/* Make sure we don't have any resource in a volume of the given type */
		miner_files_in_removable_media_remove_by_type (mf, TRACKER_STORAGE_REMOVABLE | TRACKER_STORAGE_OPTICAL);
	}

	for (m = mounts; m; m = m->next) {
		miner_files_add_removable_or_optical_directory (mf,
		                                                (gchar *) m->data,
		                                                NULL);
	}

	/* We want to get notified when config changes */

	g_signal_connect (mf->private->config, "notify::low-disk-space-limit",
	                  G_CALLBACK (low_disk_space_limit_cb),
	                  mf);
	g_signal_connect (mf->private->config, "notify::index-recursive-directories",
	                  G_CALLBACK (index_recursive_directories_cb),
	                  mf);
	g_signal_connect (mf->private->config, "notify::index-single-directories",
	                  G_CALLBACK (index_single_directories_cb),
	                  mf);
	g_signal_connect (mf->private->config, "notify::ignored-directories",
	                  G_CALLBACK (trigger_recheck_cb),
	                  mf);
	g_signal_connect (mf->private->config, "notify::ignored-directories-with-content",
	                  G_CALLBACK (trigger_recheck_cb),
	                  mf);
	g_signal_connect (mf->private->config, "notify::ignored-files",
	                  G_CALLBACK (trigger_recheck_cb),
	                  mf);
	g_signal_connect (mf->private->config, "notify::index-removable-devices",
	                  G_CALLBACK (index_volumes_changed_cb),
	                  mf);
	g_signal_connect (mf->private->config, "notify::index-optical-discs",
	                  G_CALLBACK (index_volumes_changed_cb),
	                  mf);
	g_signal_connect (mf->private->config, "notify::removable-days-threshold",
	                  G_CALLBACK (index_volumes_changed_cb),
	                  mf);

#if defined(HAVE_UPOWER) || defined(HAVE_HAL)

	g_signal_connect (mf->private->config, "notify::index-on-battery",
	                  G_CALLBACK (index_on_battery_cb),
	                  mf);
	g_signal_connect (mf->private->config, "notify::index-on-battery-first-time",
	                  G_CALLBACK (index_on_battery_cb),
	                  mf);

#endif /* defined(HAVE_UPOWER) || defined(HAVE_HAL) */

	g_slist_foreach (mounts, (GFunc) g_free, NULL);
	g_slist_free (mounts);

	disk_space_check_start (mf);

	return TRUE;
}

static void
miner_files_set_property (GObject      *object,
                          guint         prop_id,
                          const GValue *value,
                          GParamSpec   *pspec)
{
	TrackerMinerFilesPrivate *priv;

	priv = TRACKER_MINER_FILES_GET_PRIVATE (object);

	switch (prop_id) {
	case PROP_CONFIG:
		priv->config = g_value_dup_object (value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
miner_files_get_property (GObject    *object,
                          guint       prop_id,
                          GValue     *value,
                          GParamSpec *pspec)
{
	TrackerMinerFilesPrivate *priv;

	priv = TRACKER_MINER_FILES_GET_PRIVATE (object);

	switch (prop_id) {
	case PROP_CONFIG:
		g_value_set_object (value, priv->config);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
miner_files_finalize (GObject *object)
{
	TrackerMinerFiles *mf;
	TrackerMinerFilesPrivate *priv;

	mf = TRACKER_MINER_FILES (object);
	priv = mf->private;

	if (priv->config) {
		g_signal_handlers_disconnect_by_func (priv->config,
		                                      low_disk_space_limit_cb,
		                                      NULL);
		g_object_unref (priv->config);
	}

	disk_space_check_stop (TRACKER_MINER_FILES (object));

	if (priv->index_recursive_directories) {
		g_slist_foreach (priv->index_recursive_directories, (GFunc) g_free, NULL);
		g_slist_free (priv->index_recursive_directories);
	}

	if (priv->index_single_directories) {
		g_slist_foreach (priv->index_single_directories, (GFunc) g_free, NULL);
		g_slist_free (priv->index_single_directories);
	}

#if defined(HAVE_UPOWER) || defined(HAVE_HAL)
	if (priv->power) {
		g_object_unref (priv->power);
	}
#endif /* defined(HAVE_UPOWER) || defined(HAVE_HAL) */

	if (priv->storage) {
		g_object_unref (priv->storage);
	}

	if (priv->volume_monitor) {
		g_signal_handlers_disconnect_by_func (priv->volume_monitor,
		                                      mount_pre_unmount_cb,
		                                      object);
		g_object_unref (priv->volume_monitor);
	}

	if (priv->force_recheck_id) {
		g_source_remove (priv->force_recheck_id);
		priv->force_recheck_id = 0;
	}

	if (priv->stale_volumes_check_id) {
		g_source_remove (priv->stale_volumes_check_id);
		priv->stale_volumes_check_id = 0;
	}

	g_list_free (priv->extraction_queue);

	G_OBJECT_CLASS (tracker_miner_files_parent_class)->finalize (object);
}

static void
ensure_mount_point_exists (TrackerMinerFiles *miner,
                           GFile             *mount_point,
                           GString           *accumulator)
{
	gchar *iri;
	gchar *uri;

	uri = g_file_get_uri (mount_point);

	/* Query the store for the URN of the mount point */
	iri = tracker_miner_fs_query_urn (TRACKER_MINER_FS (miner),
	                                  mount_point);

	if (iri) {
		/* If exists, just return, nothing else to do */
		g_message ("Mount point '%s' already exists in store: '%s'",
		           uri, iri);
		g_free (iri);
	} else {
		/* If it doesn't exist, we need to create it */
		g_message ("Mount point '%s' does not exist in store, need to create it",
		           uri);

		/* Create a nfo:Folder for the mount point */
		g_string_append_printf (accumulator,
		                        "INSERT SILENT INTO <" TRACKER_MINER_FS_GRAPH_URN "> {"
		                        " _:file a nfo:FileDataObject, nie:InformationElement, nfo:Folder ; "
		                        "        nie:isStoredAs _:file ; "
		                        "        nie:url \"%s\" ; "
		                        "        nie:mimeType \"inode/directory\" ; "
		                        "        nfo:fileLastModified \"1981-06-05T02:20:00Z\" . "
		                        "}",
		                        uri);
	}

	g_free (uri);
}

static void
set_up_mount_point_cb (GObject      *source,
                       GAsyncResult *result,
                       gpointer      user_data)
{
	TrackerSparqlConnection *connection = TRACKER_SPARQL_CONNECTION (source);
	gchar *removable_device_urn = user_data;
	GError *error = NULL;

	tracker_sparql_connection_update_finish (connection, result, &error);

	if (error) {
		g_critical ("Could not set mount point in database '%s', %s",
		            removable_device_urn,
		            error->message);
		g_error_free (error);
	}

	g_free (removable_device_urn);
}

static void
set_up_mount_point_type (TrackerMinerFiles *miner,
                         const gchar       *removable_device_urn,
                         gboolean           removable,
                         gboolean           optical,
                         GString           *accumulator)
{
	if (!accumulator) {
		return;
	}

	g_debug ("Mount point type being set in DB for URN '%s'",
	         removable_device_urn);

	g_string_append_printf (accumulator,
	                        "DELETE { <%s> tracker:isRemovable ?unknown } WHERE { <%s> a tracker:Volume; tracker:isRemovable ?unknown } ",
	                        removable_device_urn, removable_device_urn);

	g_string_append_printf (accumulator,
	                        "INSERT INTO <%s> { <%s> a tracker:Volume; tracker:isRemovable %s } ",
	                        removable_device_urn, removable_device_urn, removable ? "true" : "false");

	g_string_append_printf (accumulator,
	                        "DELETE { <%s> tracker:isOptical ?unknown } WHERE { <%s> a tracker:Volume; tracker:isOptical ?unknown } ",
	                        removable_device_urn, removable_device_urn);

	g_string_append_printf (accumulator,
	                        "INSERT INTO <%s> { <%s> a tracker:Volume; tracker:isOptical %s } ",
	                        removable_device_urn, removable_device_urn, optical ? "true" : "false");
}

static void
set_up_mount_point (TrackerMinerFiles *miner,
                    const gchar       *removable_device_urn,
                    const gchar       *mount_point,
                    const gchar       *mount_name,
                    gboolean           mounted,
                    GString           *accumulator)
{
	GString *queries;

	queries = g_string_new (NULL);

	if (mounted) {
		g_debug ("Mount point state (MOUNTED) being set in DB for URN '%s' (mount_point: %s)",
		         removable_device_urn,
		         mount_point ? mount_point : "unknown");

		if (mount_point) {
			GFile *file;
			gchar *uri;

			file = g_file_new_for_path (mount_point);
			uri = g_file_get_uri (file);

			/* Before assigning a nfo:FileDataObject as tracker:mountPoint for
			 * the volume, make sure the nfo:FileDataObject exists in the store */
			ensure_mount_point_exists (miner, file, queries);

			g_string_append_printf (queries,
			                        "DELETE { "
			                        "  <%s> tracker:mountPoint ?u "
			                        "} WHERE { "
			                        "  ?u a nfo:FileDataObject; "
			                        "     nie:url \"%s\" "
			                        "} ",
			                        removable_device_urn, uri);

			g_string_append_printf (queries,
			                        "DELETE { <%s> a rdfs:Resource }  "
			                        "INSERT { "
			                        "  <%s> a tracker:Volume; "
			                        "       tracker:mountPoint ?u "
			                        "} WHERE { "
			                        "  ?u a nfo:FileDataObject; "
			                        "     nie:url \"%s\" "
			                        "} ",
			                        removable_device_urn, removable_device_urn, uri);

			g_object_unref (file);
			g_free (uri);
		}

		g_string_append_printf (queries,
		                        "DELETE { <%s> tracker:isMounted ?unknown } WHERE { <%s> a tracker:Volume; tracker:isMounted ?unknown } ",
		                        removable_device_urn, removable_device_urn);

                if (mount_name) {
                        g_string_append_printf (queries,
                                                "INSERT INTO <%s> { <%s> a tracker:Volume; tracker:isMounted true; nie:title \"%s\" } ",
                                                removable_device_urn, removable_device_urn, mount_name);
                } else {
                        g_string_append_printf (queries,
                                                "INSERT INTO <%s> { <%s> a tracker:Volume; tracker:isMounted true } ",
                                                removable_device_urn, removable_device_urn);
                }

                g_string_append_printf (queries,
                                        "INSERT { GRAPH <%s> { ?do tracker:available true } } WHERE { ?do nie:dataSource <%s> } ",
                                        removable_device_urn, removable_device_urn);
	} else {
		gchar *now;

		g_debug ("Mount point state (UNMOUNTED) being set in DB for URN '%s'",
		         removable_device_urn);

		now = tracker_date_to_string (time (NULL));

		g_string_append_printf (queries,
		                        "DELETE { <%s> tracker:unmountDate ?unknown } WHERE { <%s> a tracker:Volume; tracker:unmountDate ?unknown } ",
		                        removable_device_urn, removable_device_urn);

		g_string_append_printf (queries,
		                        "INSERT INTO <%s> { <%s> a tracker:Volume; tracker:unmountDate \"%s\" } ",
		                        removable_device_urn, removable_device_urn, now);

		g_string_append_printf (queries,
		                        "DELETE { <%s> tracker:isMounted ?unknown } WHERE { <%s> a tracker:Volume; tracker:isMounted ?unknown } ",
		                        removable_device_urn, removable_device_urn);

		g_string_append_printf (queries,
		                        "INSERT INTO <%s> { <%s> a tracker:Volume; tracker:isMounted false } ",
		                        removable_device_urn, removable_device_urn);

		g_string_append_printf (queries,
		                        "DELETE { ?do tracker:available true } WHERE { ?do nie:dataSource <%s> } ",
		                        removable_device_urn);

		g_free (now);
	}

	if (accumulator) {
		g_string_append_printf (accumulator, "%s ", queries->str);
	} else {
		tracker_sparql_connection_update_async (tracker_miner_get_connection (TRACKER_MINER (miner)),
		                                        queries->str,
		                                        G_PRIORITY_LOW,
		                                        NULL,
		                                        set_up_mount_point_cb,
		                                        g_strdup (removable_device_urn));
	}

	g_string_free (queries, TRUE);
}

static void
init_mount_points_cb (GObject      *source,
                      GAsyncResult *result,
                      gpointer      user_data)
{
	GError *error = NULL;

	tracker_sparql_connection_update_finish (TRACKER_SPARQL_CONNECTION (source),
	                                         result,
	                                         &error);

	if (error) {
		g_critical ("Could not initialize currently active mount points: %s",
		            error->message);
		g_error_free (error);
	} else {
		/* Mount points correctly initialized */
		(TRACKER_MINER_FILES (user_data))->private->mount_points_initialized = TRUE;
		/* If this happened AFTER we have a proper config, initialize
		 * stale volume removal now. */
		if ((TRACKER_MINER_FILES (user_data))->private->config) {
			init_stale_volume_removal (TRACKER_MINER_FILES (user_data));
		}
	}
}

static void
init_mount_points (TrackerMinerFiles *miner_files)
{
	TrackerMiner *miner = TRACKER_MINER (miner_files);
	TrackerMinerFilesPrivate *priv;
	GHashTable *volumes;
	GHashTableIter iter;
	gpointer key, value;
	GString *accumulator;
	GError *error = NULL;
	TrackerSparqlCursor *cursor;
	GSList *uuids, *u;

	g_debug ("Initializing mount points...");

	/* First, get all mounted volumes, according to tracker-store (SYNC!) */
	cursor = tracker_sparql_connection_query (tracker_miner_get_connection (miner),
	                                          "SELECT ?v WHERE { ?v a tracker:Volume ; tracker:isMounted true }",
	                                          NULL, &error);
	if (error) {
		g_critical ("Could not obtain the mounted volumes: %s", error->message);
		g_error_free (error);
		return;
	}

	priv = TRACKER_MINER_FILES_GET_PRIVATE (miner);

	volumes = g_hash_table_new_full (g_str_hash, g_str_equal,
	                                 (GDestroyNotify) g_free,
	                                 NULL);


	/* Make sure the root partition is always set to mounted, as GIO won't
	 * report it as a proper mount */
	g_hash_table_insert (volumes,
	                     g_strdup (TRACKER_NON_REMOVABLE_MEDIA_DATASOURCE_URN),
	                     GINT_TO_POINTER (VOLUME_MOUNTED));

	while (tracker_sparql_cursor_next (cursor, NULL, NULL)) {
		gint state;
		const gchar *urn;

		state = VOLUME_MOUNTED_IN_STORE;

		urn = tracker_sparql_cursor_get_string (cursor, 0, NULL);

		if (strcmp (urn, TRACKER_NON_REMOVABLE_MEDIA_DATASOURCE_URN) == 0) {
			/* Report non-removable media to be mounted by HAL as well */
			state |= VOLUME_MOUNTED;
		}

		g_hash_table_replace (volumes, g_strdup (urn), GINT_TO_POINTER (state));
	}

	g_object_unref (cursor);

	/* Then, get all currently mounted non-REMOVABLE volumes, according to GIO */
	uuids = tracker_storage_get_device_uuids (priv->storage, 0, TRUE);
	for (u = uuids; u; u = u->next) {
		const gchar *uuid;
		gchar *non_removable_device_urn;
		gint state;

		uuid = u->data;
		non_removable_device_urn = g_strdup_printf (TRACKER_DATASOURCE_URN_PREFIX "%s", uuid);

		state = GPOINTER_TO_INT (g_hash_table_lookup (volumes, non_removable_device_urn));
		state |= VOLUME_MOUNTED;

		g_hash_table_replace (volumes, non_removable_device_urn, GINT_TO_POINTER (state));
	}

	g_slist_foreach (uuids, (GFunc) g_free, NULL);
	g_slist_free (uuids);

	/* Then, get all currently mounted REMOVABLE volumes, according to GIO */
	if (priv->index_removable_devices) {
		uuids = tracker_storage_get_device_uuids (priv->storage, TRACKER_STORAGE_REMOVABLE, FALSE);
		for (u = uuids; u; u = u->next) {
			const gchar *uuid;
			gchar *removable_device_urn;
			gint state;

			uuid = u->data;
			removable_device_urn = g_strdup_printf (TRACKER_DATASOURCE_URN_PREFIX "%s", uuid);

			state = GPOINTER_TO_INT (g_hash_table_lookup (volumes, removable_device_urn));
			state |= VOLUME_MOUNTED;

			g_hash_table_replace (volumes, removable_device_urn, GINT_TO_POINTER (state));
		}

		g_slist_foreach (uuids, (GFunc) g_free, NULL);
		g_slist_free (uuids);
	}

	accumulator = g_string_new (NULL);
	g_hash_table_iter_init (&iter, volumes);

	/* Finally, set up volumes based on the composed info */
	while (g_hash_table_iter_next (&iter, &key, &value)) {
		const gchar *urn = key;
		gint state = GPOINTER_TO_INT (value);

		if ((state & VOLUME_MOUNTED) &&
		    !(state & VOLUME_MOUNTED_IN_STORE)) {
			const gchar *mount_point = NULL;
			TrackerStorageType type = 0;

			/* Note: is there any case where the urn doesn't have our
			 *  datasource prefix? */
			if (g_str_has_prefix (urn, TRACKER_DATASOURCE_URN_PREFIX)) {
				const gchar *uuid;

				uuid = urn + strlen (TRACKER_DATASOURCE_URN_PREFIX);
				mount_point = tracker_storage_get_mount_point_for_uuid (priv->storage, uuid);
				type = tracker_storage_get_type_for_uuid (priv->storage, uuid);
			}

			if (urn) {
				if (mount_point) {
					g_debug ("Mount point state incorrect in DB for URN '%s', "
					         "currently it is mounted on '%s'",
					         urn,
					         mount_point);
				} else {
					g_debug ("Mount point state incorrect in DB for URN '%s', "
					         "currently it is mounted",
					         urn);
				}

				/* Set mount point state */
				set_up_mount_point (TRACKER_MINER_FILES (miner),
				                    urn,
				                    mount_point,
                                                    NULL,
				                    TRUE,
				                    accumulator);

				/* Set mount point type */
				set_up_mount_point_type (TRACKER_MINER_FILES (miner),
				                         urn,
				                         TRACKER_STORAGE_TYPE_IS_REMOVABLE (type),
				                         TRACKER_STORAGE_TYPE_IS_OPTICAL (type),
				                         accumulator);

				if (mount_point) {
					TrackerIndexingTree *indexing_tree;
					TrackerDirectoryFlags flags;
					GFile *file;

					indexing_tree = tracker_miner_fs_get_indexing_tree (TRACKER_MINER_FS (miner));
					flags = TRACKER_DIRECTORY_FLAG_RECURSE |
						TRACKER_DIRECTORY_FLAG_CHECK_MTIME |
						TRACKER_DIRECTORY_FLAG_PRESERVE;

					if (tracker_config_get_enable_monitors (miner_files->private->config)) {
						flags |= TRACKER_DIRECTORY_FLAG_MONITOR;
					}

					/* Add the current mount point as reported to have incorrect
					 * state. We will force mtime checks on this mount points,
					 * even if no-mtime-check-needed was set. */
					file = g_file_new_for_path (mount_point);
					if (tracker_miner_files_is_file_eligible (miner_files, file)) {
						tracker_indexing_tree_add (indexing_tree,
									   file,
									   flags);
					}
					g_object_unref (file);
				}
			}
		} else if (!(state & VOLUME_MOUNTED) &&
		           (state & VOLUME_MOUNTED_IN_STORE)) {
			if (urn) {
				g_debug ("Mount point state incorrect in DB for URN '%s', "
				         "currently it is NOT mounted",
				         urn);
				set_up_mount_point (TRACKER_MINER_FILES (miner),
				                    urn,
				                    NULL,
                                                    NULL,
				                    FALSE,
				                    accumulator);
				/* There's no need to force mtime check in these inconsistent
				 * mount points, as they are not mounted right now. */
			}
		}
	}

	if (accumulator->str[0] != '\0') {
		tracker_sparql_connection_update_async (tracker_miner_get_connection (miner),
		                                        accumulator->str,
		                                        G_PRIORITY_LOW,
		                                        NULL,
		                                        init_mount_points_cb,
		                                        miner);
	} else {
		/* Note. Not initializing stale volume removal timeout because
		 * we do not have the configuration setup yet */
		(TRACKER_MINER_FILES (miner))->private->mount_points_initialized = TRUE;
	}

	g_string_free (accumulator, TRUE);
	g_hash_table_unref (volumes);
}

static gboolean
cleanup_stale_removable_volumes_cb (gpointer user_data)
{
	TrackerMinerFiles *miner = TRACKER_MINER_FILES (user_data);
	gint n_days_threshold;
	time_t n_days_ago;
	gchar *n_days_ago_as_string;

	n_days_threshold = tracker_config_get_removable_days_threshold (miner->private->config);

	if (n_days_threshold == 0)
		return TRUE;

	n_days_ago = (time (NULL) - (SECONDS_PER_DAY * n_days_threshold));
	n_days_ago_as_string = tracker_date_to_string (n_days_ago);

	g_message ("Running stale volumes check...");

	miner_files_in_removable_media_remove_by_date (miner, n_days_ago_as_string);

	g_free (n_days_ago_as_string);

	return TRUE;
}

static void
init_stale_volume_removal (TrackerMinerFiles *miner)
{
	/* If disabled, make sure we don't do anything */
	if (tracker_config_get_removable_days_threshold (miner->private->config) == 0) {
		g_message ("Stale volume check is disabled");
		return;
	}

	/* Run right away the first check */
	cleanup_stale_removable_volumes_cb (miner);

	g_message ("Initializing stale volume check timeout...");

	/* Then, setup new timeout event every day */
	miner->private->stale_volumes_check_id =
		g_timeout_add_seconds (SECONDS_PER_DAY + 1,
		                       cleanup_stale_removable_volumes_cb,
		                       miner);
}


static void
mount_point_removed_cb (TrackerStorage *storage,
                        const gchar    *uuid,
                        const gchar    *mount_point,
                        gpointer        user_data)
{
	TrackerMinerFiles *miner = user_data;
	TrackerIndexingTree *indexing_tree;
	gchar *urn;
	GFile *mount_point_file;

	urn = g_strdup_printf (TRACKER_DATASOURCE_URN_PREFIX "%s", uuid);
	g_debug ("Mount point removed for URN '%s'", urn);

	mount_point_file = g_file_new_for_path (mount_point);

	/* Notify extractor about cancellation of all tasks under the mount point */
	tracker_extract_client_cancel_for_prefix (mount_point_file);

	/* Tell TrackerMinerFS to skip monitoring everything under the mount
	 *  point (in case there was no pre-unmount notification) */
	indexing_tree = tracker_miner_fs_get_indexing_tree (TRACKER_MINER_FS (miner));
	tracker_indexing_tree_remove (indexing_tree, mount_point_file);

	/* Set mount point status in tracker-store */
	set_up_mount_point (miner, urn, mount_point, NULL, FALSE, NULL);

	g_free (urn);
	g_object_unref (mount_point_file);
}

static void
mount_point_added_cb (TrackerStorage *storage,
                      const gchar    *uuid,
                      const gchar    *mount_point,
                      const gchar    *mount_name,
                      gboolean        removable,
                      gboolean        optical,
                      gpointer        user_data)
{
	TrackerMinerFiles *miner = user_data;
	TrackerMinerFilesPrivate *priv;
	gchar *urn;
	GString *queries;

	priv = TRACKER_MINER_FILES_GET_PRIVATE (miner);

	urn = g_strdup_printf (TRACKER_DATASOURCE_URN_PREFIX "%s", uuid);
	g_message ("Mount point added for URN '%s'", urn);

	if (removable && !priv->index_removable_devices) {
		g_message ("  Not crawling, removable devices disabled in config");
	} else if (optical && !priv->index_optical_discs) {
		g_message ("  Not crawling, optical devices discs disabled in config");
	} else if (!removable && !optical) {
		TrackerIndexingTree *indexing_tree;
		TrackerDirectoryFlags flags;
		GFile *mount_point_file;
		GSList *l;

		mount_point_file = g_file_new_for_path (mount_point);
		indexing_tree = tracker_miner_fs_get_indexing_tree (TRACKER_MINER_FS (miner));

		/* Check if one of the recursively indexed locations is in
		 *   the mounted path, or if the mounted path is inside
		 *   a recursively indexed directory... */
		for (l = tracker_config_get_index_recursive_directories (miner->private->config);
		     l;
		     l = g_slist_next (l)) {
			GFile *config_file;

			config_file = g_file_new_for_path (l->data);
			flags = TRACKER_DIRECTORY_FLAG_RECURSE |
				TRACKER_DIRECTORY_FLAG_CHECK_MTIME |
				TRACKER_DIRECTORY_FLAG_PRESERVE;

			if (tracker_config_get_enable_monitors (miner->private->config)) {
				flags |= TRACKER_DIRECTORY_FLAG_MONITOR;
			}

			if (g_file_equal (config_file, mount_point_file) ||
			    g_file_has_prefix (config_file, mount_point_file)) {
				/* If the config path is contained inside the mount path,
				 *  then add the config path to re-check */
				g_message ("  Re-check of configured path '%s' needed (recursively)",
				           (gchar *) l->data);
				tracker_indexing_tree_add (indexing_tree,
							   config_file,
							   flags);
			} else if (g_file_has_prefix (mount_point_file, config_file)) {
				/* If the mount path is contained inside the config path,
				 *  then add the mount path to re-check */
				g_message ("  Re-check of path '%s' needed (inside configured path '%s')",
				           mount_point,
				           (gchar *) l->data);
				tracker_indexing_tree_add (indexing_tree,
							   config_file,
							   flags);
			}
			g_object_unref (config_file);
		}

		/* Check if one of the non-recursively indexed locations is in
		 *  the mount path... */
		for (l = tracker_config_get_index_single_directories (miner->private->config);
		     l;
		     l = g_slist_next (l)) {
			GFile *config_file;

			flags = TRACKER_DIRECTORY_FLAG_CHECK_MTIME;

			if (tracker_config_get_enable_monitors (miner->private->config)) {
				flags |= TRACKER_DIRECTORY_FLAG_MONITOR;
			}

			config_file = g_file_new_for_path (l->data);
			if (g_file_equal (config_file, mount_point_file) ||
			    g_file_has_prefix (config_file, mount_point_file)) {
				g_message ("  Re-check of configured path '%s' needed (non-recursively)",
				           (gchar *) l->data);
				tracker_indexing_tree_add (indexing_tree,
							   config_file,
							   flags);
			}
			g_object_unref (config_file);
		}

		g_object_unref (mount_point_file);
	} else {
		g_message ("  Adding directories in removable/optical media to crawler's queue");
		miner_files_add_removable_or_optical_directory (miner,
		                                                mount_point,
		                                                uuid);
	}

	queries = g_string_new ("");
	set_up_mount_point (miner, urn, mount_point, mount_name, TRUE, queries);
	set_up_mount_point_type (miner, urn, removable, optical, queries);
	tracker_sparql_connection_update_async (tracker_miner_get_connection (TRACKER_MINER (miner)),
	                                        queries->str,
	                                        G_PRIORITY_LOW,
	                                        NULL,
	                                        set_up_mount_point_cb,
	                                        g_strdup (urn));
	g_string_free (queries, TRUE);
	g_free (urn);
}

#if defined(HAVE_UPOWER) || defined(HAVE_HAL)

static void
set_up_throttle (TrackerMinerFiles *mf,
                 gboolean           enable)
{
	gdouble throttle;
	gint config_throttle;

	config_throttle = tracker_config_get_throttle (mf->private->config);
	throttle = (1.0 / 20) * config_throttle;

	if (enable) {
		throttle += 0.25;
	}

	throttle = CLAMP (throttle, 0, 1);

	g_debug ("Setting new throttle to %0.3f", throttle);
	tracker_miner_fs_set_throttle (TRACKER_MINER_FS (mf), throttle);
}

static void
check_battery_status (TrackerMinerFiles *mf)
{
	gboolean on_battery, on_low_battery;
	gboolean should_pause = FALSE;
	gboolean should_throttle = FALSE;

	on_low_battery = tracker_power_get_on_low_battery (mf->private->power);
	on_battery = tracker_power_get_on_battery (mf->private->power);

	if (!on_battery) {
		g_message ("Running on AC power");
		should_pause = FALSE;
		should_throttle = FALSE;
	} else if (on_low_battery) {
		g_message ("Running on LOW Battery, pausing");
		should_pause = TRUE;
		should_throttle = TRUE;
	} else {
		should_throttle = TRUE;

		/* Check if miner should be paused based on configuration */
		if (!tracker_config_get_index_on_battery (mf->private->config)) {
			if (!tracker_config_get_index_on_battery_first_time (mf->private->config)) {
				g_message ("Running on battery, but not enabled, pausing");
				should_pause = TRUE;
			} else if (tracker_db_manager_get_first_index_done ()) {
				g_message ("Running on battery and first-time index "
				           "already done, pausing");
				should_pause = TRUE;
			} else {
				g_message ("Running on battery, but first-time index not "
				           "already finished, keeping on");
			}
		} else {
			g_message ("Running on battery");
		}
	}

	if (should_pause) {
		/* Don't try to pause again */
		if (mf->private->low_battery_pause_cookie == 0) {
			mf->private->low_battery_pause_cookie =
				tracker_miner_pause (TRACKER_MINER (mf),
				                     _("Low battery"),
				                     NULL);
		}
	} else {
		/* Don't try to resume again */
		if (mf->private->low_battery_pause_cookie != 0) {
			tracker_miner_resume (TRACKER_MINER (mf),
			                      mf->private->low_battery_pause_cookie,
			                      NULL);
			mf->private->low_battery_pause_cookie = 0;
		}
	}

	set_up_throttle (mf, should_throttle);
}

/* Called when battery status change is detected */
static void
battery_status_cb (GObject    *object,
                   GParamSpec *pspec,
                   gpointer    user_data)
{
	TrackerMinerFiles *mf = user_data;

	check_battery_status (mf);
}

/* Called when battery-related configuration change is detected */
static void
index_on_battery_cb (GObject    *object,
                     GParamSpec *pspec,
                     gpointer    user_data)
{
	TrackerMinerFiles *mf = user_data;

	check_battery_status (mf);
}

#endif /* defined(HAVE_UPOWER) || defined(HAVE_HAL) */

/* Called when mining has finished the first time */
static void
miner_finished_cb (TrackerMinerFS *fs,
                   gdouble         seconds_elapsed,
                   guint           total_directories_found,
                   guint           total_directories_ignored,
                   guint           total_files_found,
                   guint           total_files_ignored,
                   gpointer        user_data)
{
	TrackerMinerFiles *mf = TRACKER_MINER_FILES (fs);

	/* Create stamp file if not already there */
	if (!tracker_db_manager_get_first_index_done ()) {
		tracker_db_manager_set_first_index_done (TRUE);
	}

	/* And remove the signal handler so that it's not
	 *  called again */
	if (mf->private->finished_handler) {
		g_signal_handler_disconnect (fs, mf->private->finished_handler);
		mf->private->finished_handler = 0;
	}

#if defined(HAVE_UPOWER) || defined(HAVE_HAL)
	check_battery_status (mf);
#endif /* defined(HAVE_UPOWER) || defined(HAVE_HAL) */
}

static void
mount_pre_unmount_cb (GVolumeMonitor    *volume_monitor,
                      GMount            *mount,
                      TrackerMinerFiles *mf)
{
	TrackerIndexingTree *indexing_tree;
	GFile *mount_root;
	gchar *uri;

	mount_root = g_mount_get_root (mount);
	uri = g_file_get_uri (mount_root);
	g_message ("Pre-unmount requested for '%s'", uri);

	indexing_tree = tracker_miner_fs_get_indexing_tree (TRACKER_MINER_FS (mf));
	tracker_indexing_tree_remove (indexing_tree, mount_root);
	g_object_unref (mount_root);

	g_free (uri);
}

static gboolean
disk_space_check (TrackerMinerFiles *mf)
{
	gint limit;
	gchar *data_dir;
	gdouble remaining;

	limit = tracker_config_get_low_disk_space_limit (mf->private->config);

	if (limit < 1) {
		return FALSE;
	}

	/* Get % of remaining space in the partition where the cache is */
	data_dir = g_build_filename (g_get_user_cache_dir (), "tracker", NULL);
	remaining = tracker_file_system_get_remaining_space_percentage (data_dir);
	g_free (data_dir);

	if (remaining <= limit) {
		g_message ("WARNING: Available disk space (%lf%%) is below "
		           "configured threshold for acceptable working (%d%%)",
		           remaining, limit);
		return TRUE;
	}

	return FALSE;
}

static gboolean
disk_space_check_cb (gpointer user_data)
{
	TrackerMinerFiles *mf = user_data;

	if (disk_space_check (mf)) {
		/* Don't try to pause again */
		if (mf->private->disk_space_pause_cookie == 0) {
			mf->private->disk_space_pause_cookie =
				tracker_miner_pause (TRACKER_MINER (mf),
				                     _("Low disk space"),
				                     NULL);
		}
	} else {
		/* Don't try to resume again */
		if (mf->private->disk_space_pause_cookie != 0) {
			tracker_miner_resume (TRACKER_MINER (mf),
			                      mf->private->disk_space_pause_cookie,
			                      NULL);
			mf->private->disk_space_pause_cookie = 0;
		}
	}

	return TRUE;
}

static void
disk_space_check_start (TrackerMinerFiles *mf)
{
	gint limit;

	if (mf->private->disk_space_check_id != 0) {
		return;
	}

	limit = tracker_config_get_low_disk_space_limit (mf->private->config);

	if (limit != -1) {
		g_message ("Starting disk space check for every %d seconds",
		           DISK_SPACE_CHECK_FREQUENCY);
		mf->private->disk_space_check_id =
			g_timeout_add_seconds (DISK_SPACE_CHECK_FREQUENCY,
			                       disk_space_check_cb,
			                       mf);

		/* Call the function now too to make sure we have an
		 * initial value too!
		 */
		disk_space_check_cb (mf);
	} else {
		g_message ("Not setting disk space, configuration is set to -1 (disabled)");
	}
}

static void
disk_space_check_stop (TrackerMinerFiles *mf)
{
	if (mf->private->disk_space_check_id) {
		g_message ("Stopping disk space check");
		g_source_remove (mf->private->disk_space_check_id);
		mf->private->disk_space_check_id = 0;
	}
}

static void
low_disk_space_limit_cb (GObject    *gobject,
                         GParamSpec *arg1,
                         gpointer    user_data)
{
	TrackerMinerFiles *mf = user_data;

	disk_space_check_cb (mf);
}

static void
indexing_tree_update_filter (TrackerIndexingTree *indexing_tree,
			     TrackerFilterType    filter,
			     GSList              *new_elems)
{
	tracker_indexing_tree_clear_filters (indexing_tree, filter);

	while (new_elems) {
		tracker_indexing_tree_add_filter (indexing_tree, filter,
						  new_elems->data);
		new_elems = new_elems->next;
	}
}

static void
miner_files_update_filters (TrackerMinerFiles *files)
{
	TrackerIndexingTree *indexing_tree;
	GSList *list;

	indexing_tree = tracker_miner_fs_get_indexing_tree (TRACKER_MINER_FS (files));

	/* Ignored files */
	list = tracker_config_get_ignored_files (files->private->config);
	indexing_tree_update_filter (indexing_tree, TRACKER_FILTER_FILE, list);

	/* Ignored directories */
	list = tracker_config_get_ignored_directories (files->private->config);
	indexing_tree_update_filter (indexing_tree,
				     TRACKER_FILTER_DIRECTORY,
				     list);

	/* Directories with content */
	list = tracker_config_get_ignored_directories_with_content (files->private->config);
	indexing_tree_update_filter (indexing_tree,
				     TRACKER_FILTER_PARENT_DIRECTORY,
				     list);
}

static void
update_directories_from_new_config (TrackerMinerFS *mf,
                                    GSList         *new_dirs,
                                    GSList         *old_dirs,
                                    gboolean        recurse)
{
	TrackerMinerFilesPrivate *priv;
	TrackerDirectoryFlags flags = 0;
	TrackerIndexingTree *indexing_tree;
	GSList *sl;

	priv = TRACKER_MINER_FILES_GET_PRIVATE (mf);
	indexing_tree = tracker_miner_fs_get_indexing_tree (mf);

	g_message ("Updating %s directories changed from configuration",
	           recurse ? "recursive" : "single");

	/* First remove all directories removed from the config */
	for (sl = old_dirs; sl; sl = sl->next) {
		const gchar *path;

		path = sl->data;

		/* If we are not still in the list, remove the dir */
		if (!tracker_string_in_gslist (path, new_dirs)) {
			GFile *file;

			g_message ("  Removing directory: '%s'", path);

			file = g_file_new_for_path (path);

			/* First, remove the preserve flag, it might be
			 * set on configuration directories within mount
			 * points, as data should be persistent across
			 * unmounts.
			 */
			tracker_indexing_tree_get_root (indexing_tree,
							file, &flags);

			if ((flags & TRACKER_DIRECTORY_FLAG_PRESERVE) != 0) {
				flags &= ~(TRACKER_DIRECTORY_FLAG_PRESERVE);
				tracker_indexing_tree_add (indexing_tree,
							   file, flags);
			}

			/* Fully remove item (monitors and from store),
			 * now that there's no preserve flag.
			 */
			tracker_indexing_tree_remove (indexing_tree, file);
			g_object_unref (file);
		}
	}

	flags = TRACKER_DIRECTORY_FLAG_NONE;

	if (recurse) {
		flags |= TRACKER_DIRECTORY_FLAG_RECURSE;
	}

	if (tracker_config_get_enable_monitors (priv->config)) {
		flags |= TRACKER_DIRECTORY_FLAG_MONITOR;
	}

	if (tracker_miner_fs_get_mtime_checking (TRACKER_MINER_FS (mf))) {
		flags |= TRACKER_DIRECTORY_FLAG_CHECK_MTIME;
	}

	/* Second add directories which are new */
	for (sl = new_dirs; sl; sl = sl->next) {
		const gchar *path;

		path = sl->data;

		/* If we are now in the list, add the dir */
		if (!tracker_string_in_gslist (path, old_dirs)) {
			GFile *file;

			g_message ("  Adding directory:'%s'", path);

			file = g_file_new_for_path (path);
			tracker_indexing_tree_add (indexing_tree, file, flags);
			g_object_unref (file);
		}
	}
}

static void
index_recursive_directories_cb (GObject    *gobject,
                                GParamSpec *arg1,
                                gpointer    user_data)
{
	TrackerMinerFilesPrivate *private;
	GSList *new_dirs, *old_dirs;

	private = TRACKER_MINER_FILES_GET_PRIVATE (user_data);

	new_dirs = tracker_config_get_index_recursive_directories (private->config);
	old_dirs = private->index_recursive_directories;

	update_directories_from_new_config (TRACKER_MINER_FS (user_data),
	                                    new_dirs,
	                                    old_dirs,
	                                    TRUE);

	/* Re-set the stored config in case it changes again */
	if (private->index_recursive_directories) {
		g_slist_foreach (private->index_recursive_directories, (GFunc) g_free, NULL);
		g_slist_free (private->index_recursive_directories);
	}

	private->index_recursive_directories = tracker_gslist_copy_with_string_data (new_dirs);
}

static void
index_single_directories_cb (GObject    *gobject,
                             GParamSpec *arg1,
                             gpointer    user_data)
{
	TrackerMinerFilesPrivate *private;
	GSList *new_dirs, *old_dirs;

	private = TRACKER_MINER_FILES_GET_PRIVATE (user_data);

	new_dirs = tracker_config_get_index_single_directories (private->config);
	old_dirs = private->index_single_directories;

	update_directories_from_new_config (TRACKER_MINER_FS (user_data),
	                                    new_dirs,
	                                    old_dirs,
	                                    FALSE);

	/* Re-set the stored config in case it changes again */
	if (private->index_single_directories) {
		g_slist_foreach (private->index_single_directories, (GFunc) g_free, NULL);
		g_slist_free (private->index_single_directories);
	}

	private->index_single_directories = tracker_gslist_copy_with_string_data (new_dirs);
}

static gboolean
miner_files_force_recheck_idle (gpointer user_data)
{
	TrackerMinerFiles *miner_files = user_data;
	TrackerIndexingTree *indexing_tree;
	GList *roots, *l;

	miner_files_update_filters (miner_files);

	indexing_tree = tracker_miner_fs_get_indexing_tree (TRACKER_MINER_FS (miner_files));
	roots = tracker_indexing_tree_list_roots (indexing_tree);

	for (l = roots; l; l = l->next)	{
		GFile *root = l->data;

		g_signal_emit_by_name (indexing_tree, "directory-updated", root);
	}

	miner_files->private->force_recheck_id = 0;
	g_list_free (roots);

	return FALSE;
}

static void
trigger_recheck_cb (GObject    *gobject,
                    GParamSpec *arg1,
                    gpointer    user_data)
{
	TrackerMinerFiles *mf = user_data;

	g_message ("Ignored content related configuration changed, checking index...");

	if (mf->private->force_recheck_id == 0) {
		/* Set idle so multiple changes in the config lead to one recheck */
		mf->private->force_recheck_id =
			g_idle_add (miner_files_force_recheck_idle, mf);
	}
}

static gboolean
index_volumes_changed_idle (gpointer user_data)
{
	TrackerMinerFiles *mf = user_data;
	GSList *mounts_removed = NULL;
	GSList *mounts_added = NULL;
	gboolean new_index_removable_devices;
	gboolean new_index_optical_discs;

	g_message ("Volume related configuration changed, updating...");

	/* Read new config values. Note that if removable devices is FALSE,
	 * optical discs will also always be FALSE. */
	new_index_removable_devices = tracker_config_get_index_removable_devices (mf->private->config);
	new_index_optical_discs = (new_index_removable_devices ?
	                           tracker_config_get_index_optical_discs (mf->private->config) :
	                           FALSE);

	/* Removable devices config changed? */
	if (mf->private->index_removable_devices != new_index_removable_devices) {
		GSList *m;

		/* Get list of roots for currently mounted removable devices
		 * (excluding optical) */
		m = tracker_storage_get_device_roots (mf->private->storage,
		                                      TRACKER_STORAGE_REMOVABLE,
		                                      TRUE);
		/* Set new config value */
		mf->private->index_removable_devices = new_index_removable_devices;

		if (mf->private->index_removable_devices) {
			/* If previously not indexing and now indexing, need to re-check
			 * current mounted volumes, add new monitors and index new files
			 */
			mounts_added = m;
		} else {
			/* If previously indexing and now not indexing, need to re-check
			 * current mounted volumes, remove monitors and remove all resources
			 * from the store belonging to a removable device
			 */
			mounts_removed = m;

			/* And now, single sparql update to remove all resources
			 * corresponding to removable devices (includes those
			 * not currently mounted) */
			miner_files_in_removable_media_remove_by_type (mf, TRACKER_STORAGE_REMOVABLE);
		}
	}

	/* Optical discs config changed? */
	if (mf->private->index_optical_discs != new_index_optical_discs) {
		GSList *m;

		/* Get list of roots for removable devices (excluding optical) */
		m = tracker_storage_get_device_roots (mf->private->storage,
		                                      TRACKER_STORAGE_REMOVABLE | TRACKER_STORAGE_OPTICAL,
		                                      TRUE);

		/* Set new config value */
		mf->private->index_optical_discs = new_index_optical_discs;

		if (mf->private->index_optical_discs) {
			/* If previously not indexing and now indexing, need to re-check
			 * current mounted volumes, add new monitors and index new files
			 */
			mounts_added = g_slist_concat (mounts_added, m);
		} else {
			/* If previously indexing and now not indexing, need to re-check
			 * current mounted volumes, remove monitors and remove all resources
			 * from the store belonging to a optical disc
			 */
			mounts_removed = g_slist_concat (mounts_removed, m);

			/* And now, single sparql update to remove all resources
			 * corresponding to removable+optical devices (includes those
			 * not currently mounted) */
			miner_files_in_removable_media_remove_by_type (mf, TRACKER_STORAGE_REMOVABLE | TRACKER_STORAGE_OPTICAL);
		}
	}

	/* Tell TrackerMinerFS to stop monitoring the given removed mount paths, if any */
	if (mounts_removed) {
		TrackerIndexingTree *indexing_tree;
		GSList *sl;

		indexing_tree = tracker_miner_fs_get_indexing_tree (TRACKER_MINER_FS (mf));

		for (sl = mounts_removed; sl; sl = g_slist_next (sl)) {
			GFile *mount_point_file;

			mount_point_file = g_file_new_for_path (sl->data);
			tracker_indexing_tree_remove (indexing_tree,
						      mount_point_file);
			g_object_unref (mount_point_file);
		}

		g_slist_foreach (mounts_removed, (GFunc) g_free, NULL);
		g_slist_free (mounts_removed);
	}

	/* Tell TrackerMinerFS to start monitoring the given added mount paths, if any */
	if (mounts_added) {
		GSList *sl;

		for (sl = mounts_added; sl; sl = g_slist_next (sl)) {
			miner_files_add_removable_or_optical_directory (mf,
			                                                (gchar *) sl->data,
			                                                NULL);
		}

		g_slist_foreach (mounts_added, (GFunc) g_free, NULL);
		g_slist_free (mounts_added);
	}

	mf->private->volumes_changed_id = 0;

	/* Check if the stale volume removal configuration changed from enabled to disabled
	 * or from disabled to enabled */
	if (tracker_config_get_removable_days_threshold (mf->private->config) == 0 &&
	    mf->private->stale_volumes_check_id != 0) {
		/* From having the check enabled to having it disabled, remove the timeout */
		g_debug ("  Stale volume removal now disabled, removing timeout");
		g_source_remove (mf->private->stale_volumes_check_id);
		mf->private->stale_volumes_check_id = 0;
	} else if (tracker_config_get_removable_days_threshold (mf->private->config) > 0 &&
	           mf->private->stale_volumes_check_id == 0) {
		g_debug ("  Stale volume removal now enabled, initializing timeout");
		/* From having the check disabled to having it enabled, so fire up the
		 * timeout. */
		init_stale_volume_removal (TRACKER_MINER_FILES (mf));
	}

	return FALSE;
}

static void
index_volumes_changed_cb (GObject    *gobject,
                          GParamSpec *arg1,
                          gpointer    user_data)
{
	TrackerMinerFiles *miner_files = user_data;

	if (miner_files->private->volumes_changed_id == 0) {
		/* Set idle so multiple changes in the config lead to one check */
		miner_files->private->volumes_changed_id =
			g_idle_add (index_volumes_changed_idle, miner_files);
	}
}

static const gchar *
miner_files_get_file_urn (TrackerMinerFiles *miner,
                          GFile             *file,
                          gboolean          *is_iri)
{
	const gchar *urn;

	urn = tracker_miner_fs_get_urn (TRACKER_MINER_FS (miner), file);
	*is_iri = TRUE;

	if (!urn) {
		/* This is a new insertion, use anonymous URNs to store files */
		urn = "_:file";
		*is_iri = FALSE;
	}

	return urn;
}

static void
miner_files_add_to_datasource (TrackerMinerFiles    *mf,
                               GFile                *file,
                               TrackerSparqlBuilder *sparql)
{
	TrackerMinerFilesPrivate *priv;
	const gchar *removable_device_uuid;
	gchar *removable_device_urn, *uri;
	const gchar *urn;
	gboolean is_iri;

	priv = TRACKER_MINER_FILES_GET_PRIVATE (mf);
	uri = g_file_get_uri (file);

	removable_device_uuid = tracker_storage_get_uuid_for_file (priv->storage, file);

	if (removable_device_uuid) {
		removable_device_urn = g_strdup_printf (TRACKER_DATASOURCE_URN_PREFIX "%s",
		                                        removable_device_uuid);
	} else {
		removable_device_urn = g_strdup (TRACKER_NON_REMOVABLE_MEDIA_DATASOURCE_URN);
	}

	urn = miner_files_get_file_urn (mf, file, &is_iri);

	if (is_iri) {
		tracker_sparql_builder_subject_iri (sparql, urn);
	} else {
		tracker_sparql_builder_subject (sparql, urn);
	}

	tracker_sparql_builder_predicate (sparql, "a");
	tracker_sparql_builder_object (sparql, "nfo:FileDataObject");

	tracker_sparql_builder_predicate (sparql, "nie:dataSource");
	tracker_sparql_builder_object_iri (sparql, removable_device_urn);

	tracker_sparql_builder_predicate (sparql, "tracker:available");
	tracker_sparql_builder_object_boolean (sparql, TRUE);

	g_free (removable_device_urn);
	g_free (uri);
}

static void
miner_files_add_rdf_types (TrackerSparqlBuilder *sparql,
                           GFile                *file,
                           const gchar          *mime_type)
{
	GStrv rdf_types;
	gint i = 0;

	rdf_types = tracker_extract_module_manager_get_fallback_rdf_types (mime_type);

	if (!rdf_types)
		return;

	if (rdf_types[0]) {
		tracker_sparql_builder_predicate (sparql, "a");

		while (rdf_types[i]) {
			tracker_sparql_builder_object (sparql, rdf_types[i]);
			i++;
		}
	}

	g_strfreev (rdf_types);
}

static void
process_file_data_free (ProcessFileData *data)
{
	g_object_unref (data->miner);
	g_object_unref (data->sparql);
	g_object_unref (data->cancellable);
	g_object_unref (data->file);
	g_free (data->mime_type);
	g_slice_free (ProcessFileData, data);
}

static void
sparql_builder_finish (ProcessFileData *data,
                       const gchar     *preupdate,
                       const gchar     *postupdate,
                       const gchar     *sparql,
                       const gchar     *where)
{
	const gchar *uuid;

	if (sparql && *sparql) {
		gboolean is_iri;
		const gchar *urn;

		urn = miner_files_get_file_urn (data->miner, data->file, &is_iri);

		if (is_iri) {
			gchar *str;

			str = g_strdup_printf ("<%s>", urn);
			tracker_sparql_builder_append (data->sparql, str);
			g_free (str);
		} else {
			tracker_sparql_builder_append (data->sparql, urn);
		}

		tracker_sparql_builder_append (data->sparql, sparql);
	}

	tracker_sparql_builder_graph_close (data->sparql);
	tracker_sparql_builder_insert_close (data->sparql);

	if (where && *where) {
		tracker_sparql_builder_where_open (data->sparql);
		tracker_sparql_builder_append (data->sparql, where);
		tracker_sparql_builder_where_close (data->sparql);
	}

	/* Prepend preupdate queries */
	if (preupdate && *preupdate) {
		tracker_sparql_builder_prepend (data->sparql, preupdate);
	}

	/* Append postupdate */
	if (postupdate && *postupdate) {
		tracker_sparql_builder_append (data->sparql, postupdate);
	}

	uuid = g_object_get_qdata (G_OBJECT (data->file),
	                           data->miner->private->quark_mount_point_uuid);

	/* File represents a mount point */
	if (G_UNLIKELY (uuid)) {
		GString *queries;
		gchar *removable_device_urn, *uri;

		removable_device_urn = g_strdup_printf (TRACKER_DATASOURCE_URN_PREFIX "%s", uuid);
		uri = g_file_get_uri (G_FILE (data->file));
		queries = g_string_new ("");

		g_string_append_printf (queries,
		                        "DELETE { "
		                        "  <%s> tracker:mountPoint ?unknown "
		                        "} WHERE { "
		                        "  <%s> a tracker:Volume; "
		                        "       tracker:mountPoint ?unknown "
		                        "} ",
		                        removable_device_urn, removable_device_urn);

		g_string_append_printf (queries,
		                        "INSERT { GRAPH <%s> {"
		                        "  <%s> a tracker:Volume; "
		                        "       tracker:mountPoint ?u "
		                        "} } WHERE { "
		                        "  ?u a nfo:FileDataObject; "
		                        "     nie:url \"%s\" "
		                        "} ",
		                        removable_device_urn, removable_device_urn, uri);

		tracker_sparql_builder_append (data->sparql, queries->str);
		g_string_free (queries, TRUE);
		g_free (removable_device_urn);
		g_free (uri);
	}
}

static void
process_file_cb (GObject      *object,
                 GAsyncResult *result,
                 gpointer      user_data)
{
	TrackerMinerFilesPrivate *priv;
	TrackerSparqlBuilder *sparql;
	ProcessFileData *data;
	const gchar *mime_type, *urn, *parent_urn;
	GFileInfo *file_info;
	guint64 time_;
	GFile *file;
	gchar *uri;
	GError *error = NULL;
	gboolean is_iri;
	gboolean is_directory;

	data = user_data;
	file = G_FILE (object);
	sparql = data->sparql;
	file_info = g_file_query_info_finish (file, result, &error);
	priv = TRACKER_MINER_FILES (data->miner)->private;

	if (error) {
		/* Something bad happened, notify about the error */
		tracker_miner_fs_file_notify (TRACKER_MINER_FS (data->miner), file, error);
		priv->extraction_queue = g_list_remove (priv->extraction_queue, data);
		process_file_data_free (data);
		g_error_free (error);

		return;
	}

	uri = g_file_get_uri (file);
	mime_type = g_file_info_get_content_type (file_info);
	urn = miner_files_get_file_urn (TRACKER_MINER_FILES (data->miner), file, &is_iri);

	data->mime_type = g_strdup (mime_type);

	tracker_sparql_builder_insert_silent_open (sparql, NULL);
	tracker_sparql_builder_graph_open (sparql, TRACKER_MINER_FS_GRAPH_URN);

	if (is_iri) {
		tracker_sparql_builder_subject_iri (sparql, urn);
	} else {
		tracker_sparql_builder_subject (sparql, urn);
	}

	tracker_sparql_builder_predicate (sparql, "a");
	tracker_sparql_builder_object (sparql, "nfo:FileDataObject");
	tracker_sparql_builder_object (sparql, "nie:InformationElement");

	is_directory = (g_file_info_get_file_type (file_info) == G_FILE_TYPE_DIRECTORY ?
	                TRUE : FALSE);
	if (is_directory) {
		tracker_sparql_builder_object (sparql, "nfo:Folder");
	}

	parent_urn = tracker_miner_fs_get_parent_urn (TRACKER_MINER_FS (data->miner), file);

	if (parent_urn) {
		tracker_sparql_builder_predicate (sparql, "nfo:belongsToContainer");
		tracker_sparql_builder_object_iri (sparql, parent_urn);
	}

	tracker_sparql_builder_predicate (sparql, "nfo:fileName");
	tracker_sparql_builder_object_string (sparql, g_file_info_get_display_name (file_info));

	tracker_sparql_builder_predicate (sparql, "nfo:fileSize");
	tracker_sparql_builder_object_int64 (sparql, g_file_info_get_size (file_info));

	time_ = g_file_info_get_attribute_uint64 (file_info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
	tracker_sparql_builder_predicate (sparql, "nfo:fileLastModified");
	tracker_sparql_builder_object_date (sparql, (time_t *) &time_);

	time_ = g_file_info_get_attribute_uint64 (file_info, G_FILE_ATTRIBUTE_TIME_ACCESS);
	tracker_sparql_builder_predicate (sparql, "nfo:fileLastAccessed");
	tracker_sparql_builder_object_date (sparql, (time_t *) &time_);

	/* Laying the link between the IE and the DO. We use IE = DO */
	tracker_sparql_builder_predicate (sparql, "nie:isStoredAs");
	if (is_iri) {
		tracker_sparql_builder_object_iri (sparql, urn);
	} else {
		tracker_sparql_builder_object (sparql, urn);
	}

	/* The URL of the DataObject (because IE = DO, this is correct) */
	tracker_sparql_builder_predicate (sparql, "nie:url");
	tracker_sparql_builder_object_string (sparql, uri);

	tracker_sparql_builder_predicate (sparql, "nie:mimeType");
	tracker_sparql_builder_object_string (sparql, mime_type);

	miner_files_add_to_datasource (data->miner, file, sparql);

        miner_files_add_rdf_types (sparql, file, mime_type);

	sparql_builder_finish (data, NULL, NULL, NULL, NULL);
	tracker_miner_fs_file_notify (TRACKER_MINER_FS (data->miner), data->file, NULL);

	priv->extraction_queue = g_list_remove (priv->extraction_queue, data);
	process_file_data_free (data);

	g_object_unref (file_info);
	g_free (uri);
}

static gboolean
miner_files_process_file (TrackerMinerFS       *fs,
                          GFile                *file,
                          TrackerSparqlBuilder *sparql,
                          GCancellable         *cancellable)
{
	TrackerMinerFilesPrivate *priv;
	ProcessFileData *data;
	const gchar *attrs;

	data = g_slice_new0 (ProcessFileData);
	data->miner = g_object_ref (fs);
	data->cancellable = g_object_ref (cancellable);
	data->sparql = g_object_ref (sparql);
	data->file = g_object_ref (file);

	priv = TRACKER_MINER_FILES (fs)->private;
	priv->extraction_queue = g_list_prepend (priv->extraction_queue, data);

	attrs = G_FILE_ATTRIBUTE_STANDARD_TYPE ","
		G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE ","
		G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME ","
		G_FILE_ATTRIBUTE_STANDARD_SIZE ","
		G_FILE_ATTRIBUTE_TIME_MODIFIED ","
		G_FILE_ATTRIBUTE_TIME_ACCESS;

	g_file_query_info_async (file,
	                         attrs,
	                         G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
	                         G_PRIORITY_DEFAULT,
	                         cancellable,
	                         process_file_cb,
	                         data);

	return TRUE;
}

static void
process_file_attributes_cb (GObject      *object,
                            GAsyncResult *result,
                            gpointer      user_data)
{
	TrackerSparqlBuilder *sparql;
	ProcessFileData *data;
	const gchar *urn;
	GFileInfo *file_info;
	guint64 time_;
	GFile *file;
	gchar *uri;
	GError *error = NULL;
	gboolean is_iri;

	data = user_data;
	file = G_FILE (object);
	sparql = data->sparql;
	file_info = g_file_query_info_finish (file, result, &error);

	if (error) {
		/* Something bad happened, notify about the error */
		tracker_miner_fs_file_notify (TRACKER_MINER_FS (data->miner), file, error);
		process_file_data_free (data);
		g_error_free (error);
		return;
	}

	uri = g_file_get_uri (file);
	urn = miner_files_get_file_urn (TRACKER_MINER_FILES (data->miner), file, &is_iri);

	/* We MUST have an IRI in attributes updating */
	if (!is_iri) {
		error = g_error_new_literal (miner_files_error_quark,
		                             0,
		                             "Received request to update attributes but no IRI available!");
		/* Notify about the error */
		tracker_miner_fs_file_notify (TRACKER_MINER_FS (data->miner), file, error);
		process_file_data_free (data);
		g_error_free (error);
		return;
	}

	/* Update nfo:fileLastModified */
	tracker_sparql_builder_delete_open (sparql, NULL);
	tracker_sparql_builder_subject_iri (sparql, urn);
	tracker_sparql_builder_predicate (sparql, "nfo:fileLastModified");
	tracker_sparql_builder_object_variable (sparql, "lastmodified");
	tracker_sparql_builder_delete_close (sparql);
	tracker_sparql_builder_where_open (sparql);
	tracker_sparql_builder_subject_iri (sparql, urn);
	tracker_sparql_builder_predicate (sparql, "nfo:fileLastModified");
	tracker_sparql_builder_object_variable (sparql, "lastmodified");
	tracker_sparql_builder_where_close (sparql);
	tracker_sparql_builder_insert_open (sparql, NULL);
	tracker_sparql_builder_graph_open (sparql, TRACKER_MINER_FS_GRAPH_URN);
	tracker_sparql_builder_subject_iri (sparql, urn);
	time_ = g_file_info_get_attribute_uint64 (file_info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
	tracker_sparql_builder_predicate (sparql, "nfo:fileLastModified");
	tracker_sparql_builder_object_date (sparql, (time_t *) &time_);
	tracker_sparql_builder_graph_close (sparql);
	tracker_sparql_builder_insert_close (sparql);

	/* Update nfo:fileLastAccessed */
	tracker_sparql_builder_delete_open (sparql, NULL);
	tracker_sparql_builder_subject_iri (sparql, urn);
	tracker_sparql_builder_predicate (sparql, "nfo:fileLastAccessed");
	tracker_sparql_builder_object_variable (sparql, "lastaccessed");
	tracker_sparql_builder_delete_close (sparql);
	tracker_sparql_builder_where_open (sparql);
	tracker_sparql_builder_subject_iri (sparql, urn);
	tracker_sparql_builder_predicate (sparql, "nfo:fileLastAccessed");
	tracker_sparql_builder_object_variable (sparql, "lastaccessed");
	tracker_sparql_builder_where_close (sparql);
	tracker_sparql_builder_insert_open (sparql, NULL);
	tracker_sparql_builder_graph_open (sparql, TRACKER_MINER_FS_GRAPH_URN);
	tracker_sparql_builder_subject_iri (sparql, urn);
	time_ = g_file_info_get_attribute_uint64 (file_info, G_FILE_ATTRIBUTE_TIME_ACCESS);
	tracker_sparql_builder_predicate (sparql, "nfo:fileLastAccessed");
	tracker_sparql_builder_object_date (sparql, (time_t *) &time_);
	tracker_sparql_builder_graph_close (sparql);
	tracker_sparql_builder_insert_close (sparql);

	g_object_unref (file_info);
	g_free (uri);

	/* Notify about the success */
	tracker_miner_fs_file_notify (TRACKER_MINER_FS (data->miner), data->file, NULL);

	process_file_data_free (data);
}

static gboolean
miner_files_process_file_attributes (TrackerMinerFS       *fs,
                                     GFile                *file,
                                     TrackerSparqlBuilder *sparql,
                                     GCancellable         *cancellable)
{
	ProcessFileData *data;
	const gchar *attrs;

	data = g_slice_new0 (ProcessFileData);
	data->miner = g_object_ref (fs);
	data->cancellable = g_object_ref (cancellable);
	data->sparql = g_object_ref (sparql);
	data->file = g_object_ref (file);

	/* Query only attributes that may change in an ATTRIBUTES_UPDATED event */
	attrs = G_FILE_ATTRIBUTE_TIME_MODIFIED ","
		G_FILE_ATTRIBUTE_TIME_ACCESS;

	g_file_query_info_async (file,
	                         attrs,
	                         G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
	                         G_PRIORITY_DEFAULT,
	                         cancellable,
	                         process_file_attributes_cb,
	                         data);

	return TRUE;
}

static gboolean
miner_files_ignore_next_update_file (TrackerMinerFS       *fs,
                                     GFile                *file,
                                     TrackerSparqlBuilder *sparql,
                                     GCancellable         *cancellable)
{
	const gchar *attrs;
	const gchar *mime_type;
	GFileInfo *file_info;
	guint64 time_;
	gchar *uri;
	GError *error = NULL;

	attrs = G_FILE_ATTRIBUTE_STANDARD_TYPE ","
		G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE ","
		G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME ","
		G_FILE_ATTRIBUTE_STANDARD_SIZE ","
		G_FILE_ATTRIBUTE_TIME_MODIFIED ","
		G_FILE_ATTRIBUTE_TIME_ACCESS;

	file_info = g_file_query_info (file, attrs,
	                               G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
	                               cancellable, &error);

	if (error) {
		g_warning ("Can't ignore-next-update: '%s'", error->message);
		g_clear_error (&error);
		return FALSE;
	}

	uri = g_file_get_uri (file);
	mime_type = g_file_info_get_content_type (file_info);

	/* For ignore-next-update we only write a few properties back. These properties
	 * should NEVER be marked as tracker:writeback in the ontology! (else you break
	 * the tracker-writeback feature) */

	tracker_sparql_builder_insert_silent_open (sparql, TRACKER_MINER_FS_GRAPH_URN);

	tracker_sparql_builder_subject_variable (sparql, "urn");
	tracker_sparql_builder_predicate (sparql, "a");
	tracker_sparql_builder_object (sparql, "nfo:FileDataObject");

	tracker_sparql_builder_predicate (sparql, "nfo:fileSize");
	tracker_sparql_builder_object_int64 (sparql, g_file_info_get_size (file_info));

	time_ = g_file_info_get_attribute_uint64 (file_info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
	tracker_sparql_builder_predicate (sparql, "nfo:fileLastModified");
	tracker_sparql_builder_object_date (sparql, (time_t *) &time_);

	time_ = g_file_info_get_attribute_uint64 (file_info, G_FILE_ATTRIBUTE_TIME_ACCESS);
	tracker_sparql_builder_predicate (sparql, "nfo:fileLastAccessed");
	tracker_sparql_builder_object_date (sparql, (time_t *) &time_);

	tracker_sparql_builder_predicate (sparql, "nie:mimeType");
	tracker_sparql_builder_object_string (sparql, mime_type);

	tracker_sparql_builder_insert_close (sparql);

	tracker_sparql_builder_where_open (sparql);

	tracker_sparql_builder_subject_variable (sparql, "urn");
	tracker_sparql_builder_predicate (sparql, "nie:url");
	tracker_sparql_builder_object_string (sparql, uri);

	tracker_sparql_builder_where_close (sparql);

	g_object_unref (file_info);
	g_free (uri);

	return TRUE;
}

static void
miner_files_finished (TrackerMinerFS *fs)
{
	tracker_db_manager_set_last_crawl_done (TRUE);
}

TrackerMiner *
tracker_miner_files_new (TrackerConfig  *config,
                         GError        **error)
{
	return g_initable_new (TRACKER_TYPE_MINER_FILES,
	                       NULL,
	                       error,
	                       "name", "Files",
	                       "config", config,
	                       "processing-pool-wait-limit", 10,
	                       "processing-pool-ready-limit", 100,
	                       NULL);
}

gboolean
tracker_miner_files_check_file (GFile  *file,
                                GSList *ignored_file_paths,
                                GSList *ignored_file_patterns)
{
	GSList *l;
	gchar *basename;
	gchar *path;
	gboolean should_process;

	should_process = FALSE;
	basename = NULL;
	path = NULL;

	if (tracker_file_is_hidden (file)) {
		/* Ignore hidden files */
		goto done;
	}

	path = g_file_get_path (file);

	for (l = ignored_file_paths; l; l = l->next) {
		if (strcmp (l->data, path) == 0) {
			goto done;
		}
	}

	basename = g_file_get_basename (file);

	for (l = ignored_file_patterns; l; l = l->next) {
		if (g_pattern_match_string (l->data, basename)) {
			goto done;
		}
	}

	should_process = TRUE;

done:
	g_free (basename);
	g_free (path);

	return should_process;
}

gboolean
tracker_miner_files_check_directory (GFile  *file,
                                     GSList *index_recursive_directories,
                                     GSList *index_single_directories,
                                     GSList *ignored_directory_paths,
                                     GSList *ignored_directory_patterns)
{
	GSList *l;
	gchar *basename;
	gchar *path;
	gboolean should_process;
	gboolean is_hidden;

	should_process = FALSE;
	basename = NULL;

	path = g_file_get_path (file);

	/* First we check the GIO hidden check. This does a number of
	 * things for us which is good (like checking ".foo" dirs).
	 */
	is_hidden = tracker_file_is_hidden (file);

#ifdef __linux__
	/* Second we check if the file is on FAT and if the hidden
	 * attribute is set. GIO does this but ONLY on a Windows OS,
	 * not for Windows files under a Linux OS, so we have to check
	 * anyway.
	 */
	if (!is_hidden) {
		int fd;

		fd = g_open (path, O_RDONLY, 0);
		if (fd != -1) {
			__u32 attrs;

			if (ioctl (fd, FAT_IOCTL_GET_ATTRIBUTES, &attrs) == 0) {
				is_hidden = attrs & ATTR_HIDDEN ? TRUE : FALSE;
			}

			close (fd);
		}
	}
#endif /* __linux__ */

	if (is_hidden) {
		/* FIXME: We need to check if the file is actually a
		 * config specified location before blanket ignoring
		 * all hidden files.
		 */
		if (tracker_string_in_gslist (path, index_recursive_directories)) {
			should_process = TRUE;
		}

		if (tracker_string_in_gslist (path, index_single_directories)) {
			should_process = TRUE;
		}

		/* Ignore hidden dirs */
		goto done;
	}

	for (l = ignored_directory_paths; l; l = l->next) {
		if (strcmp (l->data, path) == 0) {
			goto done;
		}
	}

	basename = g_file_get_basename (file);

	for (l = ignored_directory_patterns; l; l = l->next) {
		if (g_pattern_match_string (l->data, basename)) {
			goto done;
		}
	}

	/* Check module directory ignore patterns */
	should_process = TRUE;

done:
	g_free (basename);
	g_free (path);

	return should_process;
}

gboolean
tracker_miner_files_check_directory_contents (GFile  *parent,
                                              GList  *children,
                                              GSList *ignored_content)
{
	GSList *l;

	if (!ignored_content) {
		return TRUE;
	}

	while (children) {
		gchar *basename;

		basename = g_file_get_basename (children->data);

		for (l = ignored_content; l; l = l->next) {
			if (g_strcmp0 (basename, l->data) == 0) {
				gchar *parent_uri;

				parent_uri = g_file_get_uri (parent);
				/* g_debug ("Directory '%s' ignored since it contains a file named '%s'", */
				/*          parent_uri, basename); */

				g_free (parent_uri);
				g_free (basename);

				return FALSE;
			}
		}

		children = children->next;
		g_free (basename);
	}

	return TRUE;
}

gboolean
tracker_miner_files_monitor_directory (GFile    *file,
                                       gboolean  enable_monitors,
                                       GSList   *directories_to_check)
{
	if (!enable_monitors) {
		return FALSE;
	}

	/* We'll only get this signal for the directories where check_directory()
	 * and check_directory_contents() returned TRUE, so by default we want
	 * these directories to be indexed. */

	return TRUE;
}

static void
remove_files_in_removable_media_cb (GObject      *object,
                                    GAsyncResult *result,
                                    gpointer      user_data)
{
	GError *error = NULL;

	tracker_sparql_connection_update_finish (TRACKER_SPARQL_CONNECTION (object), result, &error);

	if (error) {
		g_critical ("Could not remove files in volumes: %s", error->message);
		g_error_free (error);
	}
}

static gboolean
miner_files_in_removable_media_remove_by_type (TrackerMinerFiles  *miner,
                                               TrackerStorageType  type)
{
	gboolean removable;
	gboolean optical;

	removable = TRACKER_STORAGE_TYPE_IS_REMOVABLE (type);
	optical = TRACKER_STORAGE_TYPE_IS_OPTICAL (type);

	/* Only remove if any of the flags was TRUE */
	if (removable || optical) {
		GString *queries;

		g_debug ("  Removing all resources in store from %s ",
		         optical ? "optical discs" : "removable devices");

		queries = g_string_new ("");

		/* Delete all resources where nie:dataSource is a volume
		 * of the given type */
		g_string_append_printf (queries,
		                        "DELETE { "
		                        "  ?f a rdfs:Resource . "
		                        "  ?ie a rdfs:Resource "
		                        "} WHERE { "
		                        "  ?v a tracker:Volume ; "
		                        "     tracker:isRemovable %s ; "
		                        "     tracker:isOptical %s . "
		                        "  ?f nie:dataSource ?v . "
		                        "  ?ie nie:isStoredAs ?f "
		                        "}",
		                        removable ? "true" : "false",
		                        optical ? "true" : "false");

		tracker_sparql_connection_update_async (tracker_miner_get_connection (TRACKER_MINER (miner)),
		                                        queries->str,
		                                        G_PRIORITY_LOW,
		                                        NULL,
		                                        remove_files_in_removable_media_cb,
		                                        NULL);

		g_string_free (queries, TRUE);

		return TRUE;
	}

	return FALSE;
}

static void
miner_files_in_removable_media_remove_by_date (TrackerMinerFiles  *miner,
                                               const gchar        *date)
{
	GString *queries;

	g_debug ("  Removing all resources in store from removable or "
	         "optical devices not mounted after '%s'",
	         date);

	queries = g_string_new ("");

	/* Delete all resources where nie:dataSource is a volume
	 * which was last unmounted before the given date */
	g_string_append_printf (queries,
	                        "DELETE { "
	                        "  ?f a rdfs:Resource . "
	                        "  ?ie a rdfs:Resource "
	                        "} WHERE { "
	                        "  ?v a tracker:Volume ; "
	                        "     tracker:isRemovable true ; "
	                        "     tracker:isMounted false ; "
	                        "     tracker:unmountDate ?d . "
	                        "  ?f nie:dataSource ?v . "
	                        "  ?ie nie:isStoredAs ?f "
	                        "  FILTER ( ?d < \"%s\") "
	                        "}",
	                        date);

	tracker_sparql_connection_update_async (tracker_miner_get_connection (TRACKER_MINER (miner)),
	                                        queries->str,
	                                        G_PRIORITY_LOW,
	                                        NULL,
	                                        remove_files_in_removable_media_cb,
	                                        NULL);

	g_string_free (queries, TRUE);
}

static void
miner_files_add_removable_or_optical_directory (TrackerMinerFiles *mf,
                                                const gchar       *mount_path,
                                                const gchar       *uuid)
{
	TrackerIndexingTree *indexing_tree;
	TrackerDirectoryFlags flags;
	GFile *mount_point_file;

	mount_point_file = g_file_new_for_path (mount_path);

	/* UUID may be NULL, and if so, get it */
	if (!uuid) {
		uuid = tracker_storage_get_uuid_for_file (mf->private->storage,
		                                          mount_point_file);
		if (!uuid) {
			g_critical ("Couldn't get UUID for mount point '%s'",
			            mount_path);
			g_object_unref (mount_point_file);
			return;
		}
	}

	indexing_tree = tracker_miner_fs_get_indexing_tree (TRACKER_MINER_FS (mf));
	flags = TRACKER_DIRECTORY_FLAG_RECURSE |
		TRACKER_DIRECTORY_FLAG_CHECK_MTIME |
		TRACKER_DIRECTORY_FLAG_PRESERVE |
		TRACKER_DIRECTORY_FLAG_PRIORITY;

	if (tracker_config_get_enable_monitors (mf->private->config)) {
		flags |= TRACKER_DIRECTORY_FLAG_MONITOR;
	}

	g_object_set_qdata_full (G_OBJECT (mount_point_file),
	                         mf->private->quark_mount_point_uuid,
	                         g_strdup (uuid),
	                         (GDestroyNotify) g_free);

	g_message ("  Adding removable/optical: '%s'", mount_path);
	tracker_indexing_tree_add (indexing_tree,
				   mount_point_file,
				   flags);
	g_object_unref (mount_point_file);
}

gboolean
tracker_miner_files_is_file_eligible (TrackerMinerFiles *miner,
                                      GFile             *file)
{
	TrackerConfig *config;
	GFile *dir;
	GFileInfo *file_info;
	gboolean is_dir;

	file_info = g_file_query_info (file,
	                               G_FILE_ATTRIBUTE_STANDARD_TYPE,
	                               G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
	                               NULL, NULL);

	if (!file_info) {
		/* file does not exist */
		return FALSE;
	}

	is_dir = (g_file_info_get_file_type (file_info) == G_FILE_TYPE_DIRECTORY);
	g_object_unref (file_info);

	g_object_get (miner,
	              "config", &config,
	              NULL);

	if (is_dir) {
		dir = g_object_ref (file);
	} else {
		if (!tracker_miner_files_check_file (file,
		                                     tracker_config_get_ignored_file_paths (config),
		                                     tracker_config_get_ignored_file_patterns (config))) {
			/* file is not eligible to be indexed */
			g_object_unref (config);
			return FALSE;
		}

		dir = g_file_get_parent (file);
	}

	if (dir) {
		gboolean found = FALSE;
		GSList *l;

		if (!tracker_miner_files_check_directory (dir,
		                                          tracker_config_get_index_recursive_directories (config),
		                                          tracker_config_get_index_single_directories (config),
		                                          tracker_config_get_ignored_directory_paths (config),
		                                          tracker_config_get_ignored_directory_patterns (config))) {
			/* file is not eligible to be indexed */
			g_object_unref (dir);
			g_object_unref (config);
			return FALSE;
		}

		l = tracker_config_get_index_recursive_directories (config);

		while (l && !found) {
			GFile *config_dir;

			config_dir = g_file_new_for_path ((gchar *) l->data);

			if (g_file_equal (dir, config_dir) ||
			    g_file_has_prefix (dir, config_dir)) {
				found = TRUE;
			}

			g_object_unref (config_dir);
			l = l->next;
		}

		l = tracker_config_get_index_single_directories (config);

		while (l && !found) {
			GFile *config_dir;

			config_dir = g_file_new_for_path ((gchar *) l->data);

			if (g_file_equal (dir, config_dir)) {
				found = TRUE;
			}

			g_object_unref (config_dir);
			l = l->next;
		}

		g_object_unref (dir);

		if (!found) {
			/* file is not eligible to be indexed */
			g_object_unref (config);
			return FALSE;
		}
	}

	g_object_unref (config);

	/* file is eligible to be indexed */
	return TRUE;
}
