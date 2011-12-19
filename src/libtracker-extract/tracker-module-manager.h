/*
 * Copyright (C) 2011, Nokia <ivan.frade@nokia.com>
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

#ifndef __TRACKER_EXTRACT_MODULE_MANAGER_H__
#define __TRACKER_EXTRACT_MODULE_MANAGER_H__

#if !defined (__LIBTRACKER_EXTRACT_INSIDE__) && !defined (TRACKER_COMPILATION)
#error "only <libtracker-extract/tracker-extract.h> must be included directly."
#endif

#include <glib.h>
#include <gmodule.h>

#include <libtracker-sparql/tracker-sparql.h>
#include "tracker-extract-info.h"

G_BEGIN_DECLS

/**
 * TrackerModuleThreadAwareness:
 * @TRACKER_MODULE_NONE: Extractions are completed in the main event
 * loop.
 * @TRACKER_MODULE_MAIN_THREAD: Extractions will be dispatched in the
 * main thread.
 * @TRACKER_MODULE_SINGLE_THREAD: Extractions will be dispatched in a
 * separate thread which is not the main thread. This means a new
 * thread is created and used for all extractions with this value.
 * @TRACKER_MODULE_MULTI_THREAD: A thread pool is used for all
 * extractions of this module. This requires that the module is thread
 * aware.
 *
 * Enumerates the different types of thread awareness which extractor
 * modules need to be aware of. This is useful to know because it
 * changes the way we queue and notify extractions with modules for
 * metadata from files.
 *
 * Since: 0.14
 **/
typedef enum {
	TRACKER_MODULE_NONE,
	TRACKER_MODULE_MAIN_THREAD,
	TRACKER_MODULE_SINGLE_THREAD,
	TRACKER_MODULE_MULTI_THREAD
} TrackerModuleThreadAwareness;

typedef struct _TrackerMimetypeInfo TrackerMimetypeInfo;

typedef gboolean (* TrackerExtractInitFunc)     (TrackerModuleThreadAwareness  *thread_awareness_ret,
                                                 GError                       **error);
typedef void     (* TrackerExtractShutdownFunc) (void);

typedef gboolean (* TrackerExtractMetadataFunc) (TrackerExtractInfo *info);


gboolean  tracker_extract_module_manager_init                (void) G_GNUC_CONST;
GModule * tracker_extract_module_manager_get_for_mimetype    (const gchar                  *mimetype,
                                                              TrackerExtractInitFunc       *init_func,
                                                              TrackerExtractShutdownFunc   *shutdown_func,
                                                              TrackerExtractMetadataFunc   *extract_func);

gboolean  tracker_extract_module_manager_mimetype_is_handled (const gchar                *mimetype);


TrackerMimetypeInfo * tracker_extract_module_manager_get_mimetype_handlers  (const gchar *mimetype);
GStrv                 tracker_extract_module_manager_get_fallback_rdf_types (const gchar *mimetype);

GModule * tracker_mimetype_info_get_module (TrackerMimetypeInfo          *info,
                                            TrackerExtractMetadataFunc   *extract_func,
                                            TrackerModuleThreadAwareness *thread_awareness);
gboolean  tracker_mimetype_info_iter_next  (TrackerMimetypeInfo          *info);
void      tracker_mimetype_info_free       (TrackerMimetypeInfo          *info);

G_END_DECLS

#endif /* __TRACKER_EXTRACT_MODULE_MANAGER_H__ */
