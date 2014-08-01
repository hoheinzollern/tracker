/*
 * Copyright (C) 2006, Jamie McCracken <jamiemcc@gnome.org>
 * Copyright (C) 2007, Jason Kivlighn <jkivlighn@gmail.com>
 * Copyright (C) 2007, Creative Commons <http://creativecommons.org>
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

#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <zlib.h>
#include <inttypes.h>

#include <glib/gstdio.h>

#if HAVE_TRACKER_FTS
#include <libtracker-fts/tracker-fts.h>
#endif

#include <libtracker-common/tracker-locale.h>

#include "tracker-class.h"
#include "tracker-data-manager.h"
#include "tracker-data-update.h"
#include "tracker-db-interface-sqlite.h"
#include "tracker-db-manager.h"
#include "tracker-db-journal.h"
#include "tracker-namespace.h"
#include "tracker-ontologies.h"
#include "tracker-ontology.h"
#include "tracker-property.h"
#include "tracker-sparql-query.h"
#include "tracker-data-query.h"

#define XSD_PREFIX TRACKER_XSD_PREFIX
#define RDF_PREFIX TRACKER_RDF_PREFIX
#define RDF_PROPERTY RDF_PREFIX "Property"
#define RDF_TYPE RDF_PREFIX "type"

#define RDFS_PREFIX TRACKER_RDFS_PREFIX
#define RDFS_CLASS RDFS_PREFIX "Class"
#define RDFS_DOMAIN RDFS_PREFIX "domain"
#define RDFS_RANGE RDFS_PREFIX "range"
#define RDFS_SUB_CLASS_OF RDFS_PREFIX "subClassOf"
#define RDFS_SUB_PROPERTY_OF RDFS_PREFIX "subPropertyOf"

#define NRL_PREFIX TRACKER_NRL_PREFIX
#define NRL_INVERSE_FUNCTIONAL_PROPERTY TRACKER_NRL_PREFIX "InverseFunctionalProperty"
#define NRL_MAX_CARDINALITY NRL_PREFIX "maxCardinality"

#define NAO_PREFIX TRACKER_NAO_PREFIX
#define NAO_LAST_MODIFIED NAO_PREFIX "lastModified"

#define TRACKER_PREFIX TRACKER_TRACKER_PREFIX

#define ZLIBBUFSIZ 8192

static gchar    *ontologies_dir;
static gboolean  initialized;
static gboolean  reloading = FALSE;
#ifndef DISABLE_JOURNAL
static gboolean  in_journal_replay;
#endif

typedef struct {
	const gchar *from;
	const gchar *to;
} Conversion;

static Conversion allowed_boolean_conversions[] = {
	{ "false", "true" },
	{ "true", "false" },
	{ NULL, NULL }
};

static Conversion allowed_range_conversions[] = {
	{ XSD_PREFIX "integer", XSD_PREFIX "string" },
	{ XSD_PREFIX "integer", XSD_PREFIX "double" },
	{ XSD_PREFIX "integer", XSD_PREFIX "boolean" },

	{ XSD_PREFIX "string", XSD_PREFIX "integer" },
	{ XSD_PREFIX "string", XSD_PREFIX "double" },
	{ XSD_PREFIX "string", XSD_PREFIX "boolean" },

	{ XSD_PREFIX "double", XSD_PREFIX "integer" },
	{ XSD_PREFIX "double", XSD_PREFIX "string" },
	{ XSD_PREFIX "double", XSD_PREFIX "boolean" },

	{ NULL, NULL }
};

GQuark
tracker_data_ontology_error_quark (void)
{
	return g_quark_from_static_string ("tracker-data-ontology-error-quark");
}

static void
handle_unsupported_ontology_change (const gchar  *ontology_path,
                                    const gchar  *subject,
                                    const gchar  *change,
                                    const gchar  *old,
                                    const gchar  *attempted_new,
                                    GError      **error)
{
#ifndef DISABLE_JOURNAL
	/* force reindex on restart */
	tracker_db_manager_remove_version_file ();
#endif /* DISABLE_JOURNAL */

	g_set_error (error, TRACKER_DATA_ONTOLOGY_ERROR,
	             TRACKER_DATA_UNSUPPORTED_ONTOLOGY_CHANGE,
	             "%s: Unsupported ontology change for %s: can't change %s (old=%s, attempted new=%s)",
	             ontology_path != NULL ? ontology_path : "Unknown",
	             subject != NULL ? subject : "Unknown",
	             change != NULL ? change : "Unknown",
	             old != NULL ? old : "Unknown",
	             attempted_new != NULL ? attempted_new : "Uknown");
}

static void
set_secondary_index_for_single_value_property (TrackerDBInterface  *iface,
                                               const gchar         *service_name,
                                               const gchar         *field_name,
                                               const gchar         *second_field_name,
                                               gboolean             enabled,
                                               GError             **error)
{
	GError *internal_error = NULL;

	g_debug ("Dropping secondary index (single-value property):  "
	         "DROP INDEX IF EXISTS \"%s_%s\"",
	         service_name, field_name);

	tracker_db_interface_execute_query (iface, &internal_error,
	                                    "DROP INDEX IF EXISTS \"%s_%s\"",
	                                    service_name,
	                                    field_name);

	if (internal_error) {
		g_propagate_error (error, internal_error);
		return;
	}

	if (enabled) {
		g_debug ("Creating secondary index (single-value property): "
		         "CREATE INDEX \"%s_%s\" ON \"%s\" (\"%s\", \"%s\")",
		         service_name, field_name, service_name, field_name, second_field_name);

		tracker_db_interface_execute_query (iface, &internal_error,
		                                    "CREATE INDEX \"%s_%s\" ON \"%s\" (\"%s\", \"%s\")",
		                                    service_name,
		                                    field_name,
		                                    service_name,
		                                    field_name,
		                                    second_field_name);

		if (internal_error) {
			g_propagate_error (error, internal_error);
		}
	}
}

static void
set_index_for_single_value_property (TrackerDBInterface  *iface,
                                     const gchar         *service_name,
                                     const gchar         *field_name,
                                     gboolean             enabled,
                                     GError             **error)
{
	GError *internal_error = NULL;

	g_debug ("Dropping index (single-value property): "
	         "DROP INDEX IF EXISTS \"%s_%s\"",
	         service_name, field_name);

	tracker_db_interface_execute_query (iface, &internal_error,
	                                    "DROP INDEX IF EXISTS \"%s_%s\"",
	                                    service_name,
	                                    field_name);

	if (internal_error) {
		g_propagate_error (error, internal_error);
		return;
	}

	if (enabled) {
		g_debug ("Creating index (single-value property): "
		         "CREATE INDEX \"%s_%s\" ON \"%s\" (\"%s\")",
		         service_name, field_name, service_name, field_name);

		tracker_db_interface_execute_query (iface, &internal_error,
		                                    "CREATE INDEX \"%s_%s\" ON \"%s\" (\"%s\")",
		                                    service_name,
		                                    field_name,
		                                    service_name,
		                                    field_name);

		if (internal_error) {
			g_propagate_error (error, internal_error);
		}
	}
}

static void
set_index_for_multi_value_property (TrackerDBInterface  *iface,
                                    const gchar         *service_name,
                                    const gchar         *field_name,
                                    gboolean             enabled,
                                    gboolean             recreate,
                                    GError             **error)
{
	GError *internal_error = NULL;

	g_debug ("Dropping index (multi-value property): "
	         "DROP INDEX IF EXISTS \"%s_%s_ID_ID\"",
	         service_name, field_name);

	tracker_db_interface_execute_query (iface, &internal_error,
	                                    "DROP INDEX IF EXISTS \"%s_%s_ID_ID\"",
	                                    service_name,
	                                    field_name);

	if (internal_error) {
		g_propagate_error (error, internal_error);
		return;
	}

	/* Useful to have this here for the cases where we want to fully
	 * re-create the indexes even without an ontology change (when locale
	 * of the user changes) */
	g_debug ("Dropping index (multi-value property): "
	         "DROP INDEX IF EXISTS \"%s_%s_ID\"",
	         service_name,
	         field_name);
	tracker_db_interface_execute_query (iface, &internal_error,
	                                    "DROP INDEX IF EXISTS \"%s_%s_ID\"",
	                                    service_name,
	                                    field_name);

	if (internal_error) {
		g_propagate_error (error, internal_error);
		return;
	}

	if (!recreate) {
		return;
	}

	if (enabled) {
		g_debug ("Creating index (multi-value property): "
		         "CREATE INDEX \"%s_%s_ID\" ON \"%s_%s\" (ID)",
		         service_name,
		         field_name,
		         service_name,
		         field_name);

		tracker_db_interface_execute_query (iface, &internal_error,
		                                    "CREATE INDEX \"%s_%s_ID\" ON \"%s_%s\" (ID)",
		                                    service_name,
		                                    field_name,
		                                    service_name,
		                                    field_name);

		if (internal_error) {
			g_propagate_error (error, internal_error);
			return;
		}

		g_debug ("Creating index (multi-value property): "
		         "CREATE UNIQUE INDEX \"%s_%s_ID_ID\" ON \"%s_%s\" (\"%s\", ID)",
		         service_name,
		         field_name,
		         service_name,
		         field_name,
		         field_name);

		tracker_db_interface_execute_query (iface, &internal_error,
		                                    "CREATE UNIQUE INDEX \"%s_%s_ID_ID\" ON \"%s_%s\" (\"%s\", ID)",
		                                    service_name,
		                                    field_name,
		                                    service_name,
		                                    field_name,
		                                    field_name);

		if (internal_error) {
			g_propagate_error (error, internal_error);
			return;
		}
	} else {
		g_debug ("Creating index (multi-value property): "
		         "CREATE UNIQUE INDEX \"%s_%s_ID_ID\" ON \"%s_%s\" (ID, \"%s\")",
		         service_name,
		         field_name,
		         service_name,
		         field_name,
		         field_name);

		tracker_db_interface_execute_query (iface, &internal_error,
		                                    "CREATE UNIQUE INDEX \"%s_%s_ID_ID\" ON \"%s_%s\" (ID, \"%s\")",
		                                    service_name,
		                                    field_name,
		                                    service_name,
		                                    field_name,
		                                    field_name);

		if (internal_error) {
			g_propagate_error (error, internal_error);
		}
	}
}

static gboolean
is_allowed_conversion (const gchar *oldv,
                       const gchar *newv,
                       Conversion   allowed[])
{
	guint i;

	for (i = 0; allowed[i].from != NULL; i++) {
		if (g_strcmp0 (allowed[i].from, oldv) == 0) {
			if (g_strcmp0 (allowed[i].to, newv) == 0) {
				return TRUE;
			}
		}
	}

	return FALSE;
}

static gboolean
check_unsupported_property_value_change (const gchar     *ontology_path,
                                         const gchar     *kind,
                                         const gchar     *subject,
                                         const gchar     *predicate,
                                         const gchar     *object)
{
	GError *error = NULL;
	gboolean needed = TRUE;
	gchar *query = NULL;
	TrackerDBCursor *cursor;

	query = g_strdup_printf ("SELECT ?old_value WHERE { "
	                           "<%s> %s ?old_value "
	                         "}", subject, kind);

	cursor = tracker_data_query_sparql_cursor (query, &error);

	if (cursor && tracker_db_cursor_iter_next (cursor, NULL, NULL)) {
		if (g_strcmp0 (object, tracker_db_cursor_get_string (cursor, 0, NULL)) == 0) {
			needed = FALSE;
		} else {
			needed = TRUE;
		}

	} else {
		if (object && (g_strcmp0 (object, "false") == 0)) {
			needed = FALSE;
		} else {
			needed = (object != NULL);
		}
	}

	g_free (query);
	if (cursor) {
		g_object_unref (cursor);
	}

	if (error) {
		g_critical ("Ontology change, %s", error->message);
		g_clear_error (&error);
	}

	return needed;
}

static gboolean
update_property_value (const gchar      *ontology_path,
                       const gchar      *kind,
                       const gchar      *subject,
                       const gchar      *predicate,
                       const gchar      *object,
                       Conversion        allowed[],
                       TrackerClass     *class,
                       TrackerProperty  *property,
                       GError          **error_in)
{
	GError *error = NULL;
	gboolean needed = TRUE;
	gboolean is_new = FALSE;

	if (class) {
		is_new = tracker_class_get_is_new (class);
	} else if (property) {
		is_new = tracker_property_get_is_new (property);
	}

	if (!is_new) {
		gchar *query = NULL;
		TrackerDBCursor *cursor;

		query = g_strdup_printf ("SELECT ?old_value WHERE { "
		                           "<%s> %s ?old_value "
		                         "}", subject, kind);

		cursor = tracker_data_query_sparql_cursor (query, &error);

		if (cursor && tracker_db_cursor_iter_next (cursor, NULL, NULL)) {
			const gchar *str = NULL;

			str = tracker_db_cursor_get_string (cursor, 0, NULL);

			if (g_strcmp0 (object, str) == 0) {
				needed = FALSE;
			} else {
				gboolean unsup_onto_err = FALSE;

				if (allowed && !is_allowed_conversion (str, object, allowed)) {
					handle_unsupported_ontology_change (ontology_path,
					                                    subject,
					                                    kind,
					                                    str,
					                                    object,
					                                    error_in);
					needed = FALSE;
					unsup_onto_err = TRUE;
				}

				if (!unsup_onto_err) {
					tracker_data_delete_statement (NULL, subject, predicate, str, &error);
					if (!error)
						tracker_data_update_buffer_flush (&error);
				}
			}

		} else {
			if (object && (g_strcmp0 (object, "false") == 0)) {
				needed = FALSE;
			} else {
				needed = (object != NULL);
			}
		}
		g_free (query);
		if (cursor) {
			g_object_unref (cursor);
		}
	} else {
		needed = FALSE;
	}


	if (!error && needed && object) {
		tracker_data_insert_statement (NULL, subject,
		                               predicate, object,
		                               &error);
		if (!error)
			tracker_data_update_buffer_flush (&error);
	}

	if (error) {
		g_critical ("Ontology change, %s", error->message);
		g_clear_error (&error);
	}

	return needed;
}

static void
check_range_conversion_is_allowed (const gchar  *ontology_path,
                                   const gchar  *subject,
                                   const gchar  *predicate,
                                   const gchar  *object,
                                   GError      **error)
{
	TrackerDBCursor *cursor;
	gchar *query;

	query = g_strdup_printf ("SELECT ?old_value WHERE { "
	                           "<%s> rdfs:range ?old_value "
	                         "}", subject);

	cursor = tracker_data_query_sparql_cursor (query, NULL);

	g_free (query);

	if (cursor && tracker_db_cursor_iter_next (cursor, NULL, NULL)) {
		const gchar *str;

		str = tracker_db_cursor_get_string (cursor, 0, NULL);

		if (g_strcmp0 (object, str) != 0) {
			if (!is_allowed_conversion (str, object, allowed_range_conversions)) {
				handle_unsupported_ontology_change (ontology_path,
				                                    subject,
				                                    "rdfs:range",
				                                    str,
				                                    object,
				                                    error);
			}
		}
	}

	if (cursor) {
		g_object_unref (cursor);
	}
}

static void
fix_indexed (TrackerProperty  *property,
             gboolean          recreate,
             GError          **error)
{
	GError *internal_error = NULL;
	TrackerDBInterface *iface;
	TrackerClass *class;
	const gchar *service_name;
	const gchar *field_name;

	iface = tracker_db_manager_get_db_interface ();

	class = tracker_property_get_domain (property);
	field_name = tracker_property_get_name (property);
	service_name = tracker_class_get_name (class);

	if (tracker_property_get_multiple_values (property)) {
		set_index_for_multi_value_property (iface, service_name, field_name,
		                                    tracker_property_get_indexed (property),
		                                    recreate,
		                                    &internal_error);
	} else {
		TrackerProperty *secondary_index;
		TrackerClass **domain_index_classes;

		secondary_index = tracker_property_get_secondary_index (property);
		if (secondary_index == NULL) {
			set_index_for_single_value_property (iface, service_name, field_name,
			                                     recreate && tracker_property_get_indexed (property),
			                                     &internal_error);
		} else {
			set_secondary_index_for_single_value_property (iface, service_name, field_name,
			                                               tracker_property_get_name (secondary_index),
			                                               recreate && tracker_property_get_indexed (property),
			                                               &internal_error);
		}

		/* single-valued properties may also have domain-specific indexes */
		domain_index_classes = tracker_property_get_domain_indexes (property);
		while (!internal_error && domain_index_classes && *domain_index_classes) {
			set_index_for_single_value_property (iface,
			                                     tracker_class_get_name (*domain_index_classes),
			                                     field_name,
			                                     recreate,
			                                     &internal_error);
			domain_index_classes++;
		}
	}

	if (internal_error) {
		g_propagate_error (error, internal_error);
	}
}

static void
tracker_data_ontology_load_statement (const gchar *ontology_path,
                                      gint         subject_id,
                                      const gchar *subject,
                                      const gchar *predicate,
                                      const gchar *object,
                                      gint        *max_id,
                                      gboolean     in_update,
                                      GHashTable  *classes,
                                      GHashTable  *properties,
                                      GPtrArray   *seen_classes,
                                      GPtrArray   *seen_properties,
                                      GError     **error)
{
	if (g_strcmp0 (predicate, RDF_TYPE) == 0) {
		if (g_strcmp0 (object, RDFS_CLASS) == 0) {
			TrackerClass *class;
			class = tracker_ontologies_get_class_by_uri (subject);

			if (class != NULL) {
				if (seen_classes)
					g_ptr_array_add (seen_classes, g_object_ref (class));
				if (!in_update) {
					g_critical ("%s: Duplicate definition of class %s", ontology_path, subject);
				} else {
					/* Reset for a correct post-check */
					tracker_class_reset_domain_indexes (class);
					tracker_class_reset_super_classes (class);
					tracker_class_set_notify (class, FALSE);
				}
				return;
			}

			if (subject_id == 0) {
				subject_id = ++(*max_id);
			}

			class = tracker_class_new (FALSE);
			tracker_class_set_is_new (class, in_update);
			tracker_class_set_uri (class, subject);
			tracker_class_set_id (class, subject_id);
			tracker_ontologies_add_class (class);
			tracker_ontologies_add_id_uri_pair (subject_id, subject);

			if (seen_classes)
				g_ptr_array_add (seen_classes, g_object_ref (class));

			if (classes) {
				g_hash_table_insert (classes, GINT_TO_POINTER (subject_id), class);
			} else {
				g_object_unref (class);
			}

		} else if (g_strcmp0 (object, RDF_PROPERTY) == 0) {
			TrackerProperty *property;

			property = tracker_ontologies_get_property_by_uri (subject);
			if (property != NULL) {
				if (seen_properties)
					g_ptr_array_add (seen_properties, g_object_ref (property));
				if (!in_update) {
					g_critical ("%s: Duplicate definition of property %s", ontology_path, subject);
				} else {
					/* Reset for a correct post and pre-check */
					tracker_property_set_last_multiple_values (property, TRUE);
					tracker_property_reset_domain_indexes (property);
					tracker_property_reset_super_properties (property);
					tracker_property_set_indexed (property, FALSE);
					tracker_property_set_secondary_index (property, NULL);
					tracker_property_set_writeback (property, FALSE);
					tracker_property_set_is_inverse_functional_property (property, FALSE);
					tracker_property_set_default_value (property, NULL);
				}
				return;
			}

			if (subject_id == 0) {
				subject_id = ++(*max_id);
			}

			property = tracker_property_new (FALSE);
			tracker_property_set_is_new (property, in_update);
			tracker_property_set_uri (property, subject);
			tracker_property_set_id (property, subject_id);
			tracker_ontologies_add_property (property);
			tracker_ontologies_add_id_uri_pair (subject_id, subject);

			if (seen_properties)
				g_ptr_array_add (seen_properties, g_object_ref (property));

			if (properties) {
				g_hash_table_insert (properties, GINT_TO_POINTER (subject_id), property);
			} else {
				g_object_unref (property);
			}

		} else if (g_strcmp0 (object, NRL_INVERSE_FUNCTIONAL_PROPERTY) == 0) {
			TrackerProperty *property;

			property = tracker_ontologies_get_property_by_uri (subject);
			if (property == NULL) {
				g_critical ("%s: Unknown property %s", ontology_path, subject);
				return;
			}

			tracker_property_set_is_inverse_functional_property (property, TRUE);
		} else if (g_strcmp0 (object, TRACKER_PREFIX "Namespace") == 0) {
			TrackerNamespace *namespace;

			if (tracker_ontologies_get_namespace_by_uri (subject) != NULL) {
				if (!in_update)
					g_critical ("%s: Duplicate definition of namespace %s", ontology_path, subject);
				return;
			}

			namespace = tracker_namespace_new (FALSE);
			tracker_namespace_set_is_new (namespace, in_update);
			tracker_namespace_set_uri (namespace, subject);
			tracker_ontologies_add_namespace (namespace);
			g_object_unref (namespace);

		} else if (g_strcmp0 (object, TRACKER_PREFIX "Ontology") == 0) {
			TrackerOntology *ontology;

			if (tracker_ontologies_get_ontology_by_uri (subject) != NULL) {
				if (!in_update)
					g_critical ("%s: Duplicate definition of ontology %s", ontology_path, subject);
				return;
			}

			ontology = tracker_ontology_new ();
			tracker_ontology_set_is_new (ontology, in_update);
			tracker_ontology_set_uri (ontology, subject);
			tracker_ontologies_add_ontology (ontology);
			g_object_unref (ontology);

		}
	} else if (g_strcmp0 (predicate, RDFS_SUB_CLASS_OF) == 0) {
		TrackerClass *class, *super_class;
		gboolean is_new;

		class = tracker_ontologies_get_class_by_uri (subject);
		if (class == NULL) {
			g_critical ("%s: Unknown class %s", ontology_path, subject);
			return;
		}

		is_new = tracker_class_get_is_new (class);
		if (is_new != in_update) {
			gboolean ignore = FALSE;
			/* Detect unsupported ontology change (this needs a journal replay) */
			if (in_update == TRUE && is_new == FALSE && g_strcmp0 (object, RDFS_PREFIX "Resource") != 0) {
				TrackerClass **super_classes = tracker_class_get_super_classes (class);
				gboolean had = FALSE;

				super_class = tracker_ontologies_get_class_by_uri (object);
				if (super_class == NULL) {
					g_critical ("%s: Unknown class %s", ontology_path, object);
					return;
				}

				while (*super_classes) {
					if (*super_classes == super_class) {
						ignore = TRUE;
						g_debug ("%s: Class %s already has rdfs:subClassOf in %s",
						         ontology_path, object, subject);
						break;
					}
					super_classes++;
				}

				super_classes = tracker_class_get_last_super_classes (class);
				if (super_classes) {
					while (*super_classes) {
						if (super_class == *super_classes) {
							had = TRUE;
						}
						super_classes++;
					}
				}

				/* This doesn't detect removed rdfs:subClassOf situations, it
				 * only checks whether no new ones are being added. For
				 * detecting the removal of a rdfs:subClassOf, please check the
				 * tracker_data_ontology_process_changes_pre_db stuff */


				if (!ignore && !had) {
					handle_unsupported_ontology_change (ontology_path,
					                                    tracker_class_get_name (class),
					                                    "rdfs:subClassOf",
					                                    "-",
					                                    tracker_class_get_name (super_class),
					                                    error);
				}
			}

			if (!ignore) {
				super_class = tracker_ontologies_get_class_by_uri (object);
				tracker_class_add_super_class (class, super_class);
			}

			return;
		}

		super_class = tracker_ontologies_get_class_by_uri (object);
		if (super_class == NULL) {
			g_critical ("%s: Unknown class %s", ontology_path, object);
			return;
		}

		tracker_class_add_super_class (class, super_class);

	} else if (g_strcmp0 (predicate, TRACKER_PREFIX "notify") == 0) {
		TrackerClass *class;

		class = tracker_ontologies_get_class_by_uri (subject);

		if (class == NULL) {
			g_critical ("%s: Unknown class %s", ontology_path, subject);
			return;
		}

		tracker_class_set_notify (class, (strcmp (object, "true") == 0));
	} else if (g_strcmp0 (predicate, TRACKER_PREFIX "domainIndex") == 0) {
		TrackerClass *class;
		TrackerProperty *property;
		TrackerProperty **properties;
		gboolean ignore = FALSE;
		gboolean had = FALSE;
		guint n_props, i;

		class = tracker_ontologies_get_class_by_uri (subject);

		if (class == NULL) {
			g_critical ("%s: Unknown class %s", ontology_path, subject);
			return;
		}

		property = tracker_ontologies_get_property_by_uri (object);

		if (property == NULL) {

			/* In this case the import of the TTL will still make the introspection
			 * have the URI set as a tracker:domainIndex for class. The critical
			 * will have happened, but future operations might not cope with this
			 * situation. TODO: add error handling so that the entire ontology
			 * change operation is discarded, for example ignore the entire
			 * .ontology file and rollback all changes that happened. I would
			 * prefer a hard abort() here over a g_critical(), to be honest.
			 *
			 * Of course don't we yet allow just anybody to alter the ontology
			 * files. So very serious is this lack of thorough error handling
			 * not. Let's just not make mistakes when changing the .ontology
			 * files for now. */

			g_critical ("%s: Unknown property %s for tracker:domainIndex in %s."
			            "Don't release this .ontology change!",
			            ontology_path, object, subject);
			return;
		}

		if (tracker_property_get_multiple_values (property)) {
			g_critical ("%s: Property %s has multiple values while trying to add it as tracker:domainIndex in %s, this isn't supported",
			            ontology_path, object, subject);
			return;
		}

		properties = tracker_ontologies_get_properties (&n_props);
		for (i = 0; i < n_props; i++) {
			if (tracker_property_get_domain (properties[i]) == class &&
			    properties[i] == property) {
				g_critical ("%s: Property %s is already a first-class property of %s while trying to add it as tracker:domainIndex",
				            ontology_path, object, subject);
			}
		}

		properties = tracker_class_get_domain_indexes (class);
		while (*properties) {
			if (property == *properties) {
				g_debug ("%s: Property %s already a tracker:domainIndex in %s",
				         ontology_path, object, subject);
				ignore = TRUE;
			}
			properties++;
		}

		properties = tracker_class_get_last_domain_indexes (class);
		if (properties) {
			while (*properties) {
				if (property == *properties) {
					had = TRUE;
				}
				properties++;
			}
		}

		/* This doesn't detect removed tracker:domainIndex situations, it
		 * only checks whether no new ones are being added. For
		 * detecting the removal of a tracker:domainIndex, please check the
		 * tracker_data_ontology_process_changes_pre_db stuff */

		if (!ignore) {
			if (!had) {
				tracker_property_set_is_new_domain_index (property, class, in_update);
			}
			tracker_class_add_domain_index (class, property);
			tracker_property_add_domain_index (property, class);
		}

	} else if (g_strcmp0 (predicate, TRACKER_PREFIX "writeback") == 0) {
		TrackerProperty *property;

		property = tracker_ontologies_get_property_by_uri (subject);

		if (property == NULL) {
			g_critical ("%s: Unknown property %s", ontology_path, subject);
			return;
		}

		tracker_property_set_writeback (property, (strcmp (object, "true") == 0));
	} else if (g_strcmp0 (predicate, TRACKER_PREFIX "forceJournal") == 0) {
		TrackerProperty *property;

		property = tracker_ontologies_get_property_by_uri (subject);

		if (property == NULL) {
			g_critical ("%s: Unknown property %s", ontology_path, subject);
			return;
		}

		tracker_property_set_force_journal (property, (strcmp (object, "true") == 0));
	} else if (g_strcmp0 (predicate, RDFS_SUB_PROPERTY_OF) == 0) {
		TrackerProperty *property, *super_property;
		gboolean is_new;

		property = tracker_ontologies_get_property_by_uri (subject);
		if (property == NULL) {
			g_critical ("%s: Unknown property %s", ontology_path, subject);
			return;
		}

		is_new = tracker_property_get_is_new (property);
		if (is_new != in_update) {
			gboolean ignore = FALSE;
			/* Detect unsupported ontology change (this needs a journal replay) */
			if (in_update == TRUE && is_new == FALSE) {
				TrackerProperty **super_properties = tracker_property_get_super_properties (property);
				gboolean had = FALSE;

				super_property = tracker_ontologies_get_property_by_uri (object);
				if (super_property == NULL) {
					g_critical ("%s: Unknown property %s", ontology_path, object);
					return;
				}

				while (*super_properties) {
					if (*super_properties == super_property) {
						ignore = TRUE;
						g_debug ("%s: Property %s already has rdfs:subPropertyOf in %s",
						         ontology_path, object, subject);
						break;
					}
					super_properties++;
				}

				super_properties = tracker_property_get_last_super_properties (property);
				if (super_properties) {
					while (*super_properties) {
						if (super_property == *super_properties) {
							had = TRUE;
						}
						super_properties++;
					}
				}

				/* This doesn't detect removed rdfs:subPropertyOf situations, it
				 * only checks whether no new ones are being added. For
				 * detecting the removal of a rdfs:subPropertyOf, please check the
				 * tracker_data_ontology_process_changes_pre_db stuff */

				if (!ignore && !had) {
					handle_unsupported_ontology_change (ontology_path,
					                                    tracker_property_get_name (property),
					                                    "rdfs:subPropertyOf",
					                                    "-",
					                                    tracker_property_get_name (super_property),
					                                    error);
				}
			}

			if (!ignore) {
				super_property = tracker_ontologies_get_property_by_uri (object);
				tracker_property_add_super_property (property, super_property);
			}

			return;
		}

		super_property = tracker_ontologies_get_property_by_uri (object);
		if (super_property == NULL) {
			g_critical ("%s: Unknown property %s", ontology_path, object);
			return;
		}

		tracker_property_add_super_property (property, super_property);
	} else if (g_strcmp0 (predicate, RDFS_DOMAIN) == 0) {
		TrackerProperty *property;
		TrackerClass *domain;
		gboolean is_new;

		property = tracker_ontologies_get_property_by_uri (subject);
		if (property == NULL) {
			g_critical ("%s: Unknown property %s", ontology_path, subject);
			return;
		}

		domain = tracker_ontologies_get_class_by_uri (object);
		if (domain == NULL) {
			g_critical ("%s: Unknown class %s", ontology_path, object);
			return;
		}

		is_new = tracker_property_get_is_new (property);
		if (is_new != in_update) {
			/* Detect unsupported ontology change (this needs a journal replay) */
			if (in_update == TRUE && is_new == FALSE) {
				TrackerClass *old_domain = tracker_property_get_domain (property);
				if (old_domain != domain) {
					handle_unsupported_ontology_change (ontology_path,
					                                    tracker_property_get_name (property),
					                                    "rdfs:domain",
					                                    tracker_class_get_name (old_domain),
					                                    tracker_class_get_name (domain),
					                                    error);
				}
			}
			return;
		}

		tracker_property_set_domain (property, domain);
	} else if (g_strcmp0 (predicate, RDFS_RANGE) == 0) {
		TrackerProperty *property;
		TrackerClass *range;

		property = tracker_ontologies_get_property_by_uri (subject);
		if (property == NULL) {
			g_critical ("%s: Unknown property %s", ontology_path, subject);
			return;
		}

		if (tracker_property_get_is_new (property) != in_update) {
			GError *err = NULL;
			check_range_conversion_is_allowed (ontology_path,
			                                   subject,
			                                   predicate,
			                                   object,
			                                   &err);
			if (err) {
				g_propagate_error (error, err);
				return;
			}
		}

		range = tracker_ontologies_get_class_by_uri (object);
		if (range == NULL) {
			g_critical ("%s: Unknown class %s", ontology_path, object);
			return;
		}

		tracker_property_set_range (property, range);
	} else if (g_strcmp0 (predicate, NRL_MAX_CARDINALITY) == 0) {
		TrackerProperty *property;
		gboolean is_new;

		property = tracker_ontologies_get_property_by_uri (subject);
		if (property == NULL) {
			g_critical ("%s: Unknown property %s", ontology_path, subject);
			return;
		}

		/* This doesn't detect removed nrl:maxCardinality situations, it
		 * only checks whether the existing one got changed. For
		 * detecting the removal of a nrl:maxCardinality, please check the
		 * tracker_data_ontology_process_changes_pre_db stuff */

		is_new = tracker_property_get_is_new (property);
		if (is_new != in_update) {
			/* Detect unsupported ontology change (this needs a journal replay) */
			if (in_update == TRUE && is_new == FALSE) {
				if (check_unsupported_property_value_change (ontology_path,
				                                             "nrl:maxCardinality",
				                                             subject,
				                                             predicate,
				                                             object)) {
					handle_unsupported_ontology_change (ontology_path,
					                                    tracker_property_get_name (property),
					                                    "nrl:maxCardinality",
					                                    tracker_property_get_multiple_values (property) ? "1" : "0",
					                                    (atoi (object) == 1)  ? "1" : "0",
					                                    error);
					return;
				}
			}
		}

		if (atoi (object) == 1) {
			tracker_property_set_multiple_values (property, FALSE);
			tracker_property_set_last_multiple_values (property, FALSE);
		} else {
			tracker_property_set_multiple_values (property, TRUE);
			tracker_property_set_last_multiple_values (property, TRUE);
		}

	} else if (g_strcmp0 (predicate, TRACKER_PREFIX "indexed") == 0) {
		TrackerProperty *property;

		property = tracker_ontologies_get_property_by_uri (subject);
		if (property == NULL) {
			g_critical ("%s: Unknown property %s", ontology_path, subject);
			return;
		}

		tracker_property_set_indexed (property, (strcmp (object, "true") == 0));
	} else if (g_strcmp0 (predicate, TRACKER_PREFIX "secondaryIndex") == 0) {
		TrackerProperty *property, *secondary_index;

		property = tracker_ontologies_get_property_by_uri (subject);
		if (property == NULL) {
			g_critical ("%s: Unknown property %s", ontology_path, subject);
			return;
		}

		secondary_index = tracker_ontologies_get_property_by_uri (object);
		if (secondary_index == NULL) {
			g_critical ("%s: Unknown property %s", ontology_path, object);
			return;
		}

		tracker_property_set_secondary_index (property, secondary_index);
	} else if (g_strcmp0 (predicate, TRACKER_PREFIX "transient") == 0) {
		TrackerProperty *property;
		gboolean is_new;

		property = tracker_ontologies_get_property_by_uri (subject);
		if (property == NULL) {
			g_critical ("%s: Unknown property %s", ontology_path, subject);
			return;
		}

		is_new = tracker_property_get_is_new (property);
		if (is_new != in_update) {
			/* Detect unsupported ontology change (this needs a journal replay).
			 * Wouldn't be very hard to support this, just dropping the tabtle
			 * or creating the table in the-non memdisk db file, but afaik this
			 * isn't supported right now */
			if (in_update == TRUE && is_new == FALSE) {
				if (check_unsupported_property_value_change (ontology_path,
				                                             "tracker:transient",
				                                             subject,
				                                             predicate,
				                                             object)) {
					handle_unsupported_ontology_change (ontology_path,
					                                    tracker_property_get_name (property),
					                                    "tracker:transient",
					                                    tracker_property_get_transient (property) ? "true" : "false",
					                                    g_strcmp0 (object, "true") ==0 ? "true" : "false",
					                                    error);
				}
			}
			return;
		}

		if (g_strcmp0 (object, "true") == 0) {
			tracker_property_set_transient (property, TRUE);
		}
	} else if (g_strcmp0 (predicate, TRACKER_PREFIX "fulltextIndexed") == 0) {
		TrackerProperty *property;
		gboolean is_new;

		property = tracker_ontologies_get_property_by_uri (subject);
		if (property == NULL) {
			g_critical ("%s: Unknown property %s", ontology_path, subject);
			return;
		}

		is_new = tracker_property_get_is_new (property);
		if (is_new != in_update) {
			/* Detect unsupported ontology change (this needs a journal replay) */
			if (in_update == TRUE && is_new == FALSE) {
				if (check_unsupported_property_value_change (ontology_path,
				                                             "tracker:fulltextIndexed",
				                                             subject,
				                                             predicate,
				                                             object)) {
					handle_unsupported_ontology_change (ontology_path,
					                                    tracker_property_get_name (property),
					                                    "tracker:fulltextIndexed",
					                                    tracker_property_get_fulltext_indexed (property) ? "true" : "false",
					                                    g_strcmp0 (object, "true") == 0 ? "true" : "false",
					                                    error);
				}
			}
			return;
		}

		if (strcmp (object, "true") == 0) {
			tracker_property_set_fulltext_indexed (property, TRUE);
		}
	} else if (g_strcmp0 (predicate, TRACKER_PREFIX "defaultValue") == 0) {
		TrackerProperty *property;

		property = tracker_ontologies_get_property_by_uri (subject);
		if (property == NULL) {
			g_critical ("%s: Unknown property %s", ontology_path, subject);
			return;
		}

		tracker_property_set_default_value (property, object);
	} else if (g_strcmp0 (predicate, TRACKER_PREFIX "prefix") == 0) {
		TrackerNamespace *namespace;

		namespace = tracker_ontologies_get_namespace_by_uri (subject);
		if (namespace == NULL) {
			g_critical ("%s: Unknown namespace %s", ontology_path, subject);
			return;
		}

		if (tracker_namespace_get_is_new (namespace) != in_update) {
			return;
		}

		tracker_namespace_set_prefix (namespace, object);
	} else if (g_strcmp0 (predicate, NAO_LAST_MODIFIED) == 0) {
		TrackerOntology *ontology;

		ontology = tracker_ontologies_get_ontology_by_uri (subject);
		if (ontology == NULL) {
			g_critical ("%s: Unknown ontology %s", ontology_path, subject);
			return;
		}

		if (tracker_ontology_get_is_new (ontology) != in_update) {
			return;
		}

		tracker_ontology_set_last_modified (ontology, tracker_string_to_date (object, NULL, NULL));
	}
}


static void
check_for_deleted_domain_index (TrackerClass *class)
{
	TrackerProperty **last_domain_indexes;
	GSList *hfound = NULL, *deleted = NULL;

	last_domain_indexes = tracker_class_get_last_domain_indexes (class);

	if (!last_domain_indexes) {
		return;
	}

	while (*last_domain_indexes) {
		TrackerProperty *last_domain_index = *last_domain_indexes;
		gboolean found = FALSE;
		TrackerProperty **domain_indexes;

		domain_indexes = tracker_class_get_domain_indexes (class);

		while (*domain_indexes) {
			TrackerProperty *domain_index = *domain_indexes;
			if (last_domain_index == domain_index) {
				found = TRUE;
				hfound = g_slist_prepend (hfound, domain_index);
				break;
			}
			domain_indexes++;
		}

		if (!found) {
			deleted = g_slist_prepend (deleted, last_domain_index);
		}

		last_domain_indexes++;
	}


	if (deleted) {
		GSList *l;
		TrackerProperty **properties;
		guint n_props, i;

		tracker_class_set_db_schema_changed (class, TRUE);

		properties = tracker_ontologies_get_properties (&n_props);
		for (i = 0; i < n_props; i++) {
			if (tracker_property_get_domain (properties[i]) == class &&
			    !tracker_property_get_multiple_values (properties[i])) {

				/* These aren't domain-indexes, but it's just a flag for the
				 * functionality that'll recreate the table to know that the
				 * property must be involved in the recreation and copy */

				tracker_property_set_is_new_domain_index (properties[i], class, TRUE);
			}
		}

		for (l = hfound; l != NULL; l = l->next) {
			TrackerProperty *prop = l->data;
			g_debug ("Ontology change: keeping tracker:domainIndex: %s",
			         tracker_property_get_name (prop));
			tracker_property_set_is_new_domain_index (prop, class, TRUE);
		}

		for (l = deleted; l != NULL; l = l->next) {
			GError *error = NULL;
			TrackerProperty *prop = l->data;

			g_debug ("Ontology change: deleting tracker:domainIndex: %s",
			         tracker_property_get_name (prop));
			tracker_property_del_domain_index (prop, class);
			tracker_class_del_domain_index (class, prop);

			tracker_data_delete_statement (NULL, tracker_class_get_uri (class),
			                               TRACKER_PREFIX "domainIndex",
			                               tracker_property_get_uri (prop),
			                               &error);

			if (error) {
				g_critical ("Ontology change, %s", error->message);
				g_clear_error (&error);
			} else {
				tracker_data_update_buffer_flush (&error);
				if (error) {
					g_critical ("Ontology change, %s", error->message);
					g_clear_error (&error);
				}
			}
		}

		g_slist_free (deleted);
	}

	g_slist_free (hfound);
}

static void
check_for_deleted_super_classes (TrackerClass  *class,
                                 GError       **error)
{
	TrackerClass **last_super_classes;

	last_super_classes = tracker_class_get_last_super_classes (class);

	if (!last_super_classes) {
		return;
	}

	while (*last_super_classes) {
		TrackerClass *last_super_class = *last_super_classes;
		gboolean found = FALSE;
		TrackerClass **super_classes;

		if (g_strcmp0 (tracker_class_get_uri (last_super_class), RDFS_PREFIX "Resource") == 0) {
			last_super_classes++;
			continue;
		}

		super_classes = tracker_class_get_super_classes (class);

		while (*super_classes) {
			TrackerClass *super_class = *super_classes;

			if (last_super_class == super_class) {
				found = TRUE;
				break;
			}
			super_classes++;
		}

		if (!found) {
			const gchar *ontology_path = "Unknown";
			const gchar *subject = tracker_class_get_uri (class);

			handle_unsupported_ontology_change (ontology_path,
			                                    subject,
			                                    "rdfs:subClassOf", "-", "-",
			                                    error);
			return;
		}

		last_super_classes++;
	}
}


static void
check_for_deleted_super_properties (TrackerProperty  *property,
                                    GError          **error)
{
	TrackerProperty **last_super_properties;
	GList *to_remove = NULL;

	last_super_properties = tracker_property_get_last_super_properties (property);

	if (!last_super_properties) {
		return;
	}

	while (*last_super_properties) {
		TrackerProperty *last_super_property = *last_super_properties;
		gboolean found = FALSE;
		TrackerProperty **super_properties;

		super_properties = tracker_property_get_super_properties (property);

		while (*super_properties) {
			TrackerProperty *super_property = *super_properties;

			if (last_super_property == super_property) {
				found = TRUE;
				break;
			}
			super_properties++;
		}

		if (!found) {
			to_remove = g_list_prepend (to_remove, last_super_property);
		}

		last_super_properties++;
	}

	if (to_remove) {
		GList *copy = to_remove;

		while (copy) {
			GError *n_error = NULL;
			TrackerProperty *prop_to_remove = copy->data;
			const gchar *object = tracker_property_get_uri (prop_to_remove);
			const gchar *subject = tracker_property_get_uri (property);

			tracker_property_del_super_property (property, prop_to_remove);

			tracker_data_delete_statement (NULL, subject,
			                               RDFS_PREFIX "subPropertyOf",
			                               object, &n_error);

			if (!n_error) {
				tracker_data_update_buffer_flush (&n_error);
			}

			if (n_error) {
				g_propagate_error (error, n_error);
				return;
			}

			copy = copy->next;
		}
		g_list_free (to_remove);
	}
}

static void
tracker_data_ontology_process_changes_pre_db (GPtrArray  *seen_classes,
                                              GPtrArray  *seen_properties,
                                              GError    **error)
{
	gint i;
	if (seen_classes) {
		for (i = 0; i < seen_classes->len; i++) {
			GError *n_error = NULL;
			TrackerClass *class = g_ptr_array_index (seen_classes, i);

			check_for_deleted_domain_index (class);
			check_for_deleted_super_classes (class, &n_error);

			if (n_error) {
				g_propagate_error (error, n_error);
				return;
			}
		}
	}

	if (seen_properties) {
		for (i = 0; i < seen_properties->len; i++) {
			GError *n_error = NULL;

			TrackerProperty *property = g_ptr_array_index (seen_properties, i);
			gboolean last_multiple_values = tracker_property_get_last_multiple_values (property);

			check_for_deleted_super_properties (property, &n_error);

			if (n_error) {
				g_propagate_error (error, n_error);
				return;
			}

			if (tracker_property_get_is_new (property) == FALSE &&
			    last_multiple_values != tracker_property_get_multiple_values (property)) {
				const gchar *ontology_path = "Unknown";
				const gchar *subject = tracker_property_get_uri (property);

				handle_unsupported_ontology_change (ontology_path,
				                                    subject,
				                                    "nrl:maxCardinality", "1", "0",
				                                    error);
				return;
			}
		}
	}
}

static void
tracker_data_ontology_process_changes_post_db (GPtrArray  *seen_classes,
                                               GPtrArray  *seen_properties,
                                               GError    **error)
{
	gint i;
	/* TODO: Collect the ontology-paths of the seen events for proper error reporting */
	const gchar *ontology_path = "Unknown";

	/* This updates property-property changes and marks classes for necessity
	 * of having their tables recreated later. There's support for
	 * tracker:notify, tracker:writeback and tracker:indexed */

	if (seen_classes) {
		for (i = 0; i < seen_classes->len; i++) {
			TrackerClass *class = g_ptr_array_index (seen_classes, i);
			const gchar *subject;
			GError *n_error = NULL;

			subject = tracker_class_get_uri (class);

			if (tracker_class_get_notify (class)) {
				update_property_value (ontology_path,
				                       "tracker:notify",
				                       subject,
				                       TRACKER_PREFIX "notify",
				                       "true", allowed_boolean_conversions,
				                       class, NULL, &n_error);
			} else {
				update_property_value (ontology_path,
				                       "tracker:notify",
				                       subject,
				                       TRACKER_PREFIX "notify",
				                       "false", allowed_boolean_conversions,
				                       class, NULL, &n_error);
			}

			if (n_error) {
				g_propagate_error (error, n_error);
				return;
			}
		}
	}

	if (seen_properties) {
		for (i = 0; i < seen_properties->len; i++) {
			TrackerProperty *property = g_ptr_array_index (seen_properties, i);
			const gchar *subject;
			gchar *query;
			TrackerProperty *secondary_index;
			gboolean indexed_set = FALSE, in_onto;
			GError *n_error = NULL;
			TrackerSparqlCursor *cursor;

			subject = tracker_property_get_uri (property);

			/* Check for nrl:InverseFunctionalProperty changes (not supported) */
			in_onto = tracker_property_get_is_inverse_functional_property (property);

			query = g_strdup_printf ("ASK { <%s> a nrl:InverseFunctionalProperty }", subject);
			cursor = TRACKER_SPARQL_CURSOR (tracker_data_query_sparql_cursor (query, &n_error));
			g_free (query);

			if (n_error) {
				g_propagate_error (error, n_error);
				return;
			}

			if (tracker_sparql_cursor_next (cursor, NULL, NULL)) {
				if (tracker_sparql_cursor_get_boolean (cursor, 0) != in_onto) {
					handle_unsupported_ontology_change (ontology_path,
					                                    subject,
					                                    "nrl:InverseFunctionalProperty", "-", "-",
					                                    &n_error);

					if (n_error) {
						g_object_unref (cursor);
						g_propagate_error (error, n_error);
						return;
					}
				}
			}

			if (cursor) {
				g_object_unref (cursor);
			}

			/* Check for possibly supported changes */
			if (tracker_property_get_writeback (property)) {
				update_property_value (ontology_path,
				                       "tracker:writeback",
				                       subject,
				                       TRACKER_PREFIX "writeback",
				                       "true", allowed_boolean_conversions,
				                       NULL, property, &n_error);
			} else {
				update_property_value (ontology_path,
				                       "tracker:writeback",
				                       subject,
				                       TRACKER_PREFIX "writeback",
				                       "false", allowed_boolean_conversions,
				                       NULL, property, &n_error);
			}

			if (n_error) {
				g_propagate_error (error, n_error);
				return;
			}

			if (tracker_property_get_indexed (property)) {
				if (update_property_value (ontology_path,
				                           "tracker:indexed",
				                           subject,
				                           TRACKER_PREFIX "indexed",
				                           "true", allowed_boolean_conversions,
				                           NULL, property, &n_error)) {
					fix_indexed (property, TRUE, &n_error);
					indexed_set = TRUE;
				}
			} else {
				if (update_property_value (ontology_path,
				                           "tracker:indexed",
				                           subject,
				                           TRACKER_PREFIX "indexed",
				                           "false", allowed_boolean_conversions,
				                           NULL, property, &n_error)) {
					fix_indexed (property, TRUE, &n_error);
					indexed_set = TRUE;
				}
			}

			if (n_error) {
				g_propagate_error (error, n_error);
				return;
			}

			secondary_index = tracker_property_get_secondary_index (property);

			if (secondary_index) {
				if (update_property_value (ontology_path,
				                           "tracker:secondaryIndex",
				                           subject,
				                           TRACKER_PREFIX "secondaryIndex",
				                           tracker_property_get_uri (secondary_index), NULL,
				                           NULL, property, &n_error)) {
					if (!indexed_set) {
						fix_indexed (property, TRUE, &n_error);
					}
				}
			} else {
				if (update_property_value (ontology_path,
				                           "tracker:secondaryIndex",
				                           subject,
				                           TRACKER_PREFIX "secondaryIndex",
				                           NULL, NULL,
				                           NULL, property, &n_error)) {
					if (!indexed_set) {
						fix_indexed (property, TRUE, &n_error);
					}
				}
			}

			if (n_error) {
				g_propagate_error (error, n_error);
				return;
			}

			if (update_property_value (ontology_path,
			                           "rdfs:range", subject, RDFS_PREFIX "range",
			                           tracker_class_get_uri (tracker_property_get_range (property)),
			                           allowed_range_conversions,
			                           NULL, property, &n_error)) {
				TrackerClass *class;

				class = tracker_property_get_domain (property);
				tracker_class_set_db_schema_changed (class, TRUE);
				tracker_property_set_db_schema_changed (property, TRUE);
			}

			if (n_error) {
				g_propagate_error (error, n_error);
				return;
			}

			if (update_property_value (ontology_path,
			                           "tracker:defaultValue", subject, TRACKER_PREFIX "defaultValue",
			                           tracker_property_get_default_value (property),
			                           NULL, NULL, property, &n_error)) {
				TrackerClass *class;

				class = tracker_property_get_domain (property);
				tracker_class_set_db_schema_changed (class, TRUE);
				tracker_property_set_db_schema_changed (property, TRUE);
			}

			if (n_error) {
				g_propagate_error (error, n_error);
			}
		}
	}
}

static void
tracker_data_ontology_process_changes_post_import (GPtrArray *seen_classes,
                                                   GPtrArray *seen_properties)
{
	return;
}

static void
tracker_data_ontology_free_seen (GPtrArray *seen)
{
	if (seen) {
		g_ptr_array_foreach (seen, (GFunc) g_object_unref, NULL);
		g_ptr_array_free (seen, TRUE);
	}
}

static void
load_ontology_file_from_path (const gchar        *ontology_path,
                              gint               *max_id,
                              gboolean            in_update,
                              GPtrArray          *seen_classes,
                              GPtrArray          *seen_properties,
                              GHashTable         *uri_id_map,
                              GError            **error)
{
	TrackerTurtleReader *reader;
	GError              *ttl_error = NULL;

	reader = tracker_turtle_reader_new (ontology_path, &ttl_error);

	if (ttl_error) {
		g_propagate_error (error, ttl_error);
		return;
	}

	/* Post checks are only needed for ontology updates, not the initial
	 * ontology */

	while (ttl_error == NULL && tracker_turtle_reader_next (reader, &ttl_error)) {
		const gchar *subject, *predicate, *object;
		gint subject_id = 0;
		GError *ontology_error = NULL;

		subject = tracker_turtle_reader_get_subject (reader);
		predicate = tracker_turtle_reader_get_predicate (reader);
		object = tracker_turtle_reader_get_object (reader);

		if (uri_id_map) {
			subject_id = GPOINTER_TO_INT (g_hash_table_lookup (uri_id_map, subject));
		}

		tracker_data_ontology_load_statement (ontology_path, subject_id, subject, predicate, object,
		                                      max_id, in_update, NULL, NULL,
		                                      seen_classes, seen_properties, &ontology_error);

		if (ontology_error) {
			g_propagate_error (error, ontology_error);
			break;
		}
	}

	g_object_unref (reader);

	if (ttl_error) {
		g_propagate_error (error, ttl_error);
	}
}


static TrackerOntology*
get_ontology_from_path (const gchar *ontology_path)
{
	TrackerTurtleReader *reader;
	GError *error = NULL;
	GHashTable *ontology_uris;
	TrackerOntology *ret = NULL;

	reader = tracker_turtle_reader_new (ontology_path, &error);

	if (error) {
		g_critical ("Turtle parse error: %s", error->message);
		g_error_free (error);
		return NULL;
	}

	ontology_uris = g_hash_table_new_full (g_str_hash,
	                                       g_str_equal,
	                                       g_free,
	                                       g_object_unref);

	while (error == NULL && tracker_turtle_reader_next (reader, &error)) {
		const gchar *subject, *predicate, *object;

		subject = tracker_turtle_reader_get_subject (reader);
		predicate = tracker_turtle_reader_get_predicate (reader);
		object = tracker_turtle_reader_get_object (reader);

		if (g_strcmp0 (predicate, RDF_TYPE) == 0) {
			if (g_strcmp0 (object, TRACKER_PREFIX "Ontology") == 0) {
				TrackerOntology *ontology;

				ontology = tracker_ontology_new ();
				tracker_ontology_set_uri (ontology, subject);

				/* Passes ownership */
				g_hash_table_insert (ontology_uris,
				                     g_strdup (subject),
				                     ontology);
			}
		} else if (g_strcmp0 (predicate, NAO_LAST_MODIFIED) == 0) {
			TrackerOntology *ontology;

			ontology = g_hash_table_lookup (ontology_uris, subject);
			if (ontology == NULL) {
				g_critical ("%s: Unknown ontology %s", ontology_path, subject);
				return NULL;
			}

			tracker_ontology_set_last_modified (ontology, tracker_string_to_date (object, NULL, NULL));

			/* This one is here because lower ontology_uris is destroyed, and
			 * else would this one's reference also be destroyed with it */
			ret = g_object_ref (ontology);

			break;
		}
	}

	g_hash_table_unref (ontology_uris);
	g_object_unref (reader);

	if (error) {
		g_critical ("Turtle parse error: %s", error->message);
		g_error_free (error);
	}

	if (ret == NULL) {
		g_critical ("Ontology file has no nao:lastModified header: %s", ontology_path);
	}

	return ret;
}

#ifndef DISABLE_JOURNAL
static void
load_ontology_ids_from_journal (GHashTable **uri_id_map_out,
                                gint        *max_id)
{
	GHashTable *uri_id_map;

	uri_id_map = g_hash_table_new_full (g_str_hash, g_str_equal,
	                                    g_free, NULL);

	while (tracker_db_journal_reader_next (NULL)) {
		TrackerDBJournalEntryType type;

		type = tracker_db_journal_reader_get_type ();
		if (type == TRACKER_DB_JOURNAL_RESOURCE) {
			gint id;
			const gchar *uri;

			tracker_db_journal_reader_get_resource (&id, &uri);
			g_hash_table_insert (uri_id_map, g_strdup (uri), GINT_TO_POINTER (id));
			if (id > *max_id) {
				*max_id = id;
			}
		}
	}

	*uri_id_map_out = uri_id_map;
}
#endif /* DISABLE_JOURNAL */

static void
tracker_data_ontology_process_statement (const gchar *graph,
                                         const gchar *subject,
                                         const gchar *predicate,
                                         const gchar *object,
                                         gboolean     is_uri,
                                         gboolean     in_update,
                                         gboolean     ignore_nao_last_modified)
{
	GError *error = NULL;

	if (g_strcmp0 (predicate, RDF_TYPE) == 0) {
		if (g_strcmp0 (object, RDFS_CLASS) == 0) {
			TrackerClass *class;

			class = tracker_ontologies_get_class_by_uri (subject);

			if (class && tracker_class_get_is_new (class) != in_update) {
				return;
			}
		} else if (g_strcmp0 (object, RDF_PROPERTY) == 0) {
			TrackerProperty *prop;

			prop = tracker_ontologies_get_property_by_uri (subject);

			if (prop && tracker_property_get_is_new (prop) != in_update) {
				return;
			}
		} else if (g_strcmp0 (object, TRACKER_PREFIX "Namespace") == 0) {
			TrackerNamespace *namespace;

			namespace = tracker_ontologies_get_namespace_by_uri (subject);

			if (namespace && tracker_namespace_get_is_new (namespace) != in_update) {
				return;
			}
		} else if (g_strcmp0 (object, TRACKER_PREFIX "Ontology") == 0) {
			TrackerOntology *ontology;

			ontology = tracker_ontologies_get_ontology_by_uri (subject);

			if (ontology && tracker_ontology_get_is_new (ontology) != in_update) {
				return;
			}
		}
	} else if (g_strcmp0 (predicate, RDFS_SUB_CLASS_OF) == 0) {
		TrackerClass *class;

		class = tracker_ontologies_get_class_by_uri (subject);

		if (class && tracker_class_get_is_new (class) != in_update) {
			return;
		}
	} else if (g_strcmp0 (predicate, RDFS_SUB_PROPERTY_OF) == 0          ||
	           g_strcmp0 (predicate, RDFS_DOMAIN) == 0                   ||
	           g_strcmp0 (predicate, RDFS_RANGE) == 0                    ||
	           g_strcmp0 (predicate, NRL_MAX_CARDINALITY) == 0           ||
	           g_strcmp0 (predicate, TRACKER_PREFIX "indexed") == 0      ||
	           g_strcmp0 (predicate, TRACKER_PREFIX "transient") == 0    ||
	           g_strcmp0 (predicate, TRACKER_PREFIX "fulltextIndexed") == 0) {
		TrackerProperty *prop;

		prop = tracker_ontologies_get_property_by_uri (subject);

		if (prop && tracker_property_get_is_new (prop) != in_update) {
			return;
		}
	} else if (g_strcmp0 (predicate, TRACKER_PREFIX "prefix") == 0) {
		TrackerNamespace *namespace;

		namespace = tracker_ontologies_get_namespace_by_uri (subject);

		if (namespace && tracker_namespace_get_is_new (namespace) != in_update) {
			return;
		}
	} else if (g_strcmp0 (predicate, NAO_LAST_MODIFIED) == 0) {
		TrackerOntology *ontology;

		ontology = tracker_ontologies_get_ontology_by_uri (subject);

		if (ontology && tracker_ontology_get_is_new (ontology) != in_update) {
			return;
		}

		if (ignore_nao_last_modified) {
			return;
		}
	}

	if (is_uri) {
		tracker_data_insert_statement_with_uri (graph, subject,
		                                        predicate, object,
		                                        &error);

		if (error != NULL) {
			g_critical ("%s", error->message);
			g_error_free (error);
			return;
		}

	} else {
		tracker_data_insert_statement_with_string (graph, subject,
		                                           predicate, object,
		                                           &error);

		if (error != NULL) {
			g_critical ("%s", error->message);
			g_error_free (error);
			return;
		}
	}
}

static void
import_ontology_path (const gchar *ontology_path,
                      gboolean in_update,
                      gboolean ignore_nao_last_modified)
{
	GError          *error = NULL;

	TrackerTurtleReader* reader;

	reader = tracker_turtle_reader_new (ontology_path, &error);

	if (error != NULL) {
		g_critical ("%s", error->message);
		g_error_free (error);
		return;
	}

	while (tracker_turtle_reader_next (reader, &error)) {

		const gchar *graph = tracker_turtle_reader_get_graph (reader);
		const gchar *subject = tracker_turtle_reader_get_subject (reader);
		const gchar *predicate = tracker_turtle_reader_get_predicate (reader);
		const gchar *object  = tracker_turtle_reader_get_object (reader);

		tracker_data_ontology_process_statement (graph, subject, predicate, object,
		                                         tracker_turtle_reader_get_object_is_uri (reader),
		                                         in_update, ignore_nao_last_modified);

	}

	g_object_unref (reader);

	if (error) {
		g_critical ("%s", error->message);
		g_error_free (error);
	}
}

static void
class_add_super_classes_from_db (TrackerDBInterface *iface,
                                 TrackerClass       *class)
{
	TrackerDBStatement *stmt;
	TrackerDBCursor *cursor;
	GError *error = NULL;

	stmt = tracker_db_interface_create_statement (iface, TRACKER_DB_STATEMENT_CACHE_TYPE_SELECT, &error,
	                                              "SELECT (SELECT Uri FROM Resource WHERE ID = \"rdfs:subClassOf\") "
	                                              "FROM \"rdfs:Class_rdfs:subClassOf\" "
	                                              "WHERE ID = (SELECT ID FROM Resource WHERE Uri = ?)");

	if (!stmt) {
		g_critical ("%s", error->message);
		g_error_free (error);
		return;
	}

	tracker_db_statement_bind_text (stmt, 0, tracker_class_get_uri (class));
	cursor = tracker_db_statement_start_cursor (stmt, NULL);
	g_object_unref (stmt);

	if (cursor) {
		while (tracker_db_cursor_iter_next (cursor, NULL, NULL)) {
			TrackerClass *super_class;
			const gchar *super_class_uri;

			super_class_uri = tracker_db_cursor_get_string (cursor, 0, NULL);
			super_class = tracker_ontologies_get_class_by_uri (super_class_uri);
			tracker_class_add_super_class (class, super_class);
		}

		g_object_unref (cursor);
	}
}


static void
class_add_domain_indexes_from_db (TrackerDBInterface *iface,
                                  TrackerClass       *class)
{
	TrackerDBStatement *stmt;
	TrackerDBCursor *cursor;
	GError *error = NULL;

	stmt = tracker_db_interface_create_statement (iface, TRACKER_DB_STATEMENT_CACHE_TYPE_SELECT, &error,
	                                              "SELECT (SELECT Uri FROM Resource WHERE ID = \"tracker:domainIndex\") "
	                                              "FROM \"rdfs:Class_tracker:domainIndex\" "
	                                              "WHERE ID = (SELECT ID FROM Resource WHERE Uri = ?)");

	if (!stmt) {
		g_critical ("%s", error->message);
		g_error_free (error);
		return;
	}

	tracker_db_statement_bind_text (stmt, 0, tracker_class_get_uri (class));
	cursor = tracker_db_statement_start_cursor (stmt, NULL);
	g_object_unref (stmt);

	if (cursor) {
		while (tracker_db_cursor_iter_next (cursor, NULL, NULL)) {
			TrackerProperty *domain_index;
			const gchar *domain_index_uri;

			domain_index_uri = tracker_db_cursor_get_string (cursor, 0, NULL);
			domain_index = tracker_ontologies_get_property_by_uri (domain_index_uri);
			tracker_class_add_domain_index (class, domain_index);
			tracker_property_add_domain_index (domain_index, class);
		}

		g_object_unref (cursor);
	}
}

static void
property_add_super_properties_from_db (TrackerDBInterface *iface,
                                       TrackerProperty *property)
{
	TrackerDBStatement *stmt;
	TrackerDBCursor *cursor;
	GError *error = NULL;

	stmt = tracker_db_interface_create_statement (iface, TRACKER_DB_STATEMENT_CACHE_TYPE_SELECT, &error,
	                                              "SELECT (SELECT Uri FROM Resource WHERE ID = \"rdfs:subPropertyOf\") "
	                                              "FROM \"rdf:Property_rdfs:subPropertyOf\" "
	                                              "WHERE ID = (SELECT ID FROM Resource WHERE Uri = ?)");

	if (!stmt) {
		g_critical ("%s", error->message);
		g_error_free (error);
		return;
	}

	tracker_db_statement_bind_text (stmt, 0, tracker_property_get_uri (property));
	cursor = tracker_db_statement_start_cursor (stmt, NULL);
	g_object_unref (stmt);

	if (cursor) {
		while (tracker_db_cursor_iter_next (cursor, NULL, NULL)) {
			TrackerProperty *super_property;
			const gchar *super_property_uri;

			super_property_uri = tracker_db_cursor_get_string (cursor, 0, NULL);
			super_property = tracker_ontologies_get_property_by_uri (super_property_uri);
			tracker_property_add_super_property (property, super_property);
		}

		g_object_unref (cursor);
	}
}

static void
db_get_static_data (TrackerDBInterface  *iface,
                    GError             **error)
{
	TrackerDBStatement *stmt;
	TrackerDBCursor *cursor = NULL;
	TrackerClass **classes;
	guint n_classes, i;
	GError *internal_error = NULL;

	stmt = tracker_db_interface_create_statement (iface, TRACKER_DB_STATEMENT_CACHE_TYPE_SELECT, &internal_error,
	                                              "SELECT (SELECT Uri FROM Resource WHERE ID = \"tracker:Ontology\".ID), "
	                                              "\"nao:lastModified\" "
	                                              "FROM \"tracker:Ontology\"");

	if (stmt) {
		cursor = tracker_db_statement_start_cursor (stmt, &internal_error);
		g_object_unref (stmt);
	}

	if (cursor) {
		while (tracker_db_cursor_iter_next (cursor, NULL, &internal_error)) {
			TrackerOntology *ontology;
			const gchar     *uri;
			time_t           last_mod;

			ontology = tracker_ontology_new ();

			uri = tracker_db_cursor_get_string (cursor, 0, NULL);
			last_mod = (time_t) tracker_db_cursor_get_int (cursor, 1);

			tracker_ontology_set_is_new (ontology, FALSE);
			tracker_ontology_set_uri (ontology, uri);
			tracker_ontology_set_last_modified (ontology, last_mod);
			tracker_ontologies_add_ontology (ontology);

			g_object_unref (ontology);
		}

		g_object_unref (cursor);
		cursor = NULL;
	}

	if (internal_error) {
		g_propagate_error (error, internal_error);
		return;
	}

	stmt = tracker_db_interface_create_statement (iface, TRACKER_DB_STATEMENT_CACHE_TYPE_SELECT, &internal_error,
	                                              "SELECT (SELECT Uri FROM Resource WHERE ID = \"tracker:Namespace\".ID), "
	                                              "\"tracker:prefix\" "
	                                              "FROM \"tracker:Namespace\"");

	if (stmt) {
		cursor = tracker_db_statement_start_cursor (stmt, &internal_error);
		g_object_unref (stmt);
	}

	if (cursor) {
		while (tracker_db_cursor_iter_next (cursor, NULL, &internal_error)) {
			TrackerNamespace *namespace;
			const gchar      *uri, *prefix;

			namespace = tracker_namespace_new (FALSE);

			uri = tracker_db_cursor_get_string (cursor, 0, NULL);
			prefix = tracker_db_cursor_get_string (cursor, 1, NULL);

			tracker_namespace_set_is_new (namespace, FALSE);
			tracker_namespace_set_uri (namespace, uri);
			tracker_namespace_set_prefix (namespace, prefix);
			tracker_ontologies_add_namespace (namespace);

			g_object_unref (namespace);

		}

		g_object_unref (cursor);
		cursor = NULL;
	}

	if (internal_error) {
		g_propagate_error (error, internal_error);
		return;
	}

	stmt = tracker_db_interface_create_statement (iface, TRACKER_DB_STATEMENT_CACHE_TYPE_SELECT, &internal_error,
	                                              "SELECT \"rdfs:Class\".ID, "
	                                              "(SELECT Uri FROM Resource WHERE ID = \"rdfs:Class\".ID), "
	                                              "\"tracker:notify\" "
	                                              "FROM \"rdfs:Class\" ORDER BY ID");

	if (stmt) {
		cursor = tracker_db_statement_start_cursor (stmt, &internal_error);
		g_object_unref (stmt);
	}

	if (cursor) {
		while (tracker_db_cursor_iter_next (cursor, NULL, &internal_error)) {
			TrackerClass *class;
			const gchar  *uri;
			gint          id;
			GValue        value = { 0 };
			gboolean      notify;

			class = tracker_class_new (FALSE);

			id = tracker_db_cursor_get_int (cursor, 0);
			uri = tracker_db_cursor_get_string (cursor, 1, NULL);

			tracker_db_cursor_get_value (cursor, 2, &value);

			if (G_VALUE_TYPE (&value) != 0) {
				notify = (g_value_get_int64 (&value) == 1);
				g_value_unset (&value);
			} else {
				/* NULL */
				notify = FALSE;
			}

			tracker_class_set_db_schema_changed (class, FALSE);
			tracker_class_set_is_new (class, FALSE);
			tracker_class_set_uri (class, uri);
			tracker_class_set_notify (class, notify);

			class_add_super_classes_from_db (iface, class);

			/* We do this later, we first need to load the properties too
			   class_add_domain_indexes_from_db (iface, class); */

			tracker_ontologies_add_class (class);
			tracker_ontologies_add_id_uri_pair (id, uri);
			tracker_class_set_id (class, id);

			g_object_unref (class);
		}

		g_object_unref (cursor);
		cursor = NULL;
	}

	if (internal_error) {
		g_propagate_error (error, internal_error);
		return;
	}

	stmt = tracker_db_interface_create_statement (iface, TRACKER_DB_STATEMENT_CACHE_TYPE_SELECT, &internal_error,
	                                              "SELECT \"rdf:Property\".ID, (SELECT Uri FROM Resource WHERE ID = \"rdf:Property\".ID), "
	                                              "(SELECT Uri FROM Resource WHERE ID = \"rdfs:domain\"), "
	                                              "(SELECT Uri FROM Resource WHERE ID = \"rdfs:range\"), "
	                                              "\"nrl:maxCardinality\", "
	                                              "\"tracker:indexed\", "
	                                              "(SELECT Uri FROM Resource WHERE ID = \"tracker:secondaryIndex\"), "
	                                              "\"tracker:fulltextIndexed\", "
	                                              "\"tracker:transient\", "
	                                              "\"tracker:writeback\", "
	                                              "(SELECT 1 FROM \"rdfs:Resource_rdf:type\" WHERE ID = \"rdf:Property\".ID AND "
	                                              "\"rdf:type\" = (SELECT ID FROM Resource WHERE Uri = '" NRL_INVERSE_FUNCTIONAL_PROPERTY "')), "
	                                              "\"tracker:forceJournal\", "
	                                              "\"tracker:defaultValue\" "
	                                              "FROM \"rdf:Property\" ORDER BY ID");

	if (stmt) {
		cursor = tracker_db_statement_start_cursor (stmt, &internal_error);
		g_object_unref (stmt);
	}

	if (cursor) {
		while (tracker_db_cursor_iter_next (cursor, NULL, &internal_error)) {
			GValue value = { 0 };
			TrackerProperty *property;
			const gchar     *uri, *domain_uri, *range_uri, *secondary_index_uri, *default_value;
			gboolean         multi_valued, indexed, fulltext_indexed;
			gboolean         transient, is_inverse_functional_property;
			gboolean         writeback, force_journal;
			gint             id;

			property = tracker_property_new (FALSE);

			id = tracker_db_cursor_get_int (cursor, 0);
			uri = tracker_db_cursor_get_string (cursor, 1, NULL);
			domain_uri = tracker_db_cursor_get_string (cursor, 2, NULL);
			range_uri = tracker_db_cursor_get_string (cursor, 3, NULL);

			tracker_db_cursor_get_value (cursor, 4, &value);

			if (G_VALUE_TYPE (&value) != 0) {
				multi_valued = (g_value_get_int64 (&value) > 1);
				g_value_unset (&value);
			} else {
				/* nrl:maxCardinality not set
				   not limited to single value */
				multi_valued = TRUE;
			}

			tracker_db_cursor_get_value (cursor, 5, &value);

			if (G_VALUE_TYPE (&value) != 0) {
				indexed = (g_value_get_int64 (&value) == 1);
				g_value_unset (&value);
			} else {
				/* NULL */
				indexed = FALSE;
			}

			secondary_index_uri = tracker_db_cursor_get_string (cursor, 6, NULL);

			tracker_db_cursor_get_value (cursor, 7, &value);

			if (G_VALUE_TYPE (&value) != 0) {
				fulltext_indexed = (g_value_get_int64 (&value) == 1);
				g_value_unset (&value);
			} else {
				/* NULL */
				fulltext_indexed = FALSE;
			}

			tracker_db_cursor_get_value (cursor, 8, &value);

			if (G_VALUE_TYPE (&value) != 0) {
				transient = (g_value_get_int64 (&value) == 1);
				g_value_unset (&value);
			} else {
				/* NULL */
				transient = FALSE;
			}

			/* tracker:writeback column */
			tracker_db_cursor_get_value (cursor, 9, &value);

			if (G_VALUE_TYPE (&value) != 0) {
				writeback = (g_value_get_int64 (&value) == 1);
				g_value_unset (&value);
			} else {
				/* NULL */
				writeback = FALSE;
			}

			/* NRL_INVERSE_FUNCTIONAL_PROPERTY column */
			tracker_db_cursor_get_value (cursor, 10, &value);

			if (G_VALUE_TYPE (&value) != 0) {
				is_inverse_functional_property = TRUE;
				g_value_unset (&value);
			} else {
				/* NULL */
				is_inverse_functional_property = FALSE;
			}

			/* tracker:forceJournal column */
			tracker_db_cursor_get_value (cursor, 11, &value);

			if (G_VALUE_TYPE (&value) != 0) {
				force_journal = (g_value_get_int64 (&value) == 1);
				g_value_unset (&value);
			} else {
				/* NULL */
				force_journal = TRUE;
			}

			default_value = tracker_db_cursor_get_string (cursor, 12, NULL);

			tracker_property_set_is_new_domain_index (property, tracker_ontologies_get_class_by_uri (domain_uri), FALSE);
			tracker_property_set_is_new (property, FALSE);
			tracker_property_set_transient (property, transient);
			tracker_property_set_uri (property, uri);
			tracker_property_set_id (property, id);
			tracker_property_set_domain (property, tracker_ontologies_get_class_by_uri (domain_uri));
			tracker_property_set_range (property, tracker_ontologies_get_class_by_uri (range_uri));
			tracker_property_set_multiple_values (property, multi_valued);
			tracker_property_set_indexed (property, indexed);
			tracker_property_set_default_value (property, default_value);
			tracker_property_set_force_journal (property, force_journal);

			tracker_property_set_db_schema_changed (property, FALSE);
			tracker_property_set_writeback (property, writeback);

			if (secondary_index_uri) {
				tracker_property_set_secondary_index (property, tracker_ontologies_get_property_by_uri (secondary_index_uri));
			}

			tracker_property_set_fulltext_indexed (property, fulltext_indexed);
			tracker_property_set_is_inverse_functional_property (property, is_inverse_functional_property);

			/* super properties are only used in updates, never for queries */
			if ((tracker_db_manager_get_flags (NULL, NULL) & TRACKER_DB_MANAGER_READONLY) == 0) {
				property_add_super_properties_from_db (iface, property);
			}

			tracker_ontologies_add_property (property);
			tracker_ontologies_add_id_uri_pair (id, uri);

			g_object_unref (property);

		}

		g_object_unref (cursor);
		cursor = NULL;
	}

	/* Now that the properties are loaded we can do this foreach class */
	classes = tracker_ontologies_get_classes (&n_classes);
	for (i = 0; i < n_classes; i++) {
		class_add_domain_indexes_from_db (iface, classes[i]);
	}

	if (internal_error) {
		g_propagate_error (error, internal_error);
		return;
	}
}

static void
insert_uri_in_resource_table (TrackerDBInterface  *iface,
                              const gchar         *uri,
                              gint                 id,
                              GError             **error)
{
	TrackerDBStatement *stmt;
	GError *internal_error = NULL;

	stmt = tracker_db_interface_create_statement (iface, TRACKER_DB_STATEMENT_CACHE_TYPE_UPDATE,
	                                              &internal_error,
	                                              "INSERT OR IGNORE "
	                                              "INTO Resource "
	                                              "(ID, Uri) "
	                                              "VALUES (?, ?)");
	if (internal_error) {
		g_propagate_error (error, internal_error);
		return;
	}

	tracker_db_statement_bind_int (stmt, 0, id);
	tracker_db_statement_bind_text (stmt, 1, uri);
	tracker_db_statement_execute (stmt, &internal_error);

	if (internal_error) {
		g_object_unref (stmt);
		g_propagate_error (error, internal_error);
		return;
	}

#ifndef DISABLE_JOURNAL
	if (!in_journal_replay) {
		tracker_db_journal_append_resource (id, uri);
	}
#endif /* DISABLE_JOURNAL */

	g_object_unref (stmt);

}

static void
range_change_for (TrackerProperty *property,
                  GString         *in_col_sql,
                  GString         *sel_col_sql,
                  const gchar     *field_name)
{
	/* TODO: TYPE_RESOURCE and TYPE_DATETIME are completely unhandled atm, we
	 * should forbid conversion from anything to resource or datetime in error
	 * handling earlier */

	g_string_append_printf (in_col_sql, ", \"%s\", \"%s:graph\"",
	                        field_name, field_name);

	if (tracker_property_get_data_type (property) == TRACKER_PROPERTY_TYPE_INTEGER ||
	    tracker_property_get_data_type (property) == TRACKER_PROPERTY_TYPE_DOUBLE) {
			g_string_append_printf (sel_col_sql, ", \"%s\" + 0, \"%s:graph\"",
			                        field_name, field_name);
	} else if (tracker_property_get_data_type (property) == TRACKER_PROPERTY_TYPE_DATETIME) {

		/* TODO (see above) */

		g_string_append_printf (sel_col_sql, ", \"%s\", \"%s:graph\"",
		                        field_name, field_name);

		g_string_append_printf (in_col_sql, ", \"%s:localDate\", \"%s:localTime\"",
		                        tracker_property_get_name (property),
		                        tracker_property_get_name (property));

		g_string_append_printf (sel_col_sql, ", \"%s:localDate\", \"%s:localTime\"",
		                        tracker_property_get_name (property),
		                        tracker_property_get_name (property));

	} else if (tracker_property_get_data_type (property) == TRACKER_PROPERTY_TYPE_BOOLEAN) {
		g_string_append_printf (sel_col_sql, ", \"%s\" != 0, \"%s:graph\"",
		                        field_name, field_name);
	} else {
		g_string_append_printf (sel_col_sql, ", \"%s\", \"%s:graph\"",
		                        field_name, field_name);
	}
}

static void
create_decomposed_metadata_property_table (TrackerDBInterface *iface,
                                           TrackerProperty    *property,
                                           const gchar        *service_name,
                                           TrackerClass       *service,
                                           const gchar       **sql_type_for_single_value,
                                           gboolean            in_update,
                                           gboolean            in_change,
                                           GError            **error)
{
	GError *internal_error = NULL;
	const char *field_name;
	const char *sql_type;
	gboolean    not_single;

	field_name = tracker_property_get_name (property);

	not_single = !sql_type_for_single_value;

	switch (tracker_property_get_data_type (property)) {
	case TRACKER_PROPERTY_TYPE_STRING:
		sql_type = "TEXT";
		break;
	case TRACKER_PROPERTY_TYPE_INTEGER:
	case TRACKER_PROPERTY_TYPE_BOOLEAN:
	case TRACKER_PROPERTY_TYPE_DATE:
	case TRACKER_PROPERTY_TYPE_DATETIME:
	case TRACKER_PROPERTY_TYPE_RESOURCE:
		sql_type = "INTEGER";
		break;
	case TRACKER_PROPERTY_TYPE_DOUBLE:
		sql_type = "REAL";
		break;
	default:
		sql_type = "";
		break;
	}

	if (!in_update || (in_update && (tracker_property_get_is_new (property) ||
	                                 tracker_property_get_is_new_domain_index (property, service) ||
	                                 tracker_property_get_db_schema_changed (property)))) {
		if (not_single || tracker_property_get_multiple_values (property)) {
			GString *sql = NULL;
			GString *in_col_sql = NULL;
			GString *sel_col_sql = NULL;

			/* multiple values */

			if (in_update) {
				g_debug ("Altering database for class '%s' property '%s': multi value",
				         service_name, field_name);
			}

			if (in_change && !tracker_property_get_is_new (property)) {
				g_debug ("Drop index: DROP INDEX IF EXISTS \"%s_%s_ID\"\nRename: ALTER TABLE \"%s_%s\" RENAME TO \"%s_%s_TEMP\"",
				         service_name, field_name, service_name, field_name,
				         service_name, field_name);

				tracker_db_interface_execute_query (iface, &internal_error,
				                                    "DROP INDEX IF EXISTS \"%s_%s_ID\"",
				                                    service_name,
				                                    field_name);

				if (internal_error) {
					g_propagate_error (error, internal_error);
					goto error_out;
				}

				tracker_db_interface_execute_query (iface, &internal_error,
				                                    "ALTER TABLE \"%s_%s\" RENAME TO \"%s_%s_TEMP\"",
				                                    service_name, field_name, service_name, field_name);

				if (internal_error) {
					g_propagate_error (error, internal_error);
					goto error_out;
				}
			}

			sql = g_string_new ("");
			g_string_append_printf (sql, "CREATE TABLE \"%s_%s\" ("
			                             "ID INTEGER NOT NULL, "
			                             "\"%s\" %s NOT NULL, "
			                             "\"%s:graph\" INTEGER",
			                             service_name,
			                             field_name,
			                             field_name,
			                             sql_type,
			                             field_name);

			if (in_change && !tracker_property_get_is_new (property)) {
				in_col_sql = g_string_new ("ID");
				sel_col_sql = g_string_new ("ID");

				range_change_for (property, in_col_sql, sel_col_sql, field_name);
			}

			if (tracker_property_get_data_type (property) == TRACKER_PROPERTY_TYPE_DATETIME) {
				/* xsd:dateTime is stored in three columns:
				 * universal time, local date, local time of day */
				g_string_append_printf (sql,
				                        ", \"%s:localDate\" INTEGER NOT NULL"
				                        ", \"%s:localTime\" INTEGER NOT NULL",
				                        tracker_property_get_name (property),
				                        tracker_property_get_name (property));
			}

			tracker_db_interface_execute_query (iface, &internal_error,
			                                    "%s)", sql->str);

			if (internal_error) {
				g_propagate_error (error, internal_error);
				goto error_out;
			}

			/* multiple values */
			if (tracker_property_get_indexed (property)) {
				/* use different UNIQUE index for properties whose
				 * value should be indexed to minimize index size */
				set_index_for_multi_value_property (iface, service_name, field_name, TRUE, TRUE,
				                                    &internal_error);
				if (internal_error) {
					g_propagate_error (error, internal_error);
					goto error_out;
				}
			} else {
				set_index_for_multi_value_property (iface, service_name, field_name, FALSE, TRUE,
				                                    &internal_error);
				/* we still have to include the property value in
				 * the unique index for proper constraints */
				if (internal_error) {
					g_propagate_error (error, internal_error);
					goto error_out;
				}
			}

			if (in_change && !tracker_property_get_is_new (property) && in_col_sql && sel_col_sql) {
				gchar *query;

				query = g_strdup_printf ("INSERT INTO \"%s_%s\"(%s) "
				                         "SELECT %s FROM \"%s_%s_TEMP\"",
				                         service_name, field_name, in_col_sql->str,
				                         sel_col_sql->str, service_name, field_name);

				tracker_db_interface_execute_query (iface, &internal_error, "%s", query);

				if (internal_error) {
					g_free (query);
					g_propagate_error (error, internal_error);
					goto error_out;
				}

				g_free (query);
				tracker_db_interface_execute_query (iface, &internal_error, "DROP TABLE \"%s_%s_TEMP\"",
				                                    service_name, field_name);

				if (internal_error) {
					g_propagate_error (error, internal_error);
					goto error_out;
				}
			}

			/* multiple values */
			if (tracker_property_get_indexed (property)) {
				/* use different UNIQUE index for properties whose
				 * value should be indexed to minimize index size */
				set_index_for_multi_value_property (iface, service_name, field_name, TRUE, TRUE,
				                                    &internal_error);
				if (internal_error) {
					g_propagate_error (error, internal_error);
					goto error_out;
				}
			} else {
				set_index_for_multi_value_property (iface, service_name, field_name, FALSE, TRUE,
				                                    &internal_error);
				if (internal_error) {
					g_propagate_error (error, internal_error);
					goto error_out;
				}
				/* we still have to include the property value in
				 * the unique index for proper constraints */
			}

			error_out:

			if (sql) {
				g_string_free (sql, TRUE);
			}

			if (sel_col_sql) {
				g_string_free (sel_col_sql, TRUE);
			}

			if (in_col_sql) {
				g_string_free (in_col_sql, TRUE);
			}
		} else if (sql_type_for_single_value) {
			*sql_type_for_single_value = sql_type;
		}
	}
}

static gboolean
is_a_domain_index (TrackerProperty **domain_indexes, TrackerProperty *property)
{
	while (*domain_indexes) {

		if (*domain_indexes == property) {
			return TRUE;
		}

		domain_indexes++;
	}

	return FALSE;
}

static void
copy_from_domain_to_domain_index (TrackerDBInterface  *iface,
                                  TrackerProperty     *domain_index,
                                  const gchar         *column_name,
                                  const gchar         *column_suffix,
                                  TrackerClass        *dest_domain,
                                  GError             **error)
{
	GError *internal_error = NULL;
	TrackerClass *source_domain;
	const gchar *source_name, *dest_name;
	gchar *query;

	source_domain = tracker_property_get_domain (domain_index);
	source_name = tracker_class_get_name (source_domain);
	dest_name = tracker_class_get_name (dest_domain);

	query = g_strdup_printf ("UPDATE \"%s\" SET \"%s%s\"=("
	                         "SELECT \"%s%s\" FROM \"%s\" "
	                         "WHERE \"%s\".ID = \"%s\".ID)",
	                         dest_name,
	                         column_name,
	                         column_suffix ? column_suffix : "",
	                         column_name,
	                         column_suffix ? column_suffix : "",
	                         source_name,
	                         source_name,
	                         dest_name);

	g_debug ("Copying: '%s'", query);

	tracker_db_interface_execute_query (iface, &internal_error, "%s", query);

	if (internal_error) {
		g_propagate_error (error, internal_error);
	}

	g_free (query);
}

typedef struct {
	TrackerProperty *prop;
	const gchar *field_name;
	const gchar *suffix;
} ScheduleCopy;

static void
schedule_copy (GPtrArray *schedule,
               TrackerProperty *prop,
               const gchar *field_name,
               const gchar *suffix)
{
	ScheduleCopy *sched = g_new0 (ScheduleCopy, 1);
	sched->prop = prop;
	sched->field_name = field_name,
	sched->suffix = suffix;
	g_ptr_array_add (schedule, sched);
}

static void
create_decomposed_metadata_tables (TrackerDBInterface  *iface,
                                   TrackerClass        *service,
                                   gboolean             in_update,
                                   gboolean             in_change,
                                   GError             **error)
{
	const char       *service_name;
	GString          *create_sql = NULL;
	GString          *in_col_sql = NULL;
	GString          *sel_col_sql = NULL;
	TrackerProperty **properties, *property, **domain_indexes;
	GSList           *class_properties = NULL, *field_it;
	gboolean          main_class;
	gint              i, n_props;
	gboolean          in_alter = in_update;
	GError           *internal_error = NULL;
	GPtrArray        *copy_schedule = NULL;

	g_return_if_fail (TRACKER_IS_CLASS (service));

	service_name = tracker_class_get_name (service);

	g_return_if_fail (service_name != NULL);

	main_class = (strcmp (service_name, "rdfs:Resource") == 0);

	if (g_str_has_prefix (service_name, "xsd:")) {
		/* xsd classes do not derive from rdfs:Resource and do not need separate tables */
		return;
	}

	if (in_change) {
		g_debug ("Rename: ALTER TABLE \"%s\" RENAME TO \"%s_TEMP\"", service_name, service_name);
		tracker_db_interface_execute_query (iface, &internal_error,
		                                    "ALTER TABLE \"%s\" RENAME TO \"%s_TEMP\"",
		                                    service_name, service_name);
		in_col_sql = g_string_new ("ID");
		sel_col_sql = g_string_new ("ID");
		if (internal_error) {
			g_propagate_error (error, internal_error);
			goto error_out;
		}
	}

	if (in_change || !in_update || (in_update && tracker_class_get_is_new (service))) {
		if (in_update)
			g_debug ("Altering database with new class '%s' (create)", service_name);
		in_alter = FALSE;
		create_sql = g_string_new ("");
		g_string_append_printf (create_sql, "CREATE TABLE \"%s\" (ID INTEGER NOT NULL PRIMARY KEY", service_name);
		if (main_class) {
			tracker_db_interface_execute_query (iface, &internal_error, "CREATE TABLE Resource (ID INTEGER NOT NULL PRIMARY KEY, Uri TEXT NOT NULL, UNIQUE (Uri))");
			if (internal_error) {
				g_propagate_error (error, internal_error);
				goto error_out;
			}
			g_string_append (create_sql, ", Available INTEGER NOT NULL");
		}
	}

	properties = tracker_ontologies_get_properties (&n_props);
	domain_indexes = tracker_class_get_domain_indexes (service);

	for (i = 0; i < n_props; i++) {
		gboolean is_domain_index;

		property = properties[i];
		is_domain_index = is_a_domain_index (domain_indexes, property);

		if (tracker_property_get_domain (property) == service || is_domain_index) {
			gboolean put_change;
			const gchar *sql_type_for_single_value = NULL;
			const gchar *field_name;

			create_decomposed_metadata_property_table (iface, property,
			                                           service_name,
			                                           service,
			                                           &sql_type_for_single_value,
			                                           in_update,
			                                           in_change,
			                                           &internal_error);

			if (internal_error) {
				g_propagate_error (error, internal_error);
				goto error_out;
			}

			field_name = tracker_property_get_name (property);

			if (sql_type_for_single_value) {
				const gchar *default_value;

				/* single value */

				default_value = tracker_property_get_default_value (property);

				if (in_update) {
					g_debug ("%sAltering database for class '%s' property '%s': single value (%s)",
					         in_alter ? "" : "  ",
					         service_name,
					         field_name,
					         in_alter ? "alter" : "create");
				}

				if (!in_alter) {
					put_change = TRUE;
					class_properties = g_slist_prepend (class_properties, property);

					g_string_append_printf (create_sql, ", \"%s\" %s",
					                        field_name,
					                        sql_type_for_single_value);

					if (!copy_schedule) {
						copy_schedule = g_ptr_array_new_with_free_func (g_free);
					}

					if (is_domain_index && tracker_property_get_is_new_domain_index (property, service)) {
						schedule_copy (copy_schedule, property, field_name, NULL);
					}

					if (g_ascii_strcasecmp (sql_type_for_single_value, "TEXT") == 0) {
						g_string_append (create_sql, " COLLATE " TRACKER_COLLATION_NAME);
					}

					/* add DEFAULT in case that the ontology specifies a default value,
					   assumes that default values never contain quotes */
					if (default_value != NULL) {
						g_string_append_printf (create_sql, " DEFAULT '%s'", default_value);
					}

					if (tracker_property_get_is_inverse_functional_property (property)) {
						g_string_append (create_sql, " UNIQUE");
					}

					g_string_append_printf (create_sql, ", \"%s:graph\" INTEGER",
					                        field_name);

					if (is_domain_index && tracker_property_get_is_new_domain_index (property, service)) {
						schedule_copy (copy_schedule, property, field_name, ":graph");
					}

					if (tracker_property_get_data_type (property) == TRACKER_PROPERTY_TYPE_DATETIME) {
						/* xsd:dateTime is stored in three columns:
						 * universal time, local date, local time of day */
						g_string_append_printf (create_sql, ", \"%s:localDate\" INTEGER, \"%s:localTime\" INTEGER",
						                        tracker_property_get_name (property),
						                        tracker_property_get_name (property));

						if (is_domain_index && tracker_property_get_is_new_domain_index (property, service)) {
							schedule_copy (copy_schedule, property, field_name, ":localTime");
							schedule_copy (copy_schedule, property, field_name, ":localDate");
						}

					}

				} else if ((!is_domain_index && tracker_property_get_is_new (property)) ||
				           (is_domain_index && tracker_property_get_is_new_domain_index (property, service))) {
					GString *alter_sql = NULL;

					put_change = FALSE;
					class_properties = g_slist_prepend (class_properties, property);

					alter_sql = g_string_new ("ALTER TABLE ");
					g_string_append_printf (alter_sql, "\"%s\" ADD COLUMN \"%s\" %s",
					                        service_name,
					                        field_name,
					                        sql_type_for_single_value);

					if (g_ascii_strcasecmp (sql_type_for_single_value, "TEXT") == 0) {
						g_string_append (alter_sql, " COLLATE " TRACKER_COLLATION_NAME);
					}

					/* add DEFAULT in case that the ontology specifies a default value,
					   assumes that default values never contain quotes */
					if (default_value != NULL) {
						g_string_append_printf (alter_sql, " DEFAULT '%s'", default_value);
					}

					if (tracker_property_get_is_inverse_functional_property (property)) {
						g_string_append (alter_sql, " UNIQUE");
					}

					g_debug ("Altering: '%s'", alter_sql->str);
					tracker_db_interface_execute_query (iface, &internal_error, "%s", alter_sql->str);
					if (internal_error) {
						g_string_free (alter_sql, TRUE);
						g_propagate_error (error, internal_error);
						goto error_out;
					} else if (is_domain_index) {
						copy_from_domain_to_domain_index (iface, property,
						                                  field_name, NULL,
						                                  service,
						                                  &internal_error);
						if (internal_error) {
							g_string_free (alter_sql, TRUE);
							g_propagate_error (error, internal_error);
							goto error_out;
						}

						/* This is implicit for all domain-specific-indices */
						set_index_for_single_value_property (iface, service_name,
						                                     field_name, TRUE,
						                                     &internal_error);
						if (internal_error) {
							g_string_free (alter_sql, TRUE);
							g_propagate_error (error, internal_error);
							goto error_out;
						}
					}

					g_string_free (alter_sql, TRUE);

					alter_sql = g_string_new ("ALTER TABLE ");
					g_string_append_printf (alter_sql, "\"%s\" ADD COLUMN \"%s:graph\" INTEGER",
					                        service_name,
					                        field_name);
					g_debug ("Altering: '%s'", alter_sql->str);
					tracker_db_interface_execute_query (iface, &internal_error,
					                                    "%s", alter_sql->str);
					if (internal_error) {
						g_string_free (alter_sql, TRUE);
						g_propagate_error (error, internal_error);
						goto error_out;
					} else if (is_domain_index) {
						copy_from_domain_to_domain_index (iface, property,
						                                  field_name, ":graph",
						                                  service,
						                                  &internal_error);
						if (internal_error) {
							g_string_free (alter_sql, TRUE);
							g_propagate_error (error, internal_error);
							goto error_out;
						}
					}

					g_string_free (alter_sql, TRUE);

					if (tracker_property_get_data_type (property) == TRACKER_PROPERTY_TYPE_DATETIME) {
						alter_sql = g_string_new ("ALTER TABLE ");
						g_string_append_printf (alter_sql, "\"%s\" ADD COLUMN \"%s:localDate\" INTEGER",
						                        service_name,
						                        field_name);
						g_debug ("Altering: '%s'", alter_sql->str);
						tracker_db_interface_execute_query (iface, &internal_error,
						                                    "%s", alter_sql->str);

						if (internal_error) {
							g_string_free (alter_sql, TRUE);
							g_propagate_error (error, internal_error);
							goto error_out;
						} else if (is_domain_index) {
							copy_from_domain_to_domain_index (iface, property,
							                                  field_name, ":localDate",
							                                  service,
							                                  &internal_error);
							if (internal_error) {
								g_string_free (alter_sql, TRUE);
								g_propagate_error (error, internal_error);
								goto error_out;
							}
						}

						g_string_free (alter_sql, TRUE);

						alter_sql = g_string_new ("ALTER TABLE ");
						g_string_append_printf (alter_sql, "\"%s\" ADD COLUMN \"%s:localTime\" INTEGER",
						                        service_name,
						                        field_name);
						g_debug ("Altering: '%s'", alter_sql->str);
						tracker_db_interface_execute_query (iface, &internal_error,
						                                    "%s", alter_sql->str);
						if (internal_error) {
							g_string_free (alter_sql, TRUE);
							g_propagate_error (error, internal_error);
							goto error_out;
						} else if (is_domain_index) {
							copy_from_domain_to_domain_index (iface, property,
							                                  field_name, ":localTime",
							                                  service,
							                                  &internal_error);
							if (internal_error) {
								g_string_free (alter_sql, TRUE);
								g_propagate_error (error, internal_error);
								goto error_out;
							}
						}
						g_string_free (alter_sql, TRUE);
					}
				} else {
					put_change = TRUE;
				}

				if (in_change && put_change) {
					range_change_for (property, in_col_sql, sel_col_sql, field_name);
				}
			}
		}
	}

	if (create_sql) {
		g_string_append (create_sql, ")");
		g_debug ("Creating: '%s'", create_sql->str);
		tracker_db_interface_execute_query (iface, &internal_error,
		                                    "%s", create_sql->str);

		if (internal_error) {
			g_propagate_error (error, internal_error);
			goto error_out;
		}
	}

	/* create index for single-valued fields */
	for (field_it = class_properties; field_it != NULL; field_it = field_it->next) {
		TrackerProperty *field, *secondary_index;
		const char *field_name;
		gboolean is_domain_index;

		field = field_it->data;

		/* This is implicit for all domain-specific-indices */
		is_domain_index = is_a_domain_index (domain_indexes, field);

		if (!tracker_property_get_multiple_values (field)
		    && (tracker_property_get_indexed (field) || is_domain_index)) {

			field_name = tracker_property_get_name (field);

			secondary_index = tracker_property_get_secondary_index (field);
			if (secondary_index == NULL) {
				set_index_for_single_value_property (iface, service_name,
				                                     field_name, TRUE,
				                                     &internal_error);
				if (internal_error) {
					g_propagate_error (error, internal_error);
					goto error_out;
				}
			} else {
				set_secondary_index_for_single_value_property (iface, service_name, field_name,
				                                               tracker_property_get_name (secondary_index),
				                                               TRUE, &internal_error);
				if (internal_error) {
					g_propagate_error (error, internal_error);
					goto error_out;
				}
			}
		}
	}

	if (in_change && sel_col_sql && in_col_sql) {
		gchar *query;

		query = g_strdup_printf ("INSERT INTO \"%s\"(%s) "
		                         "SELECT %s FROM \"%s_TEMP\"",
		                         service_name, in_col_sql->str,
		                         sel_col_sql->str, service_name);

		g_debug ("Copy: %s", query);

		tracker_db_interface_execute_query (iface, &internal_error, "%s", query);

		if (internal_error) {
			g_propagate_error (error, internal_error);
			goto error_out;
		}

		g_free (query);
		g_debug ("Rename (drop): DROP TABLE \"%s_TEMP\"", service_name);
		tracker_db_interface_execute_query (iface, &internal_error,
		                                    "DROP TABLE \"%s_TEMP\"", service_name);

		if (internal_error) {
			g_propagate_error (error, internal_error);
			goto error_out;
		}
	}

	if (copy_schedule) {
		guint i;
		for (i = 0; i < copy_schedule->len; i++) {
			ScheduleCopy *sched = g_ptr_array_index (copy_schedule, i);
			copy_from_domain_to_domain_index (iface, sched->prop,
			                                  sched->field_name, sched->suffix,
			                                  service,
			                                  &internal_error);

			if (internal_error) {
				g_propagate_error (error, internal_error);
				break;
			}
		}
	}

error_out:

	if (copy_schedule) {
		g_ptr_array_free (copy_schedule, TRUE);
	}

	if (create_sql) {
		g_string_free (create_sql, TRUE);
	}

	g_slist_free (class_properties);

	if (in_col_sql) {
		g_string_free (in_col_sql, TRUE);
	}

	if (sel_col_sql) {
		g_string_free (sel_col_sql, TRUE);
	}
}

static void
clean_decomposed_transient_metadata (TrackerDBInterface *iface)
{
	TrackerProperty **properties;
	TrackerProperty *property;
	gint i, n_props;

	properties = tracker_ontologies_get_properties (&n_props);

	for (i = 0; i < n_props; i++) {
		property = properties[i];

		if (tracker_property_get_transient (property)) {
			TrackerClass *domain;
			const gchar *service_name;
			const gchar *prop_name;
			GError *error = NULL;

			domain = tracker_property_get_domain (property);
			service_name = tracker_class_get_name (domain);
			prop_name = tracker_property_get_name (property);

			if (tracker_property_get_multiple_values (property)) {
				/* create the disposable table */
				tracker_db_interface_execute_query (iface, &error, "DELETE FROM \"%s_%s\"",
				                                    service_name,
				                                    prop_name);
			} else {
				/* create the disposable table */
				tracker_db_interface_execute_query (iface, &error, "UPDATE \"%s\" SET \"%s\" = NULL",
				                                    service_name,
				                                    prop_name);
			}

			if (error) {
				g_critical ("Cleaning transient propery '%s:%s' failed: %s",
				            service_name,
				            prop_name,
				            error->message);
				g_error_free (error);
			}
		}
	}
}

static void
tracker_data_ontology_import_finished (void)
{
	TrackerClass **classes;
	TrackerProperty **properties;
	gint i, n_props, n_classes;

	classes = tracker_ontologies_get_classes (&n_classes);
	properties = tracker_ontologies_get_properties (&n_props);

	for (i = 0; i < n_classes; i++) {
		tracker_class_set_is_new (classes[i], FALSE);
		tracker_class_set_db_schema_changed (classes[i], FALSE);
	}

	for (i = 0; i < n_props; i++) {
		tracker_property_set_is_new_domain_index (properties[i], NULL, FALSE);
		tracker_property_set_is_new (properties[i], FALSE);
		tracker_property_set_db_schema_changed (properties[i], FALSE);
	}
}

static void
tracker_data_ontology_import_into_db (gboolean   in_update,
                                      GError   **error)
{
	TrackerDBInterface *iface;

	TrackerClass **classes;
	TrackerProperty **properties;
	gint i, n_props, n_classes;

	iface = tracker_db_manager_get_db_interface ();

	classes = tracker_ontologies_get_classes (&n_classes);
	properties = tracker_ontologies_get_properties (&n_props);

	/* create tables */
	for (i = 0; i < n_classes; i++) {
		GError *internal_error = NULL;

		/* Also !is_new classes are processed, they might have new properties */
		create_decomposed_metadata_tables (iface, classes[i], in_update,
		                                   tracker_class_get_db_schema_changed (classes[i]),
		                                   &internal_error);

		if (internal_error) {
			g_propagate_error (error, internal_error);
			return;
		}
	}

	/* insert classes into rdfs:Resource table */
	for (i = 0; i < n_classes; i++) {
		if (tracker_class_get_is_new (classes[i]) == in_update) {
			GError *internal_error = NULL;

			insert_uri_in_resource_table (iface, tracker_class_get_uri (classes[i]),
			                              tracker_class_get_id (classes[i]),
			                              &internal_error);

			if (internal_error) {
				g_propagate_error (error, internal_error);
				return;
			}
		}
	}

	/* insert properties into rdfs:Resource table */
	for (i = 0; i < n_props; i++) {
		if (tracker_property_get_is_new (properties[i]) == in_update) {
			GError *internal_error = NULL;

			insert_uri_in_resource_table (iface, tracker_property_get_uri (properties[i]),
			                              tracker_property_get_id (properties[i]),
			                              &internal_error);

			if (internal_error) {
				g_propagate_error (error, internal_error);
				return;
			}
		}
	}
}

static GList*
get_ontologies (gboolean     test_schema,
                const gchar *ontologies_dir)
{
	GList *sorted = NULL;

	if (test_schema) {
		sorted = g_list_prepend (sorted, g_strdup ("12-nrl.ontology"));
		sorted = g_list_prepend (sorted, g_strdup ("11-rdf.ontology"));
		sorted = g_list_prepend (sorted, g_strdup ("10-xsd.ontology"));
	} else {
		GDir        *ontologies;
		const gchar *conf_file;

		ontologies = g_dir_open (ontologies_dir, 0, NULL);

		conf_file = g_dir_read_name (ontologies);

		/* .ontology files */
		while (conf_file) {
			if (g_str_has_suffix (conf_file, ".ontology")) {
				sorted = g_list_insert_sorted (sorted,
				                               g_strdup (conf_file),
				                               (GCompareFunc) strcmp);
			}
			conf_file = g_dir_read_name (ontologies);
		}

		g_dir_close (ontologies);
	}

	return sorted;
}


static gint
get_new_service_id (TrackerDBInterface *iface)
{
	TrackerDBCursor    *cursor = NULL;
	TrackerDBStatement *stmt;
	gint max_service_id = 0;
	GError *error = NULL;

	/* Don't intermix this thing with tracker_data_update_get_new_service_id,
	 * if you use this, know what you are doing! */

	iface = tracker_db_manager_get_db_interface ();

	stmt = tracker_db_interface_create_statement (iface, TRACKER_DB_STATEMENT_CACHE_TYPE_SELECT, &error,
	                                              "SELECT MAX(ID) AS A FROM Resource WHERE ID <= %d", TRACKER_ONTOLOGIES_MAX_ID);

	if (stmt) {
		cursor = tracker_db_statement_start_cursor (stmt, &error);
		g_object_unref (stmt);
	}

	if (cursor) {
		if (tracker_db_cursor_iter_next (cursor, NULL, &error)) {
			max_service_id = tracker_db_cursor_get_int (cursor, 0);
		}
		g_object_unref (cursor);
	}

	if (error) {
		g_error ("Unable to get max ID, aborting: %s", error->message);
	}

	return ++max_service_id;
}

static void
tracker_data_manager_recreate_indexes (TrackerBusyCallback    busy_callback,
                                       gpointer               busy_user_data,
                                       const gchar           *busy_status,
                                       GError               **error)
{
	GError *internal_error = NULL;
	TrackerProperty **properties;
	guint n_properties;
	guint i;

	properties = tracker_ontologies_get_properties (&n_properties);
	if (!properties) {
		g_critical ("Couldn't get all properties to recreate indexes");
		return;
	}

	g_debug ("Dropping all indexes...");
	for (i = 0; i < n_properties; i++) {
		fix_indexed (properties [i], FALSE, &internal_error);

		if (internal_error) {
			g_propagate_error (error, internal_error);
			return;
		}
	}

	g_debug ("Starting index re-creation...");
	for (i = 0; i < n_properties; i++) {
		fix_indexed (properties [i], TRUE, &internal_error);

		if (internal_error) {
			g_critical ("Unable to create index for %s: %s",
			            tracker_property_get_name (properties[i]),
			            internal_error->message);
			g_clear_error (&internal_error);
		}

		if (busy_callback) {
			busy_callback (busy_status,
			               (gdouble) ((gdouble) i / (gdouble) n_properties),
			               busy_user_data);
		}
	}
	g_debug ("  Finished index re-creation...");
}

gboolean
tracker_data_manager_reload (TrackerBusyCallback   busy_callback,
                             gpointer              busy_user_data,
                             const gchar          *busy_operation,
                             GError              **error)
{
	TrackerDBManagerFlags flags;
	guint select_cache_size;
	guint update_cache_size;
	gboolean is_first;
	gboolean status;
	GError *internal_error = NULL;

	g_message ("Reloading data manager...");
	/* Shutdown data manager... */
	flags = tracker_db_manager_get_flags (&select_cache_size, &update_cache_size);
	reloading = TRUE;
	tracker_data_manager_shutdown ();

	g_message ("  Data manager shut down, now initializing again...");

	/* And initialize it again, this actually triggers index recreation. */
	status = tracker_data_manager_init (flags,
	                                    NULL,
	                                    &is_first,
	                                    TRUE,
	                                    FALSE,
	                                    select_cache_size,
	                                    update_cache_size,
	                                    busy_callback,
	                                    busy_user_data,
	                                    busy_operation,
	                                    &internal_error);
	reloading = FALSE;

	if (internal_error) {
		g_propagate_error (error, internal_error);
	}

	g_message ("  %s reloading data manager",
	           status ? "Succeeded" : "Failed");

	return status;
}

static void
write_ontologies_gvdb (gboolean   overwrite,
                       GError   **error)
{
	gchar *filename;

	filename = g_build_filename (g_get_user_cache_dir (),
	                             "tracker",
	                             "ontologies.gvdb",
	                             NULL);

	if (overwrite || !g_file_test (filename, G_FILE_TEST_EXISTS)) {
		tracker_ontologies_write_gvdb (filename, error);
	}

	g_free (filename);
}

static void
load_ontologies_gvdb (GError **error)
{
	gchar *filename;

	filename = g_build_filename (g_get_user_cache_dir (),
	                             "tracker",
	                             "ontologies.gvdb",
	                             NULL);

	tracker_ontologies_load_gvdb (filename, error);

	g_free (filename);
}

#if HAVE_TRACKER_FTS
static gboolean
ontology_get_fts_properties (gboolean     only_new,
			     GHashTable **fts_properties,
			     GHashTable **multivalued)
{
	TrackerProperty **properties;
	gboolean has_new = FALSE;
	GHashTable *hashtable;
	guint i, len;

	properties = tracker_ontologies_get_properties (&len);
	hashtable = g_hash_table_new_full (g_str_hash, g_str_equal, NULL,
					   (GDestroyNotify) g_list_free);

	if (multivalued) {
		*multivalued = g_hash_table_new (g_str_hash, g_str_equal);
	}

	for (i = 0; i < len; i++) {
		const gchar *name, *table_name;
		GList *list;

		if (!tracker_property_get_fulltext_indexed (properties[i])) {
			continue;
		}

		has_new |= tracker_property_get_is_new (properties[i]);
		table_name = tracker_property_get_table_name (properties[i]);

		if (multivalued &&
		    tracker_property_get_multiple_values (properties[i])) {
			g_hash_table_insert (*multivalued, (gpointer) table_name,
					     GUINT_TO_POINTER (TRUE));
		}

		name = tracker_property_get_name (properties[i]);
		list = g_hash_table_lookup (hashtable, table_name);

		if (!list) {
			list = g_list_prepend (NULL, (gpointer) name);
			g_hash_table_insert (hashtable, (gpointer) table_name, list);
		} else {
			list = g_list_append (list, (gpointer) name);
		}
	}

	if (fts_properties) {
		*fts_properties = hashtable;
	}

	return has_new;
}
#endif

gboolean
tracker_data_manager_init_fts (TrackerDBInterface *iface,
                               gboolean            create)
{
#if HAVE_TRACKER_FTS
	GHashTable *fts_props, *multivalued;

	ontology_get_fts_properties (FALSE, &fts_props, &multivalued);
	tracker_db_interface_sqlite_fts_init (iface, fts_props,
	                                      multivalued, create);
	g_hash_table_unref (fts_props);
	g_hash_table_unref (multivalued);
	return TRUE;
#else
	g_message ("FTS support is disabled");
	return FALSE;
#endif
}

gboolean
tracker_data_manager_init (TrackerDBManagerFlags   flags,
                           const gchar           **test_schemas,
                           gboolean               *first_time,
                           gboolean                journal_check,
                           gboolean                restoring_backup,
                           guint                   select_cache_size,
                           guint                   update_cache_size,
                           TrackerBusyCallback     busy_callback,
                           gpointer                busy_user_data,
                           const gchar            *busy_operation,
                           GError                **error)
{
	TrackerDBInterface *iface;
	gboolean is_first_time_index, check_ontology;
	TrackerDBCursor *cursor;
	TrackerDBStatement *stmt;
	GHashTable *ontos_table;
	GList *sorted = NULL, *l;
	const gchar *env_path;
	gint max_id = 0;
	gboolean read_only;
	GHashTable *uri_id_map = NULL;
	gchar *busy_status;
	GError *internal_error = NULL;
#ifndef DISABLE_JOURNAL
	gboolean read_journal;
#endif

	read_only = (flags & TRACKER_DB_MANAGER_READONLY) ? TRUE : FALSE;

	tracker_data_update_init ();

#ifdef HAVE_TRACKER_FTS
	if (!tracker_fts_init ()) {
		g_warning ("FTS module loading failed");
	}
#endif

	/* First set defaults for return values */
	if (first_time) {
		*first_time = FALSE;
	}

	if (initialized) {
		return TRUE;
	}

	/* Make sure we initialize all other modules we depend on */
	tracker_ontologies_init ();

	if (!reloading) {
		tracker_locale_init ();
	}

#ifndef DISABLE_JOURNAL
	read_journal = FALSE;
#endif

	if (!tracker_db_manager_init (flags,
	                              &is_first_time_index,
	                              restoring_backup,
	                              FALSE,
	                              select_cache_size,
	                              update_cache_size,
	                              busy_callback,
	                              busy_user_data,
	                              busy_operation,
	                              &internal_error)) {
		g_propagate_error (error, internal_error);

		tracker_ontologies_shutdown ();
		if (!reloading) {
			tracker_locale_shutdown ();
		}
		tracker_data_update_shutdown ();

		return FALSE;
	}

	/* Report OPERATION - STATUS */
	if (busy_callback) {
		busy_status = g_strdup_printf ("%s - %s",
		                               busy_operation,
		                               "Initializing data manager");
		busy_callback (busy_status, 0, busy_user_data);
		g_free (busy_status);
	}


	if (first_time != NULL) {
		*first_time = is_first_time_index;
	}

	iface = tracker_db_manager_get_db_interface ();

#ifndef DISABLE_JOURNAL
	if (journal_check && is_first_time_index) {
		/* Call may fail without notice (it's handled) */
		if (tracker_db_journal_reader_init (NULL, &internal_error)) {
			if (tracker_db_journal_reader_next (NULL)) {
				/* journal with at least one valid transaction
				   is required to trigger journal replay */
				read_journal = TRUE;
			}
			tracker_db_journal_reader_shutdown ();
		} else if (internal_error) {
			if (!g_error_matches (internal_error,
			                      TRACKER_DB_JOURNAL_ERROR,
			                      TRACKER_DB_JOURNAL_ERROR_BEGIN_OF_JOURNAL)) {
				g_propagate_error (error, internal_error);

				tracker_db_manager_shutdown ();
				tracker_ontologies_shutdown ();
				if (!reloading) {
					tracker_locale_shutdown ();
				}
				tracker_data_update_shutdown ();

				return FALSE;
			} else {
				g_clear_error (&internal_error);
			}
		}
	}
#endif /* DISABLE_JOURNAL */

	env_path = g_getenv ("TRACKER_DB_ONTOLOGIES_DIR");

	if (G_LIKELY (!env_path)) {
		ontologies_dir = g_build_filename (SHAREDIR,
		                                   "tracker",
		                                   "ontologies",
		                                   NULL);
	} else {
		ontologies_dir = g_strdup (env_path);
	}

#ifndef DISABLE_JOURNAL
	if (read_journal) {
		in_journal_replay = TRUE;

		if (tracker_db_journal_reader_ontology_init (NULL, &internal_error)) {
			/* Load ontology IDs from journal into memory */
			load_ontology_ids_from_journal (&uri_id_map, &max_id);

			tracker_db_journal_reader_shutdown ();
		} else {
			if (internal_error) {
				if (!g_error_matches (internal_error,
					              TRACKER_DB_JOURNAL_ERROR,
					              TRACKER_DB_JOURNAL_ERROR_BEGIN_OF_JOURNAL)) {
					g_propagate_error (error, internal_error);

					tracker_db_manager_shutdown ();
					tracker_ontologies_shutdown ();
					if (!reloading) {
						tracker_locale_shutdown ();
					}
					tracker_data_update_shutdown ();

					return FALSE;
				} else {
					g_clear_error (&internal_error);
				}
			}

			/* do not trigger journal replay if ontology journal
			   does not exist or is not valid,
			   same as with regular journal further above */
			in_journal_replay = FALSE;
			read_journal = FALSE;
		}
	}
#endif /* DISABLE_JOURNAL */

	if (is_first_time_index && !read_only) {
		sorted = get_ontologies (test_schemas != NULL, ontologies_dir);

#ifndef DISABLE_JOURNAL
		if (!read_journal) {
			/* Truncate journal as it does not even contain a single valid transaction
			 * or is explicitly ignored (journal_check == FALSE, only for test cases) */
			tracker_db_journal_init (NULL, TRUE, &internal_error);

			if (internal_error) {
				g_propagate_error (error, internal_error);

				tracker_db_manager_shutdown ();
				tracker_ontologies_shutdown ();
				if (!reloading) {
					tracker_locale_shutdown ();
				}
				tracker_data_update_shutdown ();

				return FALSE;
			}
		}
#endif /* DISABLE_JOURNAL */

		/* load ontology from files into memory (max_id starts at zero: first-time) */

		for (l = sorted; l; l = l->next) {
			GError *ontology_error = NULL;
			gchar *ontology_path;
			g_debug ("Loading ontology %s", (char *) l->data);
			ontology_path = g_build_filename (ontologies_dir, l->data, NULL);
			load_ontology_file_from_path (ontology_path,
			                              &max_id,
			                              FALSE,
			                              NULL,
			                              NULL,
			                              uri_id_map,
			                              &ontology_error);
			if (ontology_error) {
				g_error ("Error loading ontology (%s): %s",
				         ontology_path,
				         ontology_error->message);
			}
			g_free (ontology_path);

		}

		if (test_schemas) {
			guint p;
			for (p = 0; test_schemas[p] != NULL; p++) {
				GError *ontology_error = NULL;
				gchar *test_schema_path;
				test_schema_path = g_strconcat (test_schemas[p], ".ontology", NULL);

				g_debug ("Loading ontology:'%s' (TEST ONTOLOGY)", test_schema_path);

				load_ontology_file_from_path (test_schema_path,
				                              &max_id,
				                              FALSE,
				                              NULL,
				                              NULL,
				                              uri_id_map,
				                              &ontology_error);
				if (ontology_error) {
					g_error ("Error loading ontology (%s): %s",
					         test_schema_path,
					         ontology_error->message);
				}
				g_free (test_schema_path);
			}
		}

		tracker_data_begin_ontology_transaction (&internal_error);
		if (internal_error) {
			g_propagate_error (error, internal_error);

#ifndef DISABLE_JOURNAL
			tracker_db_journal_shutdown (NULL);
#endif /* DISABLE_JOURNAL */
			tracker_db_manager_shutdown ();
			tracker_ontologies_shutdown ();
			if (!reloading) {
				tracker_locale_shutdown ();
			}
			tracker_data_update_shutdown ();

			return FALSE;
		}

		tracker_data_ontology_import_into_db (FALSE,
		                                      &internal_error);

		tracker_data_manager_init_fts (iface, TRUE);

		if (internal_error) {
			g_propagate_error (error, internal_error);

#ifndef DISABLE_JOURNAL
			tracker_db_journal_shutdown (NULL);
#endif /* DISABLE_JOURNAL */
			tracker_db_manager_shutdown ();
			tracker_ontologies_shutdown ();
			if (!reloading) {
				tracker_locale_shutdown ();
			}
			tracker_data_update_shutdown ();

			return FALSE;
		}

#ifndef DISABLE_JOURNAL
		if (uri_id_map) {
			/* restore all IDs from ontology journal */
			GHashTableIter iter;
			gpointer key, value;

			g_hash_table_iter_init (&iter, uri_id_map);
			while (g_hash_table_iter_next (&iter, &key, &value)) {
				insert_uri_in_resource_table (iface,
				                              key,
				                              GPOINTER_TO_INT (value),
				                              &internal_error);
				if (internal_error) {
					g_propagate_error (error, internal_error);

					tracker_db_journal_shutdown (NULL);
					tracker_db_manager_shutdown ();
					tracker_ontologies_shutdown ();
					if (!reloading) {
						tracker_locale_shutdown ();
					}
					tracker_data_update_shutdown ();

					return FALSE;
				}
			}
		}
#endif /* DISABLE_JOURNAL */

		/* store ontology in database */
		for (l = sorted; l; l = l->next) {
			gchar *ontology_path = g_build_filename (ontologies_dir, l->data, NULL);
			import_ontology_path (ontology_path, FALSE, !journal_check);
			g_free (ontology_path);
		}

		if (test_schemas) {
			guint p;
			for (p = 0; test_schemas[p] != NULL; p++) {
				gchar *test_schema_path;

				test_schema_path = g_strconcat (test_schemas[p], ".ontology", NULL);
				import_ontology_path (test_schema_path, FALSE, TRUE);
				g_free (test_schema_path);
			}
		}

		tracker_data_commit_transaction (&internal_error);
		if (internal_error) {
			g_propagate_error (error, internal_error);

#ifndef DISABLE_JOURNAL
			tracker_db_journal_shutdown (NULL);
#endif /* DISABLE_JOURNAL */
			tracker_db_manager_shutdown ();
			tracker_ontologies_shutdown ();
			if (!reloading) {
				tracker_locale_shutdown ();
			}
			tracker_data_update_shutdown ();

			return FALSE;
		}

		write_ontologies_gvdb (TRUE /* overwrite */, NULL);

		g_list_foreach (sorted, (GFunc) g_free, NULL);
		g_list_free (sorted);
		sorted = NULL;

		/* First time, no need to check ontology */
		check_ontology = FALSE;
	} else {
		if (!read_only) {

#ifndef DISABLE_JOURNAL
			tracker_db_journal_init (NULL, FALSE, &internal_error);

			if (internal_error) {
				g_propagate_error (error, internal_error);

				tracker_db_manager_shutdown ();
				tracker_ontologies_shutdown ();
				if (!reloading) {
					tracker_locale_shutdown ();
				}
				tracker_data_update_shutdown ();

				return FALSE;
			}
#endif /* DISABLE_JOURNAL */

			/* Load ontology from database into memory */
			db_get_static_data (iface, &internal_error);
			check_ontology = (flags & TRACKER_DB_MANAGER_DO_NOT_CHECK_ONTOLOGY) == 0;

			if (internal_error) {
				g_propagate_error (error, internal_error);
				return FALSE;
			}

			write_ontologies_gvdb (FALSE /* overwrite */, NULL);

			/* Skipped in the read-only case as it can't work with direct access and
			   it reduces initialization time */
			clean_decomposed_transient_metadata (iface);
		} else {
			GError *gvdb_error = NULL;

			load_ontologies_gvdb (&gvdb_error);
			check_ontology = FALSE;

			if (gvdb_error) {
				g_critical ("Error loading ontology cache: %s",
				            gvdb_error->message);
				g_clear_error (&gvdb_error);

				/* fall back to loading ontology from database into memory */
				db_get_static_data (iface, &internal_error);
				if (internal_error) {
					g_propagate_error (error, internal_error);
					return FALSE;
				}
			}
		}

		tracker_data_manager_init_fts (iface, FALSE);
	}

	if (check_ontology) {
		GList *to_reload = NULL;
		GList *ontos = NULL;
		guint p;
		GPtrArray *seen_classes;
		GPtrArray *seen_properties;
		GError *n_error = NULL;
		gboolean transaction_started = FALSE;

		seen_classes = g_ptr_array_new ();
		seen_properties = g_ptr_array_new ();

		/* Get all the ontology files from ontologies_dir */
		sorted = get_ontologies (test_schemas != NULL, ontologies_dir);

		for (l = sorted; l; l = l->next) {
			gchar *ontology_path;
			ontology_path = g_build_filename (ontologies_dir, l->data, NULL);
			ontos = g_list_append (ontos, ontology_path);
		}

		g_list_foreach (sorted, (GFunc) g_free, NULL);
		g_list_free (sorted);

		if (test_schemas) {
			for (p = 0; test_schemas[p] != NULL; p++) {
				gchar *test_schema_path;
				test_schema_path = g_strconcat (test_schemas[p], ".ontology", NULL);
				ontos = g_list_append (ontos, test_schema_path);
			}
		}

		/* check ontology against database */

		/* Get a map of tracker:Ontology v. nao:lastModified so that we can test
		 * for all the ontology files in ontologies_dir whether the last-modified
		 * has changed since we dealt with the file last time. */

		stmt = tracker_db_interface_create_statement (iface, TRACKER_DB_STATEMENT_CACHE_TYPE_SELECT, &n_error,
		        "SELECT Resource.Uri, \"rdfs:Resource\".\"nao:lastModified\" FROM \"tracker:Ontology\" "
		        "INNER JOIN Resource ON Resource.ID = \"tracker:Ontology\".ID "
		        "INNER JOIN \"rdfs:Resource\" ON \"tracker:Ontology\".ID = \"rdfs:Resource\".ID");

		if (stmt) {
			cursor = tracker_db_statement_start_cursor (stmt, &n_error);
			g_object_unref (stmt);
		} else {
			cursor = NULL;
		}

		ontos_table = g_hash_table_new_full (g_str_hash,
		                                     g_str_equal,
		                                     g_free,
		                                     NULL);

		if (cursor) {
			while (tracker_db_cursor_iter_next (cursor, NULL, &n_error)) {
				const gchar *onto_uri = tracker_db_cursor_get_string (cursor, 0, NULL);
				/* It's stored as an int in the db anyway. This is caused by
				 * string_to_gvalue in tracker-data-update.c */
				gint value = tracker_db_cursor_get_int (cursor, 1);

				g_hash_table_insert (ontos_table, g_strdup (onto_uri),
				                     GINT_TO_POINTER (value));
			}

			g_object_unref (cursor);
		}

		if (n_error) {
			g_warning ("%s", n_error->message);
			g_clear_error (&n_error);
		}

		for (l = ontos; l; l = l->next) {
			TrackerOntology *ontology;
			const gchar *ontology_path = l->data;
			const gchar *ontology_uri;
			gboolean found, update_nao = FALSE;
			gpointer value;
			gint last_mod;

			/* Parse a TrackerOntology from ontology_file */
			ontology = get_ontology_from_path (ontology_path);

			if (!ontology) {
				/* TODO: cope with full custom .ontology files: deal with this
				 * error gracefully. App devs might install wrong ontology files
				 * and we shouldn't critical() due to this. */
				g_critical ("Can't get ontology from file: %s", ontology_path);
				continue;
			}

			ontology_uri = tracker_ontology_get_uri (ontology);
			/* We can't do better than this cast, it's stored as an int in the
			 * db. See above comment for more info. */
			last_mod = (gint) tracker_ontology_get_last_modified (ontology);

			found = g_hash_table_lookup_extended (ontos_table,
			                                      ontology_uri,
			                                      NULL, &value);

			if (found) {
				GError *ontology_error = NULL;
				gint val = GPOINTER_TO_INT (value);

				/* When the last-modified in our database isn't the same as the last
				 * modified in the latest version of the file, deal with changes. */
				if (val != last_mod) {
					g_debug ("Ontology file '%s' needs update", ontology_path);

					if (!transaction_started) {
						tracker_data_begin_ontology_transaction (&internal_error);
						if (internal_error) {
							g_propagate_error (error, internal_error);

#ifndef DISABLE_JOURNAL
							tracker_db_journal_shutdown (NULL);
#endif /* DISABLE_JOURNAL */
							tracker_db_manager_shutdown ();
							tracker_ontologies_shutdown ();
							if (!reloading) {
								tracker_locale_shutdown ();
							}
							tracker_data_update_shutdown ();

							return FALSE;
						}
						transaction_started = TRUE;
					}

					if (max_id == 0) {
						/* In case of first-time, this wont start at zero */
						max_id = get_new_service_id (iface);
					}
					/* load ontology from files into memory, set all new's
					 * is_new to TRUE */
					load_ontology_file_from_path (ontology_path,
					                              &max_id,
					                              TRUE,
					                              seen_classes,
					                              seen_properties,
					                              uri_id_map,
					                              &ontology_error);

					if (g_error_matches (ontology_error,
					                     TRACKER_DATA_ONTOLOGY_ERROR,
					                     TRACKER_DATA_UNSUPPORTED_ONTOLOGY_CHANGE)) {
						g_warning ("%s", ontology_error->message);
						g_error_free (ontology_error);

						tracker_data_ontology_free_seen (seen_classes);
						tracker_data_ontology_free_seen (seen_properties);
						tracker_data_ontology_import_finished ();

						/* as we're processing an ontology change,
						   transaction is guaranteed to be started */
						tracker_data_rollback_transaction ();

						if (ontos_table) {
							g_hash_table_unref (ontos_table);
						}
						if (ontos) {
							g_list_foreach (ontos, (GFunc) g_free, NULL);
							g_list_free (ontos);
						}
						g_free (ontologies_dir);
						if (uri_id_map) {
							g_hash_table_unref (uri_id_map);
						}
						initialized = TRUE;

						/* This also does tracker_locale_shutdown */
						tracker_data_manager_shutdown ();

						return tracker_data_manager_init (flags | TRACKER_DB_MANAGER_DO_NOT_CHECK_ONTOLOGY,
						                                  test_schemas,
						                                  first_time,
						                                  journal_check,
						                                  restoring_backup,
						                                  select_cache_size,
						                                  update_cache_size,
						                                  busy_callback,
						                                  busy_user_data,
						                                  busy_operation,
						                                  error);
					}

					if (ontology_error) {
						g_critical ("Fatal error dealing with ontology changes: %s", ontology_error->message);
						g_error_free (ontology_error);
					}

					to_reload = g_list_prepend (to_reload, l->data);
					update_nao = TRUE;
				}
			} else {
				GError *ontology_error = NULL;

				g_debug ("Ontology file '%s' got added", ontology_path);

				if (!transaction_started) {
					tracker_data_begin_ontology_transaction (&internal_error);
					if (internal_error) {
						g_propagate_error (error, internal_error);

#ifndef DISABLE_JOURNAL
						tracker_db_journal_shutdown (NULL);
#endif /* DISABLE_JOURNAL */
						tracker_db_manager_shutdown ();
						tracker_ontologies_shutdown ();
						if (!reloading) {
							tracker_locale_shutdown ();
						}
						tracker_data_update_shutdown ();

						return FALSE;
					}
					transaction_started = TRUE;
				}

				if (max_id == 0) {
					/* In case of first-time, this wont start at zero */
					max_id = get_new_service_id (iface);
				}
				/* load ontology from files into memory, set all new's
				 * is_new to TRUE */
				load_ontology_file_from_path (ontology_path,
				                              &max_id,
				                              TRUE,
				                              seen_classes,
				                              seen_properties,
				                              uri_id_map,
				                              &ontology_error);

				if (g_error_matches (ontology_error,
				                     TRACKER_DATA_ONTOLOGY_ERROR,
				                     TRACKER_DATA_UNSUPPORTED_ONTOLOGY_CHANGE)) {
					g_warning ("%s", ontology_error->message);
					g_error_free (ontology_error);

					tracker_data_ontology_free_seen (seen_classes);
					tracker_data_ontology_free_seen (seen_properties);
					tracker_data_ontology_import_finished ();

					/* as we're processing an ontology change,
					   transaction is guaranteed to be started */
					tracker_data_rollback_transaction ();

					if (ontos_table) {
						g_hash_table_unref (ontos_table);
					}
					if (ontos) {
						g_list_foreach (ontos, (GFunc) g_free, NULL);
						g_list_free (ontos);
					}
					g_free (ontologies_dir);
					if (uri_id_map) {
						g_hash_table_unref (uri_id_map);
					}
					initialized = TRUE;

					/* This also does tracker_locale_shutdown */
					tracker_data_manager_shutdown ();

					return tracker_data_manager_init (flags | TRACKER_DB_MANAGER_DO_NOT_CHECK_ONTOLOGY,
					                                  test_schemas,
					                                  first_time,
					                                  journal_check,
					                                  restoring_backup,
					                                  select_cache_size,
					                                  update_cache_size,
					                                  busy_callback,
					                                  busy_user_data,
					                                  busy_operation,
					                                  error);
				}

				if (ontology_error) {
					g_critical ("Fatal error dealing with ontology changes: %s", ontology_error->message);
					g_error_free (ontology_error);
				}

				to_reload = g_list_prepend (to_reload, l->data);
				update_nao = TRUE;
			}

			if (update_nao) {
#if HAVE_TRACKER_FTS
				GHashTable *fts_properties, *multivalued;

				if (ontology_get_fts_properties (TRUE, &fts_properties, &multivalued)) {
					tracker_db_interface_sqlite_fts_alter_table (iface, fts_properties, multivalued);
				}

				g_hash_table_unref (fts_properties);
				g_hash_table_unref (multivalued);
#endif

				/* Update the nao:lastModified in the database */
				stmt = tracker_db_interface_create_statement (iface, TRACKER_DB_STATEMENT_CACHE_TYPE_UPDATE, &n_error,
				        "UPDATE \"rdfs:Resource\" SET \"nao:lastModified\"= ? "
				        "WHERE \"rdfs:Resource\".ID = "
				        "(SELECT Resource.ID FROM Resource INNER JOIN \"rdfs:Resource\" "
				        "ON \"rdfs:Resource\".ID = Resource.ID WHERE "
				        "Resource.Uri = ?)");

				if (stmt) {
					tracker_db_statement_bind_int (stmt, 0, last_mod);
					tracker_db_statement_bind_text (stmt, 1, ontology_uri);
					tracker_db_statement_execute (stmt, &n_error);
					g_object_unref (stmt);
				}

				if (n_error) {
					g_critical ("%s", n_error->message);
					g_clear_error (&n_error);
				}
			}

			g_object_unref (ontology);
		}

		if (to_reload) {
			GError *ontology_error = NULL;

			tracker_data_ontology_process_changes_pre_db (seen_classes,
			                                              seen_properties,
			                                              &ontology_error);

			if (!ontology_error) {
				/* Perform ALTER-TABLE and CREATE-TABLE calls for all that are is_new */
				tracker_data_ontology_import_into_db (TRUE,
				                                      &ontology_error);

				if (!ontology_error) {
					tracker_data_ontology_process_changes_post_db (seen_classes,
					                                               seen_properties,
					                                               &ontology_error);
				}
			}

			if (g_error_matches (ontology_error,
			                     TRACKER_DATA_ONTOLOGY_ERROR,
			                     TRACKER_DATA_UNSUPPORTED_ONTOLOGY_CHANGE)) {
				g_warning ("%s", ontology_error->message);
				g_error_free (ontology_error);

				tracker_data_ontology_free_seen (seen_classes);
				tracker_data_ontology_free_seen (seen_properties);
				tracker_data_ontology_import_finished ();

				/* as we're processing an ontology change,
				   transaction is guaranteed to be started */
				tracker_data_rollback_transaction ();

				if (ontos_table) {
					g_hash_table_unref (ontos_table);
				}
				if (ontos) {
					g_list_foreach (ontos, (GFunc) g_free, NULL);
					g_list_free (ontos);
				}
				g_free (ontologies_dir);
				if (uri_id_map) {
					g_hash_table_unref (uri_id_map);
				}
				initialized = TRUE;

				/* This also does tracker_locale_shutdown */
				tracker_data_manager_shutdown ();

				return tracker_data_manager_init (flags | TRACKER_DB_MANAGER_DO_NOT_CHECK_ONTOLOGY,
				                                  test_schemas,
				                                  first_time,
				                                  journal_check,
				                                  restoring_backup,
				                                  select_cache_size,
				                                  update_cache_size,
				                                  busy_callback,
				                                  busy_user_data,
				                                  busy_operation,
				                                  error);
			}

			if (ontology_error) {
				g_critical ("Fatal error dealing with ontology changes: %s", ontology_error->message);
				g_propagate_error (error, ontology_error);

#ifndef DISABLE_JOURNAL
				tracker_db_journal_shutdown (NULL);
#endif /* DISABLE_JOURNAL */
				tracker_db_manager_shutdown ();
				tracker_ontologies_shutdown ();
				if (!reloading) {
					tracker_locale_shutdown ();
				}
				tracker_data_update_shutdown ();

				return FALSE;
			}

			for (l = to_reload; l; l = l->next) {
				const gchar *ontology_path = l->data;
				/* store ontology in database */
				import_ontology_path (ontology_path, TRUE, !journal_check);
			}
			g_list_free (to_reload);

			tracker_data_ontology_process_changes_post_import (seen_classes, seen_properties);

			write_ontologies_gvdb (TRUE /* overwrite */, NULL);
		}

		tracker_data_ontology_free_seen (seen_classes);
		tracker_data_ontology_free_seen (seen_properties);

		/* Reset the is_new flag for all classes and properties */
		tracker_data_ontology_import_finished ();

		if (transaction_started) {
			tracker_data_commit_transaction (&internal_error);
			if (internal_error) {
				g_propagate_error (error, internal_error);

#ifndef DISABLE_JOURNAL
				tracker_db_journal_shutdown (NULL);
#endif /* DISABLE_JOURNAL */
				tracker_db_manager_shutdown ();
				tracker_ontologies_shutdown ();
				if (!reloading) {
					tracker_locale_shutdown ();
				}
				tracker_data_update_shutdown ();

				return FALSE;
			}
		}

		g_hash_table_unref (ontos_table);

		g_list_foreach (ontos, (GFunc) g_free, NULL);
		g_list_free (ontos);
	}

#ifndef DISABLE_JOURNAL
	if (read_journal) {
		/* Report OPERATION - STATUS */
		busy_status = g_strdup_printf ("%s - %s",
		                               busy_operation,
		                               "Replaying journal");
		/* Start replay */
		tracker_data_replay_journal (busy_callback,
		                             busy_user_data,
		                             busy_status,
		                             &internal_error);
		g_free (busy_status);

		if (internal_error) {

			if (g_error_matches (internal_error, TRACKER_DB_INTERFACE_ERROR, TRACKER_DB_NO_SPACE)) {
				GError *n_error = NULL;
				tracker_db_manager_remove_all (FALSE);
				tracker_db_manager_shutdown ();
				/* Call may fail without notice, we're in error handling already.
				 * When fails it means that close() of journal file failed. */
				tracker_db_journal_shutdown (&n_error);
				if (n_error) {
					g_warning ("Error closing journal: %s",
					           n_error->message ? n_error->message : "No error given");
					g_error_free (n_error);
				}
			}

			g_hash_table_unref (uri_id_map);
			g_propagate_error (error, internal_error);

			tracker_db_journal_shutdown (NULL);
			tracker_db_manager_shutdown ();
			tracker_ontologies_shutdown ();
			if (!reloading) {
				tracker_locale_shutdown ();
			}
			tracker_data_update_shutdown ();

			return FALSE;
		}

		in_journal_replay = FALSE;

		/* open journal for writing */
		tracker_db_journal_init (NULL, FALSE, &internal_error);

		if (internal_error) {
			g_hash_table_unref (uri_id_map);
			g_propagate_error (error, internal_error);

			tracker_db_journal_shutdown (NULL);
			tracker_db_manager_shutdown ();
			tracker_ontologies_shutdown ();
			if (!reloading) {
				tracker_locale_shutdown ();
			}
			tracker_data_update_shutdown ();

			return FALSE;
		}

		g_hash_table_unref (uri_id_map);
	}
#endif /* DISABLE_JOURNAL */

	/* If locale changed, re-create indexes */
	if (!read_only && tracker_db_manager_locale_changed ()) {
		/* Report OPERATION - STATUS */
		busy_status = g_strdup_printf ("%s - %s",
		                               busy_operation,
		                               "Recreating indexes");
		/* No need to reset the collator in the db interface,
		 * as this is only executed during startup, which should
		 * already have the proper locale set in the collator */
		tracker_data_manager_recreate_indexes (busy_callback,
		                                       busy_user_data,
		                                       busy_status,
		                                       &internal_error);
		g_free (busy_status);

		if (internal_error) {
			g_propagate_error (error, internal_error);

#ifndef DISABLE_JOURNAL
			tracker_db_journal_shutdown (NULL);
#endif /* DISABLE_JOURNAL */
			tracker_db_manager_shutdown ();
			tracker_ontologies_shutdown ();
			if (!reloading) {
				tracker_locale_shutdown ();
			}
			tracker_data_update_shutdown ();

			return FALSE;
		}

		tracker_db_manager_set_current_locale ();
	}

	if (!read_only) {
		tracker_ontologies_sort ();
	}

	initialized = TRUE;

	g_free (ontologies_dir);

	/* This is the only one which doesn't show the 'OPERATION' part */
	if (busy_callback) {
		busy_callback ("Idle", 1, busy_user_data);
	}

	return TRUE;
}

void
tracker_data_manager_shutdown (void)
{
#ifndef DISABLE_JOURNAL
	GError *error = NULL;
#endif /* DISABLE_JOURNAL */

	g_return_if_fail (initialized == TRUE);

#ifndef DISABLE_JOURNAL
	/* Make sure we shutdown all other modules we depend on */
	tracker_db_journal_shutdown (&error);

	if (error) {
		/* TODO: propagate error */
		g_warning ("While shutting down journal %s",
		           error->message ? error->message : "No error given");
		g_error_free (error);
	}
#endif /* DISABLE_JOURNAL */

	tracker_db_manager_shutdown ();
	tracker_ontologies_shutdown ();
	if (!reloading) {
		tracker_locale_shutdown ();
	}
	tracker_data_update_shutdown ();

	initialized = FALSE;
}
