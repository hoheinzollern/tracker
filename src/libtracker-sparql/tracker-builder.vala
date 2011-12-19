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


/**
 * SECTION: tracker-sparql-builder
 * @short_description: Creating insertion/update SPARQL queries.
 * @title: TrackerSparqlBuilder
 * @stability: Stable
 * @include: tracker-sparql.h
 *
 * <para>
 * #TrackerSparqlBuilder is an object what will gather a set of
 * subject/predicate/object triples, together with an optional WHERE clause,
 * in order to create a query that may be issued to tracker-store.
 * </para>
 *
 * <para>
 * When using #TrackerSparqlBuilder, note that you may append several predicates
 * for the same subject, and several objects for the same predicate.
 * </para>
 */


/**
 * TrackerSparqlBuilder:
 *
 * The <structname>TrackerSparqlBuilder</structname> object represents an
 * insertion/update SPARQL query.
 */

/**
 * tracker_sparql_builder_new:
 *
 * Creates a stateless #TrackerSparqlBuilder.
 *
 * Returns: a newly created #TrackerSparqlBuilder. Free with g_object_unref() when done
 *
 * Since: 0.10
 */
public class Tracker.Sparql.Builder : Object {

	/**
	 * TrackerSparqlBuilderState:
	 * @TRACKER_SPARQL_BUILDER_STATE_UPDATE: Builder is generating an UPDATE
	 * @TRACKER_SPARQL_BUILDER_STATE_INSERT: Builder is generating an INSERT
	 * @TRACKER_SPARQL_BUILDER_STATE_DELETE: Builder is generating a DELETE
	 * @TRACKER_SPARQL_BUILDER_STATE_SUBJECT: Builder is generating the subject of the query
	 * @TRACKER_SPARQL_BUILDER_STATE_PREDICATE: Builder is generating the predicate of the query
	 * @TRACKER_SPARQL_BUILDER_STATE_OBJECT: Builder is generating the object of the query
	 * @TRACKER_SPARQL_BUILDER_STATE_BLANK: Builder is generating a blank node subject
	 * @TRACKER_SPARQL_BUILDER_STATE_WHERE: Builder is generating the WHERE clause contents
	 * @TRACKER_SPARQL_BUILDER_STATE_EMBEDDED_INSERT: Builder is generating an embedded INSERT
	 * @TRACKER_SPARQL_BUILDER_STATE_GRAPH: Builder is generating the GRAPH clause contents
	 *
	 * Enumeration with the possible states of the SPARQL Builder
	 */
	public enum State {
		UPDATE,
		INSERT,
		DELETE,
		SUBJECT,
		PREDICATE,
		OBJECT,
		BLANK,
		WHERE,
		EMBEDDED_INSERT,
		GRAPH
	}

	/**
	 * tracker_sparql_builder_get_result:
	 * @self: a #TrackerSparqlBuilder
	 *
	 * Retrieves a string representation of the constructed SPARQL query.
	 *
	 * Returns: the created SPARQL query. The string is contained in the
	 * #TrackerSparqlBuilder object, and should not be freed by the caller.
	 *
	 * Since: 0.10
	 */

	/**
	 * TrackerSparqlBuilder:result:
	 *
	 * String containing the constructed SPARQL in the #TrackerSparqlBuilder.
	 *
	 * Since: 0.10
	 */
	public string result {
		get {
			warn_if_fail (states.length == 1 ||
			              (states[0] == State.EMBEDDED_INSERT &&
			               states.length == 3));
			return str.str;
		}
	}

	/**
	 * tracker_sparql_builder_get_length:
	 * @self: a #TrackerSparqlBuilder
	 *
	 * Returns the number of objects added to @self.
	 *
	 * Returns: the number of objects contained.
	 *
	 * Since: 0.10
	 */

	/**
	 * TrackerSparqlBuilder:length:
	 *
	 * Number of objects added to the #TrackerSparqlBuilder.
	 *
	 * Since: 0.10
	 */
	public int length {
		get;
		private set;
	}

	/**
	 * tracker_sparql_builder_get_state:
	 * @self: a #TrackerSparqlBuilder
	 *
	 * Returns the current state of @self
	 *
	 * Returns: a #TrackerSparqlBuilderState defining the current state of @self
	 *
	 * Since: 0.10
	 */

	/**
	 * TrackerSparqlBuilder:state:
	 *
	 * Current state of the #TrackerSparqlBuilder.
	 *
	 * Since: 0.10
	 */
	public State state {
		get { return states[states.length - 1]; }
	}

	State[] states;
	StringBuilder str = new StringBuilder ();

	/**
	 * tracker_sparql_builder_new_update:
	 *
	 * Creates an empty #TrackerSparqlBuilder for an update query.
	 *
	 * Returns: a newly created #TrackerSparqlBuilder. Free with g_object_unref() when done
	 *
	 * Since: 0.10
	 */
	public Builder.update () {
		states += State.UPDATE;
	}

	/**
	 * tracker_sparql_builder_new_embedded_insert:
	 *
	 * Creates a #TrackerSparqlBuilder ready to be embedded in another query. In embedded
	 * inserts, the subject is implied (responsibility of the embedder), so only calls to
	 * append predicates and objects for the given subject are allowed.
	 *
	 * Returns: a newly created #TrackerSparqlBuilder. Free with g_object_unref() when done
	 *
	 * Since: 0.10
	 */
	public Builder.embedded_insert () {
		states += State.EMBEDDED_INSERT;
		states += State.INSERT;
		states += State.SUBJECT;
	}

	/**
	 * tracker_sparql_builder_insert_open:
	 * @self: a #TrackerSparqlBuilder
	 * @graph: graph name, or %NULL.
	 *
	 * Opens an insertion statement.
	 *
	 * Since: 0.10
	 */
	public void insert_open (string? graph)
		requires (state == State.UPDATE)
	{
		states += State.INSERT;
		if (graph != null)
			str.append ("INSERT INTO <%s> {\n".printf (graph));
		else
			str.append ("INSERT {\n");
	}

	/**
	 * tracker_sparql_builder_insert_silent_open:
	 * @self: a #TrackerSparqlBuilder
	 * @graph: graph name, or %NULL.
	 *
	 * Opens a silent insertion statement.
	 *
	 * Since: 0.10
	 */
	public void insert_silent_open (string? graph)
		requires (state == State.UPDATE)
	{
		states += State.INSERT;
		if (graph != null)
			str.append ("INSERT SILENT INTO <%s> {\n".printf (graph));
		else
			str.append ("INSERT SILENT {\n");
	}

	/**
	 * tracker_sparql_builder_insert_close:
	 * @self: a #TrackerSparqlBuilder
	 *
	 * Closes an insertion statement opened with tracker_sparql_builder_insert_open().
	 *
	 * Since: 0.10
	 */
	public void insert_close ()
		requires (state == State.INSERT || state == State.OBJECT)
	{
		if (state == State.OBJECT) {
			str.append (" .\n");
			states.length -= 3;
		}
		states.length--;

		if (state != State.EMBEDDED_INSERT) {
			str.append ("}\n");
		}
	}

	/**
	 * tracker_sparql_builder_delete_open:
	 * @self: a #TrackerSparqlBuilder
	 * @graph: graph name, or %NULL.
	 *
	 * Opens a DELETE clause. Data triples may be appended in order to prepare
	 * a query to delete them.
	 *
	 * Since: 0.10
	 */
	public void delete_open (string? graph)
		requires (state == State.UPDATE)
	{
		states += State.DELETE;
		if (graph != null)
			str.append ("DELETE FROM <%s> {\n".printf (graph));
		else
			str.append ("DELETE {\n");
	}

	/**
	 * tracker_sparql_builder_delete_close:
	 * @self: a #TrackerSparqlBuilder
	 *
	 * Closes a DELETE clause opened through tracker_sparql_builder_delete_open().
	 *
	 * Since: 0.10
	 */
	public void delete_close ()
		requires (state == State.DELETE || state == State.OBJECT)
	{
		if (state == State.OBJECT) {
			str.append (" .\n");
			states.length -= 3;
		}
		states.length--;

		str.append ("}\n");
	}

	/**
	 * tracker_sparql_builder_graph_open:
	 * @self: a #TrackerSparqlBuilder
	 * @graph: graph name.
	 *
	 * Opens a GRAPH clause within INSERT, DELETE, or WHERE.
	 *
	 * Since: 0.10
	 */
	public void graph_open (string graph)
		requires (state == State.INSERT || state == State.DELETE || state == State.OBJECT || state == State.WHERE || state == State.GRAPH)
	{
		states += State.GRAPH;
		str.append_printf ("GRAPH <%s> {\n", graph);
	}

	/**
	 * tracker_sparql_builder_graph_close:
	 * @self: a #TrackerSparqlBuilder
	 *
	 * Closes a GRAPH clause opened through tracker_sparql_builder_graph_open().
	 *
	 * Since: 0.10
	 */
	public void graph_close ()
		requires (state == State.GRAPH || state == State.OBJECT)
	{
		if (state == State.OBJECT) {
			str.append (" .\n");
			states.length -= 3;
		}
		states.length--;

		str.append ("}\n");
	}

	/**
	 * tracker_sparql_builder_where_open:
	 * @self: a #TrackerSparqlBuilder
	 *
	 * Opens a WHERE clause. Data triples may be appended then to narrow the scope
	 * to which the update query applies.
	 *
	 * Since: 0.10
	 */
	public void where_open ()
	       requires (state == State.UPDATE)
	{
		states += State.WHERE;
		str.append ("WHERE {\n");
	}

	/**
	 * tracker_sparql_builder_where_close:
	 * @self: a #TrackerSparqlBuilder
	 *
	 * Closes a WHERE clause opened through tracker_sparql_builder_where_open().
	 *
	 * Since: 0.10
	 */
	public void where_close ()
		requires (state == State.WHERE || state == State.OBJECT)
	{
		if (state == State.OBJECT) {
			str.append (" .\n");
			states.length -= 3;
		}
		states.length--;
		str.append ("}\n");
	}

	/**
	 * tracker_sparql_builder_subject_variable:
	 * @self: a #TrackerSparqlBuilder
	 * @var_name: variable name, without leading '?'
	 *
	 * Appends a subject as a SPARQL variable, such as "?urn".
	 *
	 * Since: 0.10
	 */
	public void subject_variable (string var_name) {
		subject ("?%s".printf (var_name));
	}

	/**
	 * tracker_sparql_builder_object_variable:
	 * @self: a #TrackerSparqlBuilder
	 * @var_name: variable name, without leading '?'
	 *
	 * Appends an object as a SparQL variable, such as "?urn".
	 *
	 * Since: 0.10
	 */
	public void object_variable (string var_name) {
		object ("?%s".printf (var_name));
	}

	/**
	 * tracker_sparql_builder_subject_iri:
	 * @self: a #TrackerSparqlBuilder
	 * @iri: IRI name, without leading and trailing greater/less than symbols.
	 *
	 * Appends a subject as an IRI, such as "&lt;urn:file:1234-5678&gt;". IRIs
	 * univocally identify a resource in tracker-store.
	 *
	 * Since: 0.10
	 */
	public void subject_iri (string iri) {
		subject ("<%s>".printf (iri));
	}

	/**
	 * tracker_sparql_builder_subject:
	 * @self: a #TrackerSparqlBuilder
	 * @s: subject string
	 *
	 * Appends a subject.
	 *
	 * Since: 0.10
	 */
	public void subject (string s)
		requires (state == State.INSERT || state == State.OBJECT || state == State.EMBEDDED_INSERT || state == State.DELETE || state == State.WHERE || state == State.GRAPH)
	{
		if (state == State.OBJECT) {
			str.append (" .\n");
			states.length -= 3;
		}
		str.append (s);
		states += State.SUBJECT;
	}

	/**
	 * tracker_sparql_builder_predicate_iri:
	 * @self: a #TrackerSparqlBuilder
	 * @iri: IRI name, without leading and trailing greater/less than symbols.
	 *
	 * Appends a predicate as an IRI.
	 *
	 * Since: 0.10
	 */
	public void predicate_iri (string iri) {
		predicate ("<%s>".printf (iri));
	}

	/**
	 * tracker_sparql_builder_predicate:
	 * @self: a #TrackerSparqlBuilder
	 * @s: predicate string
	 *
	 * Appends a predicate for the previously appended subject.
	 *
	 * Since: 0.10
	 */
	public void predicate (string s)
		requires (state == State.SUBJECT || state == State.OBJECT || state == State.BLANK)
	{
		if (state == State.OBJECT) {
			str.append (" ;\n\t");
			states.length -= 2;
		}
		str.append (" ");
		str.append (s);
		states += State.PREDICATE;
	}

	/**
	 * tracker_sparql_builder_object_iri:
	 * @self: a #TrackerSparqlBuilder
	 * @iri: IRI name, without leading and trailing greater/less than symbols.
	 *
	 * Appends an object as an IRI.
	 *
	 * Since: 0.10
	 */
	public void object_iri (string iri) {
		object ("<%s>".printf (iri));
	}

	/**
	 * tracker_sparql_builder_object:
	 * @self: a #TrackerSparqlBuilder
	 * @s: object string
	 *
	 * Appends a free-form object for the previously appended subject and predicate.
	 *
	 * Since: 0.10
	 */
	public void object (string s)
		requires (state == State.PREDICATE || state == State.OBJECT)
	{
		if (state == State.OBJECT) {
			str.append (" ,");
			states.length--;
		}
		str.append (" ");
		str.append (s);
		states += State.OBJECT;

		length++;
	}

	/**
	 * tracker_sparql_builder_object_string:
	 * @self: a #TrackerSparqlBuilder
	 * @literal: string object
	 *
	 * Appends an object formatted as an string. @literal will be escaped and surrounded
	 * by double quotes.
	 *
	 * Since: 0.10
	 */
	public void object_string (string literal)
		requires (state == State.PREDICATE || state == State.OBJECT)
	{
		if (state == State.OBJECT) {
			str.append (" ,");
			states.length--;
		}

		str.append (" \"");

		char* p = literal;
		while (*p != '\0') {
			size_t len = Posix.strcspn ((string) p, "\t\n\r\"\\");
			str.append_len ((string) p, (long) len);
			p += len;
			switch (*p) {
			case '\t':
				str.append ("\\t");
				break;
			case '\n':
				str.append ("\\n");
				break;
			case '\r':
				str.append ("\\r");
				break;
			case '"':
				str.append ("\\\"");
				break;
			case '\\':
				str.append ("\\\\");
				break;
			default:
				continue;
			}
			p++;
		}

		str.append ("\"");

		states += State.OBJECT;

		length++;
	}

	/**
	 * tracker_sparql_builder_object_unvalidated:
	 * @self: a #TrackerSparqlBuilder
	 * @value: possibly UTF-8 invalid string.
	 *
	 * Appends a string not validated as UTF-8 as an object.
	 *
	 * Since: 0.10
	 */
	public void object_unvalidated (string value) {
		char* end;

		if (!value.validate (-1, out end)) {
			if (value != end) {
				object_string (value.substring (0, (long) (end - (char*) value)));
			} else {
				object_string ("(invalid data)");
			}

			return;
		}

		object_string (value);
	}

	/**
	 * tracker_sparql_builder_object_boolean:
	 * @self: a #TrackerSparqlBuilder
	 * @literal: object as a #gboolean
	 *
	 * Appends a #gboolean value as an object.
	 *
	 * Since: 0.10
	 */
	public void object_boolean (bool literal) {
		object (literal ? "true" : "false");
	}

	/**
	 * tracker_sparql_builder_object_int64:
	 * @self: a #TrackerSparqlBuilder
	 * @literal: object as a #gint64
	 *
	 * Appends a #gint64 value as an object.
	 *
	 * Since: 0.10
	 */
	public void object_int64 (int64 literal) {
		object (literal.to_string ());
	}

	/**
	 * tracker_sparql_builder_object_date:
	 * @self: a #TrackerSparqlBuilder
	 * @literal: object as a #time_t
	 *
	 * Appends a #time_t value as an object. @literal will be converted
	 * to a string in the date format used by tracker-store.
	 *
	 * Since: 0.10
	 */
	public void object_date (ref time_t literal) {
		var tm = Time.gm (literal);

		object_string ("%04d-%02d-%02dT%02d:%02d:%02dZ".printf (tm.year + 1900, tm.month + 1, tm.day, tm.hour, tm.minute, tm.second));
	}

	/**
	 * tracker_sparql_builder_object_double:
	 * @self: a #TrackerSparqlBuilder
	 * @literal: object as a #gdouble
	 *
	 * Appends a #gdouble value as an object.
	 *
	 * Since: 0.10
	 */
	public void object_double (double literal) {
		object (literal.to_string ());
	}

	/**
	 * tracker_sparql_builder_object_blank_open:
	 * @self: a #TrackerSparqlBuilder
	 *
	 * Opens an anonymous blank node. In insertions this can be used to create
	 * anonymous nodes for not previously known data without the need of a
	 * separate insertion.
	 *
	 * Since: 0.10
	 */
	public void object_blank_open ()
		requires (state == State.PREDICATE || state == State.OBJECT)
	{
		if (state == State.OBJECT) {
			str.append (" ,");
			states.length--;
		}
		str.append (" [");
		states += State.BLANK;
	}

	/**
	 * tracker_sparql_builder_object_blank_close:
	 * @self: a #TrackerSparqlBuilder
	 *
	 * Closes an anomymous blank node opened with tracker_sparql_builder_object_blank_open()
	 *
	 * Since: 0.10
	 */
	public void object_blank_close ()
		requires (state == State.OBJECT && states[states.length - 3] == state.BLANK)
	{
		str.append ("]");
		states.length -= 3;
		states += State.OBJECT;

		length++;
	}

	/**
	 * tracker_sparql_builder_prepend:
	 * @self: a #TrackerSparqlBuilder
	 * @raw: raw content to prepend.
	 *
	 * Prepends raw, unvalidated content to @self.
	 *
	 * Since: 0.10
	 */
	public void prepend (string raw)
	{
		str.prepend ("%s\n".printf (raw));

		length++;
	}

	/**
	 * tracker_sparql_builder_append:
	 * @self: a #TrackerSparqlBuilder
	 * @raw: raw content to append.
	 *
	 * Appends raw, unvalidated content to @self.
	 *
	 * Since: 0.10
	 */
	public void append (string raw)
	{
		if (state == State.OBJECT) {
			str.append (" .\n");
			states.length -= 3;
		}

		str.append (raw);

		length++;
	}
}

