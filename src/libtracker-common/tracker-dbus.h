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

#ifndef __LIBTRACKER_COMMON_DBUS_H__
#define __LIBTRACKER_COMMON_DBUS_H__

#include <gio/gio.h>

G_BEGIN_DECLS

#if !defined (__LIBTRACKER_COMMON_INSIDE__) && !defined (TRACKER_COMPILATION)
#error "only <libtracker-common/tracker-common.h> must be included directly."
#endif

/* Allow bus type override by env var TRACKER_BUS_TYPE */
#define TRACKER_IPC_BUS           tracker_ipc_bus()

#define TRACKER_DBUS_ERROR_DOMAIN "TrackerDBus"
#define TRACKER_DBUS_ERROR        tracker_dbus_error_quark()

#define tracker_gdbus_async_return_if_fail(expr,invocation)	\
	G_STMT_START { \
		if G_LIKELY(expr) { } else { \
			GError *assert_error = NULL; \
	  \
			g_set_error (&assert_error, \
			             TRACKER_DBUS_ERROR, \
			             TRACKER_DBUS_ERROR_ASSERTION_FAILED, \
			             "Assertion `%s' failed", \
			             #expr); \
	  \
			g_dbus_method_invocation_return_gerror (invocation, assert_error); \
			g_clear_error (&assert_error); \
	  \
			return; \
		}; \
	} G_STMT_END

typedef struct _TrackerDBusRequest TrackerDBusRequest;

typedef enum {
	TRACKER_DBUS_EVENTS_TYPE_ADD,
	TRACKER_DBUS_EVENTS_TYPE_UPDATE,
	TRACKER_DBUS_EVENTS_TYPE_DELETE
} TrackerDBusEventsType;

typedef enum {
	TRACKER_DBUS_ERROR_ASSERTION_FAILED,
	TRACKER_DBUS_ERROR_UNSUPPORTED,
	TRACKER_DBUS_ERROR_BROKEN_PIPE
} TrackerDBusError;


GBusType            tracker_ipc_bus                    (void);

GQuark              tracker_dbus_error_quark           (void);

/* Utils */
gchar **            tracker_dbus_slist_to_strv         (GSList                     *list);

/* Requests */
TrackerDBusRequest *tracker_dbus_request_begin         (const gchar                *sender,
                                                        const gchar                *format,
                                                        ...);
void                tracker_dbus_request_end           (TrackerDBusRequest         *request,
                                                        GError                     *error);
void                tracker_dbus_request_comment       (TrackerDBusRequest         *request,
                                                        const gchar                *format,
                                                        ...);
void                tracker_dbus_request_info          (TrackerDBusRequest         *request,
                                                        const gchar                *format,
                                                        ...);
void                tracker_dbus_request_debug         (TrackerDBusRequest         *request,
                                                        const gchar                *format,
                                                        ...);

void                tracker_dbus_enable_client_lookup  (gboolean                    enable);

/* GDBus convenience API */
TrackerDBusRequest *tracker_g_dbus_request_begin       (GDBusMethodInvocation      *invocation,
                                                        const gchar                *format,
                                                        ...);

G_END_DECLS

#endif /* __LIBTRACKER_COMMON_DBUS_H__ */
