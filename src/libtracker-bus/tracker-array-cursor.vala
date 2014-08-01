/*
 * Copyright (C) 2010, Nokia <ivan.frade@nokia.com>
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

internal class Tracker.Bus.ArrayCursor : Tracker.Sparql.Cursor {
	int rows;
	int current_row = -1;
	string[,] results;
	string[] var_names;
	Sparql.ValueType[] types;
	int cols;

	public ArrayCursor (owned string[,] results, int rows, int cols, string[] var_names, Sparql.ValueType[] types) {
		this.rows = rows;
		this.cols = cols;
		this.results = (owned) results;
		this.types = types;
		this.var_names = var_names;
	}

	public override int n_columns { get { return cols; } }

	public override Sparql.ValueType get_value_type (int column)
	requires (current_row >= 0) {
		return this.types[column];
	}

	public override unowned string? get_variable_name (int column)
	requires (current_row >= 0) {
		return this.var_names[column];
	}

	public override unowned string? get_string (int column, out long length = null)
	requires (current_row >= 0) {
		unowned string str;

		str = results[current_row, column];

		length = str.length;

		return str;
	}

	public override bool next (Cancellable? cancellable = null) throws GLib.Error {
		if (cancellable != null && cancellable.is_cancelled ()) {
			throw new IOError.CANCELLED ("Operation was cancelled");
		}

		if (current_row >= rows - 1) {
			return false;
		}
		current_row++;
		return true;
	}

	public override async bool next_async (Cancellable? cancellable = null) throws GLib.Error {
		/* This cursor isn't blocking, it's fine to just call next here */
		return next (cancellable);
	}

	public override void rewind () {
		current_row = 0;
	}
}
