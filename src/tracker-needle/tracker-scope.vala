using GLib;

class Tracker.Scope {
	private Unity.Scope scope;

	public Scope() {
		scope = new Unity.Scope("/org/gnome/scope/tracker");
		scope.search_in_global = true;
		scope.notify["active-search"].connect(on_search_changed);
		scope.notify["active-global-search"].connect(on_global_search_changed);
		scope.filters_changed.connect(on_filters_changed);
	}

	public void export() {
		scope.export();
	}

	private void on_search_changed(Object obj, ParamSpec pspec) {
		var search = scope.active_search;

		if (search == null)
			return;

		update_model.begin(search, scope.results_model);
	}

	private void on_global_search_changed(Object obj, ParamSpec pspec) {
		var search = scope.active_global_search;

		if (search == null)
			return;

		update_model.begin(search, scope.global_results_model);
	}

	private void on_filters_changed() {
		var search = scope.active_search;

		if (search == null)
			return;

		update_model.begin(search, scope.results_model);
	}

	private async void update_model(Unity.LensSearch search, Dee.Model model) {
		model.clear();
		scope.freeze_notify();

		Idle.add(() => {
				scope.thaw_notify();
				return false;
			});

		if (false) // Code disabled
		foreach (var filter in scope.filters) {
			print("%s\n", filter.id);
			var fo = filter as Unity.OptionsFilter;
			if (fo != null)
				foreach (var option in fo.options) {
					print("%s %s\n", option.id, option.active ? "active" : "");
				}
		}

		var query = new Tracker.Query();
		query.offset = 0;
		query.limit = 100;
		print("Search string: %s\n", search.search_string);
		query.criteria = search.search_string;

		var type = Tracker.Query.Type.ALL;		
		var type_filter = scope.get_filter("type") as Unity.RadioOptionFilter;
		var type_value = type_filter.get_active_option();
		if (type_value != null) {
			debug("%s: %s\n", type_filter.id, type_value.id);
			switch (type_value.id) {
			case "other": // TODO: support in query engine
				break;
			case "audio":
				type = Tracker.Query.Type.MUSIC;
				break;
			case "folders":
				type = Tracker.Query.Type.FOLDERS;
				break;
			case "documents":
				type = Tracker.Query.Type.DOCUMENTS;
				break;
			case "images":
				type = Tracker.Query.Type.IMAGES;
				break;
			case "presentations": // TODO: support in query engine
				break;
			case "videos":
				type = Tracker.Query.Type.VIDEOS;
				break;
			}
		}

		// TODO: not supported yet
		var modified_filter = scope.get_filter("modified") as Unity.RadioOptionFilter;
		var modified_value = modified_filter.get_active_option();
		if (modified_value != null)
			debug("%s: %s\n", modified_filter.id, modified_value.id);

		// TODO: not supported yet
		var size_filter = scope.get_filter("size") as Unity.MultiRangeFilter;
		var size_smallest = size_filter.get_first_active();
		var size_largest = size_filter.get_last_active();
		if (size_smallest != null && size_largest != null)
			debug("%s: [%s %s]\n", size_filter.id, size_smallest.id, size_largest.id);

		string[] args = {};
		args += "?urn";
		args += "nie:url(?urn)";
		args += "tracker:coalesce(nie:title(?urn), nfo:fileName(?urn))";
		args += "nie:title(?urn)";
		args += "nie:mimeType(?urn)";
		
		var cancellable = new GLib.Cancellable();
		var cursor = yield query.perform_async(type,
											   Tracker.Query.Match.FTS,
											   args, cancellable);
		if (cursor == null) return;
		bool b = yield cursor.next_async(cancellable);

		while (b) {

			var urn = cursor.get_string(0);
			var url = cursor.get_string(1);
			var fileName = cursor.get_string(2);
			var title = cursor.get_string(3);
			var mimeType = cursor.get_string(4);

			var file = File.new_for_uri(url);
			var info = file.query_info("standard::icon", 0, cancellable);
			var icon = info.get_icon().to_string();
			
			model.append(url, // activation uri
						 icon, // icon uri
						 0, // category index
						 mimeType, // mime type
						 fileName, // name
						 "description", // comment
						 url // link uri
				);
			b = yield cursor.next_async(cancellable);
		}
		
		debug("Result size: %d\n", (int)model.get_n_rows());

		search.finished();
	}
}

// Checks if name is already owned in DBus
public static bool dbus_name_has_owner(string name) {
	try {
		bool has_owner;
		DBusConnection bus = Bus.get_sync (BusType.SESSION);
		Variant result = bus.call_sync ("org.freedesktop.DBus",
										"/org/freedesktop/dbus",
										"org.freedesktop.DBus",
										"NameHasOwner",
										new Variant ("(s)", name),
										new VariantType ("(b)"),
										DBusCallFlags.NO_AUTO_START,
										-1);
		result.get ("(b)", out has_owner);
		return has_owner;
	} catch (Error e) {
		warning ("Unable to decide whether '%s' is running: %s", name, e.message);
	}
    
	return false;
}

static int main(string[] args) {
	var dbus_name = "org.gnome.Tracker.Scope";

	if (dbus_name_has_owner(dbus_name)) {
		print("another instance exists!\n");
		return 2; // Another instance is running
	}

	var scope = new Tracker.Scope();
	scope.export();

	var app = new Application(dbus_name, ApplicationFlags.IS_SERVICE);

	try {
		app.register();
	} catch (Error e) {
		return 1;
	}

	if (app.get_is_remote())
		return 2; // another instance is running
		
	app.hold();
	return app.run();
}