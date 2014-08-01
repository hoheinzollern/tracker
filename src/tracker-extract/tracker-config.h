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

#ifndef __TRACKER_EXTRACT_CONFIG_H__
#define __TRACKER_EXTRACT_CONFIG_H__

#include <glib-object.h>

#include <libtracker-common/tracker-config-file.h>

G_BEGIN_DECLS

#define TRACKER_TYPE_CONFIG         (tracker_config_get_type ())
#define TRACKER_CONFIG(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), TRACKER_TYPE_CONFIG, TrackerConfig))
#define TRACKER_CONFIG_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST ((k), TRACKER_TYPE_CONFIG, TrackerConfigClass))
#define TRACKER_IS_CONFIG(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), TRACKER_TYPE_CONFIG))
#define TRACKER_IS_CONFIG_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), TRACKER_TYPE_CONFIG))
#define TRACKER_CONFIG_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), TRACKER_TYPE_CONFIG, TrackerConfigClass))

typedef struct TrackerConfig      TrackerConfig;
typedef struct TrackerConfigClass TrackerConfigClass;

struct TrackerConfig {
	GSettings parent;
	gpointer priv;
};

struct TrackerConfigClass {
	GSettingsClass parent_class;
};

GType          tracker_config_get_type                (void) G_GNUC_CONST;

TrackerConfig *tracker_config_new                     (void);
gint           tracker_config_get_verbosity           (TrackerConfig *config);
gint           tracker_config_get_sched_idle          (TrackerConfig *config);
gint           tracker_config_get_max_bytes           (TrackerConfig *config);
gint           tracker_config_get_max_media_art_width (TrackerConfig *config);
gboolean       tracker_config_get_wait_for_miner_fs   (TrackerConfig *config);

void           tracker_config_set_verbosity           (TrackerConfig *config,
                                                       gint           value);

G_END_DECLS

#endif /* __TRACKER_EXTRACT_CONFIG_H__ */

