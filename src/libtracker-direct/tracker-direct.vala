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

public class Tracker.Direct.Connection : Tracker.Sparql.Connection {
	static int use_count;
	bool initialized;

	public Connection () throws Sparql.Error, IOError, DBusError {
		DBManager.lock ();

		try {
			if (use_count == 0) {
				// make sure that current locale vs db locale are the same,
				// otherwise return an error
				Locale.init ();
				if (DBManager.locale_changed ()) {
					throw new Sparql.Error.INTERNAL ("Locale mismatch, cannot use direct connection");
				}

				uint select_cache_size = 100;
				string env_cache_size = Environment.get_variable ("TRACKER_SPARQL_CACHE_SIZE");

				if (env_cache_size != null) {
					select_cache_size = int.parse (env_cache_size);
				}

				Data.Manager.init (DBManagerFlags.READONLY, null, null, false, false, select_cache_size, 0, null, null);
			}

			use_count++;
			initialized = true;
		} catch (Error e) {
			throw new Sparql.Error.INTERNAL (e.message);
		} finally {
			DBManager.unlock ();
		}
	}

	~Connection () {
		if (!initialized) {
			// use_count did not get increased if initialization failed
			return;
		}

		// Clean up connection
		DBManager.lock ();

		try {
			use_count--;

			if (use_count == 0) {
				Data.Manager.shutdown ();
			}
		} finally {
			DBManager.unlock ();
		}
	}

	Sparql.Cursor query_unlocked (string sparql, Cancellable? cancellable) throws Sparql.Error, IOError, DBusError {
		try {
			var query_object = new Sparql.Query (sparql);
			var cursor = query_object.execute_cursor (true);
			cursor.connection = this;
			return cursor;
		} catch (DBInterfaceError e) {
			throw new Sparql.Error.INTERNAL (e.message);
		} catch (DateError e) {
			throw new Sparql.Error.PARSE (e.message);
		}
	}

	public override Sparql.Cursor query (string sparql, Cancellable? cancellable) throws Sparql.Error, IOError, DBusError {
		DBManager.lock ();
		try {
			return query_unlocked (sparql, cancellable);
		} finally {
			DBManager.unlock ();
		}
	}

	public async override Sparql.Cursor query_async (string sparql, Cancellable? cancellable) throws Sparql.Error, IOError, DBusError {
		if (!DBManager.trylock ()) {
			// run in a separate thread
			Sparql.Error sparql_error = null;
			IOError io_error = null;
			DBusError dbus_error = null;
			Sparql.Cursor result = null;
			var context = MainContext.get_thread_default ();

			g_io_scheduler_push_job (job => {
				try {
					result = query (sparql, cancellable);
				} catch (IOError e_io) {
					io_error = e_io;
				} catch (Sparql.Error e_spql) {
					sparql_error = e_spql;
				} catch (DBusError e_dbus) {
					dbus_error = e_dbus;
				}

				var source = new IdleSource ();
				source.set_callback (() => {
					query_async.callback ();
					return false;
				});
				source.attach (context);

				return false;
			});
			yield;

			if (sparql_error != null) {
				throw sparql_error;
			} else if (io_error != null) {
				throw io_error;
			} else if (dbus_error != null) {
				throw dbus_error;
			} else {
				return result;
			}
		}
		try {
			return query_unlocked (sparql, cancellable);
		} finally {
			DBManager.unlock ();
		}
	}
}
