/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * Copyright (C) 2008, Nokia

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

#include <libtracker-common/tracker-storage.h>

#include "tracker-miner-crawler.h"
#include "tracker-config.h"
#include "tracker-processor.h"

#define TRACKER_MINER_CRAWLER_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), TRACKER_TYPE_MINER_CRAWLER, TrackerMinerCrawlerPrivate))

typedef struct TrackerMinerCrawlerPrivate TrackerMinerCrawlerPrivate;
typedef struct DirectoryData DirectoryData;

struct DirectoryData {
	GFile *dir;
	guint monitor : 1;
	guint recurse : 1;
};

struct TrackerMinerCrawlerPrivate {
	TrackerConfig *config;
	TrackerStorage *storage;
	TrackerProcessor *processor;

	GArray *dirs;
};

static void tracker_miner_crawler_finalize (GObject *object);

static void tracker_miner_crawler_started (TrackerMiner *miner);


G_DEFINE_ABSTRACT_TYPE (TrackerMinerCrawler, tracker_miner_crawler, TRACKER_TYPE_MINER)

static void
tracker_miner_crawler_class_init (TrackerMinerCrawlerClass *klass)
{
        GObjectClass *object_class = G_OBJECT_CLASS (klass);
        TrackerMinerClass *miner_class = TRACKER_MINER_CLASS (klass);

        object_class->finalize = tracker_miner_crawler_finalize;

        miner_class->started = tracker_miner_crawler_started;

	/*
        miner_class->stopped = tracker_miner_crawler_stopped;
        miner_class->paused = tracker_miner_crawler_paused;
        miner_class->resumed = tracker_miner_crawler_resumed;
	*/

        g_type_class_add_private (object_class, sizeof (TrackerMinerCrawlerPrivate));
}

static void
tracker_miner_crawler_init (TrackerMinerCrawler *miner)
{
	TrackerMinerCrawlerPrivate *priv;

	priv = miner->_priv = TRACKER_MINER_CRAWLER_GET_PRIVATE (miner);

	priv->config = tracker_config_new ();
	priv->storage = tracker_storage_new ();

	priv->processor = tracker_processor_new (priv->config, priv->storage);

	priv->dirs = g_array_new (FALSE, TRUE, sizeof (DirectoryData));
}

static void
tracker_miner_crawler_finalize (GObject *object)
{
        G_OBJECT_CLASS (tracker_miner_crawler_parent_class)->finalize (object);
}

static void
tracker_miner_crawler_started (TrackerMiner *miner)
{
	TrackerMinerCrawler *miner_crawler;
	TrackerMinerCrawlerPrivate *priv;

	miner_crawler = TRACKER_MINER_CRAWLER (miner);
	priv = miner_crawler->_priv;

	tracker_processor_start (priv->processor);
}

/* Public API */
void
tracker_miner_crawler_add_directory (TrackerMinerCrawler *miner,
				     const gchar         *directory_uri,
				     gboolean             monitor,
				     gboolean             recurse)
{
	TrackerMinerCrawlerPrivate *priv;
	DirectoryData dir_data;

	g_return_if_fail (TRACKER_IS_MINER_CRAWLER (miner));
	g_return_if_fail (directory_uri != NULL);

	priv = miner->_priv;

	dir_data.dir = g_file_new_for_uri (directory_uri);
	dir_data.monitor = (monitor == TRUE);
	dir_data.recurse = (recurse == TRUE);

	g_array_append_val (priv->dirs, dir_data);
}

void
tracker_miner_crawler_set_ignore_directory_patterns (TrackerMinerCrawler *miner,
						     GList               *patterns)
{
}

void
tracker_miner_crawler_set_ignore_file_patterns (TrackerMinerCrawler *miner,
						GList               *patterns)
{
}
