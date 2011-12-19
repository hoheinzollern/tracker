//
// Copyright 2010, Carlos Garnacho <carlos@lanedo.com>
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
// 02110-1301, USA.
//

using Gtk;

public class Tracker.ResultStore : Gtk.TreeModel, GLib.Object {
	private GLib.Cancellable cancellable;

	private struct ResultNode {
		string [] values;
		Gdk.Pixbuf pixbuf;
	}

	private class CategoryNode {
		public Tracker.Query.Type type;
		public QueryData *query;
		public ResultNode [] results;
		public Gdk.Pixbuf pixbuf;
		public int count;
	}

	private struct QueryData {
		Tracker.Query.Type type;
		Tracker.Query.Match match;
		string [] args;
	}

	private QueryData [] queries;
	private GenericArray<CategoryNode> categories;

	private class Operation : GLib.Object {
		public CategoryNode node;
		public int offset;
	}

	private GenericArray<Operation> running_operations;
	private GenericArray<Operation> delayed_operations;

	private int n_extra_columns = 2; // Pixbuf and query type
	private int n_columns;
	private int timestamp;

	public int icon_size {
		get;
		set;
	}

	public uint limit {
		get;
		set;
	}

	public signal void result_overflow ();

	private Operation? find_operation (GenericArray<Operation> array, CategoryNode node, int offset) {
		Operation op;
		int i;

		for (i = 0; i < array.length; i++) {
			op = array[i];

			if (op.node == node &&
			    op.offset == offset) {
				return op;
			}
		}

		return null;
	}

	async void load_operation (Operation op, Cancellable? cancellable) {
		Tracker.Query query;
		Sparql.Cursor cursor = null;
		int i;

		try {
			cancellable.set_error_if_cancelled ();

			query = new Tracker.Query ();
			query.criteria = _search_term;
			query.tags = search_tags;
			query.limit = limit;
			query.offset = op.offset;

			cursor = yield query.perform_async (op.node.query.type, op.node.query.match, op.node.query.args, cancellable);

			cancellable.set_error_if_cancelled ();

			if (cursor != null) {
				for (i = op.offset; i < op.offset + 100; i++) {
					ResultNode *result;
					TreeIter iter;
					TreePath path;
					bool b = false;
					int j;

					try {
						b = yield cursor.next_async (cancellable);
					} catch (GLib.Error ge) {
						if (!cancellable.is_cancelled ()) {
							warning ("Could not fetch row: %s\n", ge.message);
						}
					}

					if (!b) {
						break;
					}

					result = &op.node.results[i];

					for (j = 0; j < n_columns; j++) {
						if (j == n_columns - 1) {
							// FIXME: Set markup for tooltip column in a nicer way
							string s = cursor.get_string (j);

							if (s != null)
								result.values[j] = Markup.escape_text (s);
							else
								result.values[j] = null;
						} else {
							result.values[j] = cursor.get_string (j);
						}
					}

					// Emit row-changed
					iter = TreeIter ();
					iter.stamp = this.timestamp;
					iter.user_data = op.node;
					iter.user_data2 = result;
					iter.user_data3 = i.to_pointer ();

					path = this.get_path (iter);
					row_changed (path, iter);
				}
			}

			running_operations.remove (op);
		} catch (GLib.IOError ie) {
			if (!cancellable.is_cancelled ()) {
				warning ("Could not load items: %s\n", ie.message);
			}
			return;
		}

		if (delayed_operations.length > 0) {
			Operation next_to_start;

			// Take last added task from delayed queue and start it
			next_to_start = delayed_operations[delayed_operations.length - 1];
			delayed_operations.remove (next_to_start);
			running_operations.add (next_to_start);

			load_operation.begin (next_to_start, cancellable);
		} else if (running_operations.length == 0) {
			// finished processing
			this.active = false;
		}
	}

	private void add_operation (CategoryNode cat, int offset) {
		Operation op = new Operation ();
		Operation old;

		op.node = cat;
		op.offset = offset;

		if (find_operation (running_operations, cat, offset) != null) {
			// Operation already running
			return;
		}

		// If the task is delayed, it will be either pushed
		// to the running queue, or reordered to be processed
		// next in the delayed queue.
		old = find_operation (delayed_operations, cat, offset);

		if (old != null) {
			delayed_operations.remove (old);
		}

		this.active = true;

		// Running queue is limited to 2 simultaneous queries,
		// anything after that will be added to a different queue.
		if (running_operations.length < 2) {
			running_operations.add (op);

			// Start the operation right away
			load_operation.begin (op, cancellable);
		} else {
			// Reorder the operation if it was already there, else just add
			delayed_operations.add (op);
		}
	}

	async void load_category (QueryData *query_data, Cancellable? cancellable) {
		uint count = 0;

		try {
			cancellable.set_error_if_cancelled ();

			Tracker.Query query = new Tracker.Query ();
			query.criteria = _search_term;
			query.tags = search_tags;

			count = yield query.get_count_async (query_data.type, query_data.match, cancellable);
			cancellable.set_error_if_cancelled ();
		} catch (GLib.IOError ie) {
			if (!cancellable.is_cancelled ()) {
				warning ("Could not get count: %s\n", ie.message);
			}
			return;
		}

		if (count != 0) {
			CategoryNode cat;
			ResultNode *res;
			int i;

			if (count > limit) {
				result_overflow ();
				count = limit;
			}

			Gtk.TreeIter iter;
			Gtk.TreePath path;

			cat = new CategoryNode ();
			cat.type = query_data.type;
			cat.query = query_data;
			cat.results.resize ((int) count);
			categories.add (cat);

			iter = TreeIter ();
			iter.stamp = this.timestamp;
			iter.user_data = cat;

			if (queries.length > 1) {
				path = this.get_path (iter);
				row_inserted (path, iter);
			}

			for (i = 0; i < count; i++) {
				res = &cat.results[i];
				res.values = new string[n_columns];

				iter.user_data2 = res;
				iter.user_data3 = i.to_pointer ();
				path = this.get_path (iter);

				cat.count++;
				row_inserted (path, iter);
			}

			if (queries.length > 1) {
				iter.user_data2 = null;
				iter.user_data3 = null;
				path = get_path (iter);

				row_changed (path, iter);
			}
		}

		if (running_operations.length == 0) {
			this.active = false;
		}
	}

	private void clear_results () {
		int j;

		while (categories.length > 0) {
			CategoryNode cat = categories[0];
			TreeIter iter;
			TreePath path;

			if (cat.results.length == 0) {
				continue;
			}

			iter = TreeIter ();
			iter.stamp = this.timestamp;
			iter.user_data = cat;

			for (j = cat.count - 1; j >= 0; j--) {
				iter.user_data2 = &cat.results[j];
				iter.user_data3 = j.to_pointer ();
				path = get_path (iter);

				row_deleted (path);
				cat.count--;
			}

			if (queries.length > 1) {
				iter.user_data2 = null;
				iter.user_data3 = null;
				path = get_path (iter);

				row_deleted (path);
			}

			categories.remove (cat);
		}
	}

	private string _search_term;
	public string search_term {
		get {
			return _search_term;
		}
		set {
			int i;

			cancel_search ();
			_search_term = value;

			cancellable = new Cancellable ();

			this.active = true;

			categories = new GenericArray<CategoryNode> ();
			running_operations = new GenericArray<Operation?> ();
			delayed_operations = new GenericArray<Operation?> ();
			this.timestamp++;

			for (i = 0; i < queries.length; i++) {
				load_category.begin (&queries[i], cancellable);
			}
		}
	}

	public GenericArray<string> search_tags { get; set; }

	public bool active {
		get;
		private set;
	}

	private int find_nth_category_index (CategoryNode? node, int n) {
		int i;

		if (node == null) {
			// Count from the first one
			return n;
		}

		for (i = 0; i < categories.length; i++) {
			CategoryNode cat;

			cat = categories[i];

			if (cat == node) {
				return i + n;
			}
		}

		return -1;
	}

	private int filled_categories_count () {
		int i, n = 0;

		for (i = 0; i < categories.length; i++) {
			CategoryNode cat;

			cat = categories[i];

			if (cat.count > 0) {
				n++;
			}
		}

		return n;
	}

	public GLib.Type get_column_type (int index_) {
		if (index_ == n_columns) {
			return typeof (Gdk.Pixbuf);
		} else if (index_ == n_columns + 1) {
			return typeof (Tracker.Query.Type);
		} else {
			return typeof (string);
		}
	}

	public Gtk.TreeModelFlags get_flags () {
		Gtk.TreeModelFlags flags;

		flags = Gtk.TreeModelFlags.ITERS_PERSIST;

		if (queries.length == 1) {
			flags |= Gtk.TreeModelFlags.LIST_ONLY;
		}

		return flags;
	}

	public bool get_iter (out Gtk.TreeIter iter, Gtk.TreePath path) {
		unowned int [] indices = path.get_indices ();
		CategoryNode cat;
		int i = 0;

		iter = TreeIter ();

		if (queries.length > 1) {
			if (indices[i] >= categories.length) {
				iter.stamp = 0;
				return false;
			}

			cat = categories[indices[i]];
			i++;
		} else {
			if (categories.length == 0) {
				iter.stamp = 0;
				return false;
			}

			cat = categories[0];
		}

		iter.stamp = this.timestamp;
		iter.user_data = cat;

		if (path.get_depth () == i + 1) {
			// it's a result
			if (indices[i] >= cat.count) {
				iter.stamp = 0;
				return false;
			}

			iter.user_data2 = &cat.results[indices[i]];
			iter.user_data3 = indices[i].to_pointer ();
		}

		return true;
	}

	public int get_n_columns () {
		return n_columns + n_extra_columns;
	}

#if VALA_0_14
	public Gtk.TreePath? get_path (Gtk.TreeIter iter) {
#else
	public Gtk.TreePath get_path (Gtk.TreeIter iter) {
#endif
		TreePath path = new TreePath ();
		CategoryNode cat;
		int i;

		if (queries.length > 1) {
			for (i = 0; i < categories.length; i++) {
				cat = categories[i];

				if (cat == iter.user_data) {
					path.append_index (i);
					break;
				}
			}
		}

		if (iter.user_data2 != null) {
			path.append_index ((int) (long) iter.user_data3);
		}

		return path;
	}

	private async void fetch_thumbnail (TreeIter iter) {
		GLib.File file;
		GLib.FileInfo info;
		ResultNode *result;
		string thumb_path;
		Gdk.Pixbuf pixbuf = null;

		result = iter.user_data2;

		// Query thumbnail to GIO
		file = GLib.File.new_for_uri (result.values[1]);

		try {
			info = yield file.query_info_async ("thumbnail::path,standard::icon",
			                                    GLib.FileQueryInfoFlags.NONE,
			                                    GLib.Priority.DEFAULT,
			                                    cancellable);
		} catch (GLib.Error ie) {
			if (!cancellable.is_cancelled ()) {
				warning ("Could not get thumbnail: %s", ie.message);
			}
			return;
		}

		thumb_path = info.get_attribute_byte_string ("thumbnail::path");

		try {
			if (thumb_path != null) {
				pixbuf = new Gdk.Pixbuf.from_file_at_size (thumb_path, icon_size, icon_size);
			} else {
				GLib.Icon icon;
				Gtk.IconInfo icon_info;

				icon = (GLib.Icon) info.get_attribute_object ("standard::icon");

				if (icon == null) {
					return;
				}

				var theme = IconTheme.get_for_screen (Gdk.Screen.get_default ());
				icon_info = theme.lookup_by_gicon (icon, icon_size, 0); // Gtk.IconLookupFlags.FORCE_SIZE

				if (icon_info == null) {
					return;
				}

				pixbuf = icon_info.load_icon ();
			}
		} catch (GLib.Error e) {
			warning ("Could not get icon pixbuf: %s\n", e.message);
		}

		if (pixbuf != null) {
			TreePath path;

			result.pixbuf = pixbuf;
			path = get_path (iter);
			row_changed (path, iter);
		}
	}

	public void get_value (Gtk.TreeIter iter, int column, out GLib.Value value) {
		CategoryNode cat;

		value = GLib.Value (this.get_column_type (column));

		if (column >= n_columns + n_extra_columns) {
			return;
		}

		cat = (CategoryNode) iter.user_data;

		if (column == n_columns + 1) {
			// Type column
			value.set_enum (cat.type);
			return;
		}

		if (iter.user_data2 == null) {
			if (column == n_columns) {
				Gdk.Pixbuf pixbuf;

				pixbuf = cat.pixbuf;

				if (pixbuf == null) {
					var theme = IconTheme.get_for_screen (Gdk.Screen.get_default ());
					int size = icon_size;

					switch (cat.type) {
					case Tracker.Query.Type.APPLICATIONS:
						pixbuf = tracker_pixbuf_new_from_name (theme, "package-x-generic", size);
						break;
					case Tracker.Query.Type.MUSIC:
						pixbuf = tracker_pixbuf_new_from_name (theme, "audio-x-generic", size);
						break;
					case Tracker.Query.Type.IMAGES:
						pixbuf = tracker_pixbuf_new_from_name (theme, "image-x-generic", size);
						break;
					case Tracker.Query.Type.VIDEOS:
						pixbuf = tracker_pixbuf_new_from_name (theme, "video-x-generic", size);
						break;
					case Tracker.Query.Type.DOCUMENTS:
						pixbuf = tracker_pixbuf_new_from_name (theme, "x-office-presentation", size);
						break;
					case Tracker.Query.Type.MAIL:
						pixbuf = tracker_pixbuf_new_from_name (theme, "emblem-mail", size);
						break;
					case Tracker.Query.Type.FOLDERS:
						pixbuf = tracker_pixbuf_new_from_name (theme, "folder", size);
						break;
					case Tracker.Query.Type.BOOKMARKS:
						pixbuf = tracker_pixbuf_new_from_name (theme, "web-browser", size);
						break;
					}
				}

				value.set_object (pixbuf);
			}
		} else {
			ResultNode *result;
			int n_node;

			result = iter.user_data2;
			n_node = (int) (long) iter.user_data3;

			if (result.values[0] != null) {
				if (column == n_columns ) {
					if (result.pixbuf != null) {
						value.set_object (result.pixbuf);
					} else if (queries.length == 1) {
						fetch_thumbnail.begin (iter);
					}
				} else {
					value.set_string (result.values[column]);
				}
			} else {
				n_node /= 100;
				n_node *= 100;

				add_operation (cat, n_node);
			}
		}
	}

	public bool iter_children (out Gtk.TreeIter iter, Gtk.TreeIter? parent) {
		CategoryNode cat;

		iter = TreeIter ();

		if (parent == null) {
			if (categories.length == 0) {
				iter.stamp = 0;
				return false;
			}

			if (queries.length > 1) {
				int i;

				i = find_nth_category_index (null, 0);
				cat = categories[i];
				iter.stamp = this.timestamp;
				iter.user_data = cat;
				return true;
			} else {
				iter.stamp = this.timestamp;
				iter.user_data = categories[0];
				iter.user_data2 = &cat.results[0];
				iter.user_data3 = 0.to_pointer ();
				return true;
			}
		}

		if (parent.user_data2 != null) {
			iter.stamp = 0;
			return false;
		}

		cat = (CategoryNode) parent.user_data;

		if (cat.results.length <= 0) {
			iter.stamp = 0;
			return false;
		}

		iter.stamp = this.timestamp;
		iter.user_data = cat;
		iter.user_data2 = &cat.results[0];
		iter.user_data3 = 0.to_pointer ();

		return true;
	}

	public bool iter_has_child (Gtk.TreeIter iter) {
		if (iter.user_data2 == null) {
			CategoryNode cat;

			cat = (CategoryNode) iter.user_data;
			return (cat.count > 0);
		}

		return false;
	}

	public int iter_n_children (Gtk.TreeIter? iter) {
		if (iter == null) {
			if (queries.length > 1) {
				return categories.length - 1;
			} else if (categories.length > 0) {
				return categories[0].count;
			} else {
				return -1;
			}
		}

		if (iter.user_data2 != null) {
			// a result doesn't have children
			return -1;
		}

		CategoryNode cat = (CategoryNode) iter.user_data;

		return cat.count;
	}

	public bool iter_next (ref Gtk.TreeIter iter) {
		CategoryNode cat;
		int i;

		cat = (CategoryNode) iter.user_data;

		if (iter.user_data2 == null) {
			i = find_nth_category_index (cat, 1);

			if (i < 0 || i >= categories.length) {
				iter.stamp = 0;
				return false;
			}

			iter.stamp = this.timestamp;
			iter.user_data = categories[i];

			return true;
		} else {
			// Result node
			i = (int) (long) iter.user_data3;
			i++;

			if (i >= cat.count) {
				iter.stamp = 0;
				return false;
			}

			iter.user_data2 = &cat.results[i];
			iter.user_data3 = i.to_pointer ();

			return true;
		}
	}

	public bool iter_nth_child (out Gtk.TreeIter iter, Gtk.TreeIter? parent, int n) {
		CategoryNode cat;

		iter = TreeIter ();

		if (parent != null) {
			cat = (CategoryNode) parent.user_data;

			if (n >= cat.count) {
				iter.stamp = 0;
				return false;
			}

			iter.stamp = this.timestamp;
			iter.user_data = cat;
			iter.user_data2 = &cat.results[n];
			iter.user_data3 = n.to_pointer ();
			return true;
		} else {
			int index;

			if (queries.length > 1) {
				index = find_nth_category_index (null, n);

				if (index < 0 || index >= categories.length) {
					iter.stamp = 0;
					return false;
				}
			} else {
				index = 0;
			}

			cat = categories[index];
			iter.stamp = this.timestamp;
			iter.user_data = cat;

			if (queries.length > 1) {
				iter.user_data2 = &cat.results[0];
				iter.user_data3 = 0.to_pointer ();
			}

			return true;
		}
	}

	public bool iter_parent (out Gtk.TreeIter iter, Gtk.TreeIter child) {
		iter = TreeIter ();

		if (queries.length > 1 &&
		    child.user_data2 != null) {
			// child within a category
			iter.stamp = this.timestamp;
			iter.user_data = child.user_data;
			iter.user_data2 = null;
			iter.user_data3 = null;
			return true;
		}

		iter.stamp = 0;
		return false;
	}

	public void ref_node (Gtk.TreeIter iter) {
	}

	public void unref_node (Gtk.TreeIter iter) {
	}

	private void theme_changed (IconTheme theme) {
		TreeIter iter;
		int i, j;

		iter = TreeIter ();
		iter.stamp = this.timestamp;

		for (i = 0; i < categories.length; i++) {
			CategoryNode cat = categories[i];

			iter.user_data = cat;

			for (j = cat.count - 1; j >= 0; j--) {
				var result = cat.results[j];

				iter.user_data2 = &cat.results[j];
				iter.user_data3 = j.to_pointer ();

				if (result.pixbuf != null) {
					fetch_thumbnail.begin (iter);
				}
			}
		}
	}

	public ResultStore (int _n_columns) {
		categories = new GenericArray<CategoryNode> ();
		running_operations = new GenericArray<Operation?> ();
		delayed_operations = new GenericArray<Operation?> ();

		n_columns = _n_columns;
		timestamp = 1;
		icon_size = 24; // Default value, overridden by tracker-needle.vala

		var theme = IconTheme.get_for_screen (Gdk.Screen.get_default ());
		theme.changed.connect (theme_changed);
	}

	public void add_query (Tracker.Query.Type type, Tracker.Query.Match match, ...) {
		var l = va_list ();
		string str = null;
		string [] args = null;
		QueryData query_data;

		do {
			str = l.arg ();

			if (str != null) {
				args += str;
			}
		} while (str != null);

		if (args.length != n_columns ) {
			warning ("Arguments and number of columns doesn't match");
			return;
		}

		query_data = QueryData ();
		query_data.type = type;
		query_data.match = match;
		query_data.args = args;

		queries += query_data;
	}

	public bool has_results () {
		return filled_categories_count () > 0;
	}

	public void cancel_search () {
		if (cancellable != null) {
			cancellable.cancel ();
			cancellable = null;
		}

		clear_results ();
	}
}
