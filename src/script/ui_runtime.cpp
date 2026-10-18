#include "vb/script/ui_runtime.hpp"

#if !VB_WITH_LUA

// Stub build: scripting compiled out. Every entry point is a no-op,
// mirroring vm.cpp / pack_runtime.cpp's disabled-build pattern.

namespace vb::script {

struct UiRuntime::Impl {};

UiRuntime::UiRuntime(VmLimits) : impl_(nullptr) {}
UiRuntime::~UiRuntime() = default;
UiRuntime::UiRuntime(UiRuntime &&) noexcept = default;
UiRuntime &UiRuntime::operator=(UiRuntime &&) noexcept = default;

ScriptResult UiRuntime::load_pack_file(std::string_view, std::string_view) {
	return { false, core::ScriptError::kDisabled,
		"scripting disabled (built without VB_WITH_LUA)" };
}
void UiRuntime::attach_session(net::ClientSession &) {}
std::vector<std::string> UiRuntime::describe_api() { return {}; }
std::vector<std::string> UiRuntime::global_names() { return {}; }
void UiRuntime::open(std::string_view, std::string_view) {}
void UiRuntime::close() {}
bool UiRuntime::is_open() const { return false; }
void UiRuntime::set_mouse_captured(bool) {}
std::optional<bool> UiRuntime::take_capture_request() { return std::nullopt; }
const std::string &UiRuntime::current_name() const {
	static const std::string kEmpty;
	return kEmpty;
}
const std::vector<Widget> &UiRuntime::render_frame() {
	static const std::vector<Widget> kEmpty;
	return kEmpty;
}
const std::vector<Widget> &UiRuntime::widgets() const {
	static const std::vector<Widget> kEmpty;
	return kEmpty;
}
void UiRuntime::report_click(const std::string &) {}
void UiRuntime::report_change(const std::string &, std::string_view) {}
void UiRuntime::report_list_change(const std::string &, int) {}
void UiRuntime::set_break_progress(std::optional<float>) {}
void UiRuntime::set_screen_size(int, int) {}
void UiRuntime::set_clock(double) {}
void UiRuntime::set_mouse_position(float, float) {}
void UiRuntime::set_player_list(std::string, std::vector<std::string>) {}
void UiRuntime::set_chat(std::vector<std::string>, bool) {}
void UiRuntime::set_inventory(std::vector<InventorySlotView>, int) {}
void UiRuntime::set_player_status(std::optional<StatusView>) {}
void UiRuntime::set_login(std::optional<LoginView>) {}
const std::vector<Widget> &UiRuntime::render_hud() {
	static const std::vector<Widget> kEmpty;
	return kEmpty;
}
void UiRuntime::report_hud_click(const std::string &) {}
void UiRuntime::report_hud_change(const std::string &, std::string_view) {}
void UiRuntime::report_hud_list_change(const std::string &, int) {}

} // namespace vb::script

#else

#include <unordered_map>
#include <utility>

#include <nlohmann/json.hpp>

#include "vb/core/log.hpp"
#include "vb/script/api_surface.hpp"
#include "vb/script/vm_internal.hpp"

namespace vb::script {

namespace {

sol::object json_to_lua(sol::state_view lua, const nlohmann::json &j) {
	switch (j.type()) {
		case nlohmann::json::value_t::null:
			return sol::make_object(lua, sol::lua_nil);
		case nlohmann::json::value_t::boolean:
			return sol::make_object(lua, j.get<bool>());
		case nlohmann::json::value_t::number_integer:
		case nlohmann::json::value_t::number_unsigned:
		case nlohmann::json::value_t::number_float:
			return sol::make_object(lua, j.get<double>());
		case nlohmann::json::value_t::string:
			return sol::make_object(lua, j.get<std::string>());
		case nlohmann::json::value_t::array: {
			sol::table t = lua.create_table();
			int i = 1;
			for (const auto &e : j) {
				t[i++] = json_to_lua(lua, e);
			}
			return t;
		}
		case nlohmann::json::value_t::object: {
			sol::table t = lua.create_table();
			for (const auto &[k, v] : j.items()) {
				t[k] = json_to_lua(lua, v);
			}
			return t;
		}
		default:
			return sol::make_object(lua, sol::lua_nil);
	}
}

nlohmann::json lua_to_json(const sol::object &obj) {
	switch (obj.get_type()) {
		case sol::type::lua_nil:
		case sol::type::none:
			return nullptr;
		case sol::type::boolean:
			return obj.as<bool>();
		case sol::type::number:
			return obj.as<double>();
		case sol::type::string:
			return obj.as<std::string>();
		case sol::type::table: {
			sol::table t = obj.as<sol::table>();
			std::size_t count = 0;
			for (const auto &kv : t) {
				(void)kv;
				++count;
			}
			bool is_array = count > 0;
			for (std::size_t i = 1; i <= count && is_array; ++i) {
				if (!t[i].valid()) {
					is_array = false;
				}
			}
			if (is_array) {
				nlohmann::json arr = nlohmann::json::array();
				for (std::size_t i = 1; i <= count; ++i) {
					arr.push_back(lua_to_json(t[i]));
				}
				return arr;
			}
			nlohmann::json j = nlohmann::json::object();
			for (const auto &kv : t) {
				if (kv.first.is<std::string>()) {
					j[kv.first.as<std::string>()] = lua_to_json(kv.second);
				}
			}
			return j;
		}
		default:
			return nullptr;
	}
}

WidgetType widget_type_from(const std::string &s) {
	if (s == "panel") {
		return WidgetType::kPanel;
	}
	if (s == "button") {
		return WidgetType::kButton;
	}
	if (s == "textbox") {
		return WidgetType::kTextBox;
	}
	if (s == "list") {
		return WidgetType::kList;
	}
	if (s == "rect") {
		return WidgetType::kRect;
	}
	if (s == "text") {
		return WidgetType::kText;
	}
	if (s == "icon") {
		return WidgetType::kIcon;
	}
	if (s == "sprite") {
		return WidgetType::kSprite;
	}
	if (s != "label") {
		VB_WARN("script", "ui: unknown widget type '", s, "', treating as label");
	}
	return WidgetType::kLabel;
}

// Shared by evaluate_frame() (modal screens) and evaluate_hud_frame() (the
// always-on HUD) -- both parse a `{widgets = {...}}` render result the same
// way, just into two separate widget lists/widget_by_id maps.
Widget widget_from_table(const sol::table &wt) {
	Widget w;
	w.id = wt.get_or("id", std::string{});
	w.type = widget_type_from(wt.get_or("type", std::string("label")));
	w.x = wt.get_or("x", 0.0f);
	w.y = wt.get_or("y", 0.0f);
	w.w = wt.get_or("w", 0.0f);
	w.h = wt.get_or("h", 0.0f);
	w.text = wt.get_or("text", std::string{});
	w.list_index = wt.get_or("list_index", -1);
	sol::object items_obj = wt["items"];
	if (items_obj.get_type() == sol::type::table) {
		for (const auto &ikv : items_obj.as<sol::table>()) {
			if (ikv.second.is<std::string>()) {
				w.items.push_back(ikv.second.as<std::string>());
			}
		}
	}
	// kText only: `font_size` (default 16) and `align` ("left"/"center"/
	// "right", default "left" -- see TextAlign's own comment for why this
	// is a raw parameter rather than the engine guessing pixel widths for
	// Lua).
	w.font_size = wt.get_or("font_size", 16);
	// kIcon only: `item`, a registered block/item id -- the same numeric id
	// `player:get_inventory()`/`vb.world.set_block` already use elsewhere.
	w.item = static_cast<core::BlockId>(wt.get_or("item", std::uint16_t{ 0 }));
	// kSprite only: `texture` and an optional `src = {x, y, w, h}` crop.
	w.texture = wt.get_or("texture", std::string{});
	sol::object src_obj = wt["src"];
	if (src_obj.get_type() == sol::type::table) {
		sol::table src = src_obj.as<sol::table>();
		w.src_x = src.get_or(1, 0.0f);
		w.src_y = src.get_or(2, 0.0f);
		w.src_w = src.get_or(3, 0.0f);
		w.src_h = src.get_or(4, 0.0f);
	}
	const std::string align_str = wt.get_or("align", std::string("left"));
	if (align_str == "right") {
		w.align = TextAlign::kRight;
	} else if (align_str == "center") {
		w.align = TextAlign::kCenter;
	} else {
		w.align = TextAlign::kLeft;
	}

	// kRect/kText/kIcon: `color = {r,g,b,a?}` (fill for kRect/kText, tint
	// multiplier for kIcon; default opaque white -- no tint for kIcon) and
	// an optional `border = {r,g,b,a?}` (kRect only; default fully
	// transparent -- no border drawn). 1-indexed like every other Lua color
	// array in this codebase (vb.daynight.set_curve's `color = {r,g,b}`).
	sol::object color_obj = wt["color"];
	if (color_obj.get_type() == sol::type::table) {
		sol::table c = color_obj.as<sol::table>();
		w.fill_r = c.get_or(1, std::uint8_t{ 255 });
		w.fill_g = c.get_or(2, std::uint8_t{ 255 });
		w.fill_b = c.get_or(3, std::uint8_t{ 255 });
		w.fill_a = c.get_or(4, std::uint8_t{ 255 });
	}
	sol::object border_obj = wt["border"];
	if (border_obj.get_type() == sol::type::table) {
		sol::table c = border_obj.as<sol::table>();
		w.border_r = c.get_or(1, std::uint8_t{ 0 });
		w.border_g = c.get_or(2, std::uint8_t{ 0 });
		w.border_b = c.get_or(3, std::uint8_t{ 0 });
		w.border_a = c.get_or(4, std::uint8_t{ 255 });
	}
	return w;
}

} // namespace

struct UiRuntime::Impl {
	Vm vm;
	net::ClientSession *session = nullptr;
	std::unordered_map<std::string, sol::protected_function> layout_fns;

	std::string current_name;
	sol::protected_function current_render_fn;
	sol::table current_state; // persists across every frame this screen is open
	sol::protected_function current_on_close; // refreshed by each evaluate_frame()
	bool current_capture_on_close = false; // layout's capture_mouse_on_close, ditto
	// ui.close() from the screen's own render function can't tear the screen
	// down under the running call; it's recorded here and done once the
	// render returns (with that frame's on_close).
	bool in_render = false;
	bool close_after_render = false;
	std::optional<bool> close_after_render_capture;
	// Mouse capture: what the client reports (client.mouse_captured()), and
	// the latest request from client.capture_mouse / ui.close{capture_mouse}
	// / capture_mouse_on_close, taken by the client each frame.
	bool mouse_captured = false;
	std::optional<bool> capture_request;
	std::unordered_map<std::string, sol::table> widget_by_id;
	std::vector<Widget> widgets_vec;
	std::string current_widget_id; // scratch, valid during a callback

	// HUD (always-on overlay, independent of open()/close() above): a
	// single registered render_fn with its own persistent state table --
	// there's only ever one HUD, unlike the named-screen registry
	// (layout_fns) modal screens use.
	sol::protected_function hud_render_fn;
	sol::table hud_state;
	std::vector<Widget> hud_widgets_vec;
	std::unordered_map<std::string, sol::table> hud_widget_by_id;
	// Set just before invoking a widget's on_click/on_change callback (either
	// map above), read by send_event() so a HUD widget's ui.send_event(...)
	// carries ui_name = "hud" instead of whatever modal screen (or none) is
	// separately open.
	bool current_widget_is_hud = false;
	// Raw engine state a HUD's render_fn can read via client.break_progress()
	// -- nullopt when the player isn't currently breaking anything. Set once
	// per frame by src/client/main.cpp's own input-handling code, which
	// already computes this; the engine never draws it itself.
	std::optional<float> break_progress;
	// Window size a HUD's render_fn can read via client.screen_size() --
	// widgets take absolute pixel positions, so centering anything needs
	// this. Zero until the first set_screen_size() call.
	int screen_w = 0;
	double clock_seconds = 0.0;
	float mouse_x = 0.0f;
	float mouse_y = 0.0f;
	int screen_h = 0;

	// Player list / chat / hotbar raw state (see the header's own comments
	// on the set_* calls below) -- read via client.player_name()/
	// client.players()/client.chat_log()/client.chat_open()/
	// client.inventory()/client.selected_slot().
	std::string own_player_name;
	std::vector<std::string> other_player_names;
	std::vector<std::string> chat_log;
	bool chat_open = false;
	std::vector<UiRuntime::InventorySlotView> inventory;
	int selected_slot = 1;
	std::optional<UiRuntime::StatusView> player_status;
	// state.login (set_login): a read-only proxy table, or nil.
	std::optional<UiRuntime::LoginView> login;
	sol::object login_table;

	explicit Impl(VmLimits limits);
	void put_login(sol::table &state);

	sol::state &lua_state() { return vm.native_impl().lua; }
	void install_bindings();
	void send_event(const std::string &kind, const sol::object &value);
	void run_callback(const std::string &widget_id, const char *field,
			const sol::object &arg, bool has_arg,
			std::unordered_map<std::string, sol::table> &widget_map, bool is_hud);
	void evaluate_frame();
	void evaluate_hud_frame();
	void do_close(std::optional<bool> capture = std::nullopt);
};

UiRuntime::Impl::Impl(VmLimits limits) : vm(limits) {
	install_bindings();
	hud_state = lua_state().create_table();
	login_table = sol::make_object(lua_state(), sol::lua_nil);
}

void UiRuntime::Impl::put_login(sol::table &state) {
	if (state.valid()) {
		state["login"] = login_table;
	}
}

void UiRuntime::Impl::send_event(const std::string &kind, const sol::object &value) {
	if (session == nullptr) {
		return;
	}
	protocol::C2SUiEvent e;
	e.ui_name = current_widget_is_hud ? "hud" : current_name;
	e.widget_id = current_widget_id;
	e.event_kind = kind;
	e.value_json = lua_to_json(value).dump();
	session->send_ui_event(e);
}

void UiRuntime::Impl::install_bindings() {
	sol::state &lua = lua_state();
	sol::table ui = lua.create_named_table("ui");

	ui["define"] = [this](const std::string &name, sol::protected_function fn) {
		layout_fns[name] = std::move(fn);
	};

	// Registers the single always-on HUD render function (see the header's
	// "HUD" section) -- distinct from ui.define's named-screen registry
	// above since a HUD isn't opened/closed, just always active.
	ui["define_hud"] = [this](sol::protected_function fn) {
		hud_render_fn = std::move(fn);
	};

	ui["send_event"] = [this](const std::string &kind, sol::object value) {
		send_event(kind, value);
	};

	// ui.close() / ui.close{ capture_mouse = true }: the option asks the
	// client to recapture the mouse once nothing else needs a cursor.
	ui["close"] = [this](sol::optional<sol::table> opts) {
		std::optional<bool> capture;
		if (opts) {
			const sol::object c = (*opts)["capture_mouse"];
			if (c.is<bool>()) {
				capture = c.as<bool>();
			}
		}
		if (in_render) {
			close_after_render = true;
			if (capture) {
				close_after_render_capture = capture;
			}
			return;
		}
		do_close(capture);
	};

	// Raw, engine-computed local client state a HUD (or any Lua UI) can
	// query for presentation -- "engine provides raw state, Lua decides how
	// to show it": nothing here draws a pixel, it's read-only data.
	sol::table client_tbl = lua.create_named_table("client");
	client_tbl["break_progress"] = [this]() -> sol::object {
		if (!break_progress) {
			return sol::make_object(lua_state(), sol::lua_nil);
		}
		return sol::make_object(lua_state(), *break_progress);
	};
	client_tbl["screen_size"] = [this]() -> sol::table {
		sol::table t = lua_state().create_table();
		t["width"] = screen_w;
		t["height"] = screen_h;
		return t;
	};
	client_tbl["time"] = [this]() -> double { return clock_seconds; };
	client_tbl["mouse_position"] = [this]() -> sol::table {
		sol::table t = lua_state().create_table();
		t["x"] = mouse_x;
		t["y"] = mouse_y;
		return t;
	};
	client_tbl["player_name"] = [this]() -> std::string { return own_player_name; };
	client_tbl["players"] = [this]() -> sol::table {
		sol::table t = lua_state().create_table();
		int i = 1;
		for (const auto &name : other_player_names) {
			t[i++] = name;
		}
		return t;
	};
	client_tbl["chat_log"] = [this]() -> sol::table {
		sol::table t = lua_state().create_table();
		int i = 1;
		for (const auto &line : chat_log) {
			t[i++] = line;
		}
		return t;
	};
	client_tbl["chat_open"] = [this]() -> bool { return chat_open; };
	// Asks the client to capture (true) or release (false) the mouse. A
	// capture waits until no screen and no chat box are open; the last
	// request wins.
	client_tbl["capture_mouse"] = [this](bool on) { capture_request = on; };
	client_tbl["mouse_captured"] = [this]() -> bool { return mouse_captured; };
	client_tbl["inventory"] = [this]() -> sol::table {
		sol::table t = lua_state().create_table();
		int i = 1;
		for (const auto &slot : inventory) {
			sol::table entry = lua_state().create_table();
			entry["name"] = slot.name;
			entry["count"] = slot.count;
			entry["item"] = slot.item;
			t[i++] = entry;
		}
		return t;
	};
	client_tbl["selected_slot"] = [this]() -> int { return selected_slot; };
	// The local player's own appearance (player:set_visual_override on the
	// server), or nil when the pack set none: {texture=, frame_width=,
	// frame_height=, facings=, layers = {{texture=, below=, rows=,
	// tint={r,g,b,a}}, ...}}, with only the fields the override sets. Feed
	// `texture`/`layers[i].texture` to `sprite` widgets for a preview.
	client_tbl["my_appearance"] = [this]() -> sol::object {
		const protocol::EntityVisualOverride *ov =
				session != nullptr ? session->my_visual_override() : nullptr;
		if (ov == nullptr) {
			return sol::make_object(lua_state(), sol::lua_nil);
		}
		sol::table t = lua_state().create_table();
		if (ov->texture) {
			t["texture"] = *ov->texture;
		}
		if (ov->frame_width) {
			t["frame_width"] = *ov->frame_width;
		}
		if (ov->frame_height) {
			t["frame_height"] = *ov->frame_height;
		}
		if (ov->facings) {
			t["facings"] = *ov->facings;
		}
		if (ov->layers) {
			sol::table layers = lua_state().create_table();
			int i = 1;
			for (const auto &l : *ov->layers) {
				sol::table lt = lua_state().create_table();
				lt["texture"] = l.texture;
				lt["below"] = l.below;
				lt["rows"] = l.rows;
				lt["tint"] = lua_state().create_table_with(1, l.tint[0], 2, l.tint[1], 3, l.tint[2], 4, l.tint[3]);
				layers[i++] = lt;
			}
			t["layers"] = layers;
		}
		return t;
	};
	// {current=, max=} each, or nil before the server's first status arrives.
	client_tbl["health"] = [this]() -> sol::object {
		if (!player_status) {
			return sol::make_object(lua_state(), sol::lua_nil);
		}
		sol::table t = lua_state().create_table();
		t["current"] = player_status->health;
		t["max"] = player_status->max_health;
		return t;
	};
	client_tbl["hunger"] = [this]() -> sol::object {
		if (!player_status) {
			return sol::make_object(lua_state(), sol::lua_nil);
		}
		sol::table t = lua_state().create_table();
		t["current"] = player_status->hunger;
		t["max"] = player_status->max_hunger;
		return t;
	};
}

void UiRuntime::Impl::evaluate_frame() {
	if (current_name.empty() || !current_render_fn.valid()) {
		return;
	}
	vm.begin_call_budget();
	close_after_render = false;
	close_after_render_capture.reset();
	in_render = true;
	put_login(current_state);
	sol::protected_function_result r = current_render_fn(current_state);
	in_render = false;
	// A ui.close() during the render, done now that the call has returned.
	const auto finish_close = [this]() {
		if (!close_after_render) {
			return false;
		}
		close_after_render = false;
		do_close(std::exchange(close_after_render_capture, std::nullopt));
		return true;
	};
	if (!r.valid()) {
		const sol::error e = r;
		VB_WARN("script", "ui '", current_name, "' render function error: ", e.what());
		finish_close();
		return; // leave the previous frame's widgets in place, don't flicker
	}
	sol::object result = r;
	sol::table layout;
	if (result.get_type() == sol::type::table) {
		layout = result.as<sol::table>();
		sol::object on_close = layout["on_close"];
		current_on_close = on_close.is<sol::protected_function>()
				? on_close.as<sol::protected_function>()
				: sol::protected_function();
		const sol::object capture_on_close = layout["capture_mouse_on_close"];
		current_capture_on_close = capture_on_close.is<bool>() && capture_on_close.as<bool>();
	}
	if (finish_close()) {
		return;
	}
	if (result.get_type() != sol::type::table) {
		VB_WARN("script", "ui '", current_name,
				"' render function did not return a table");
		return;
	}
	sol::object widgets_obj = layout["widgets"];
	if (widgets_obj.get_type() != sol::type::table) {
		VB_WARN("script", "ui '", current_name, "' render result has no 'widgets' array");
		return;
	}
	sol::table widget_tables = widgets_obj.as<sol::table>();

	widget_by_id.clear();
	widgets_vec.clear();
	for (const auto &kv : widget_tables) {
		sol::object entry = kv.second;
		if (entry.get_type() != sol::type::table) {
			continue;
		}
		sol::table wt = entry.as<sol::table>();
		Widget w = widget_from_table(wt);
		if (!w.id.empty()) {
			widget_by_id[w.id] = wt;
		}
		widgets_vec.push_back(std::move(w));
	}
}

void UiRuntime::Impl::evaluate_hud_frame() {
	if (!hud_render_fn.valid()) {
		return;
	}
	vm.begin_call_budget();
	put_login(hud_state);
	sol::protected_function_result r = hud_render_fn(hud_state);
	if (!r.valid()) {
		const sol::error e = r;
		VB_WARN("script", "ui hud render function error: ", e.what());
		return; // leave the previous frame's widgets in place, don't flicker
	}
	sol::object result = r;
	if (result.get_type() != sol::type::table) {
		VB_WARN("script", "ui hud render function did not return a table");
		return;
	}
	sol::table layout = result.as<sol::table>();
	sol::object widgets_obj = layout["widgets"];
	if (widgets_obj.get_type() != sol::type::table) {
		VB_WARN("script", "ui hud render result has no 'widgets' array");
		return;
	}
	sol::table widget_tables = widgets_obj.as<sol::table>();

	hud_widget_by_id.clear();
	hud_widgets_vec.clear();
	for (const auto &kv : widget_tables) {
		sol::object entry = kv.second;
		if (entry.get_type() != sol::type::table) {
			continue;
		}
		sol::table wt = entry.as<sol::table>();
		Widget w = widget_from_table(wt);
		if (!w.id.empty()) {
			hud_widget_by_id[w.id] = wt;
		}
		hud_widgets_vec.push_back(std::move(w));
	}
}

void UiRuntime::Impl::do_close(std::optional<bool> capture) {
	if (current_name.empty()) {
		return;
	}
	// The screen's own default first, then the caller's explicit choice; the
	// on_close handler below can still override both via client.capture_mouse.
	if (current_capture_on_close) {
		capture_request = true;
	}
	if (capture) {
		capture_request = *capture;
	}
	// A real nil object, built on this VM's state: a bare sol::lua_nil converts to a
	// state-less reference, and lua_to_json() then dereferences a null lua_State
	// once a session is attached (found by the e2e inventory-screen test).
	send_event("close", sol::make_object(lua_state(), sol::lua_nil));
	if (current_on_close.valid()) {
		vm.begin_call_budget();
		sol::protected_function_result r = current_on_close();
		if (!r.valid()) {
			const sol::error e = r;
			VB_WARN("script", "ui on_close handler error: ", e.what());
		}
	}
	current_name.clear();
	current_render_fn = sol::protected_function();
	current_state = sol::lua_nil;
	current_on_close = sol::protected_function();
	current_capture_on_close = false;
	widget_by_id.clear();
	widgets_vec.clear();
}

void UiRuntime::Impl::run_callback(const std::string &widget_id, const char *field,
		const sol::object &arg, bool has_arg,
		std::unordered_map<std::string, sol::table> &widget_map, bool is_hud) {
	auto it = widget_map.find(widget_id);
	if (it == widget_map.end()) {
		return;
	}
	sol::object cb_obj = it->second[field];
	if (!cb_obj.is<sol::protected_function>()) {
		return;
	}
	sol::protected_function cb = cb_obj.as<sol::protected_function>();
	current_widget_id = widget_id;
	current_widget_is_hud = is_hud;
	vm.begin_call_budget();
	sol::protected_function_result r = has_arg ? cb(arg) : cb();
	if (!r.valid()) {
		const sol::error e = r;
		VB_WARN("script", "ui widget '", widget_id, "' ", field,
				" handler error: ", e.what());
	}
}

UiRuntime::UiRuntime(VmLimits limits) : impl_(std::make_unique<Impl>(limits)) {}
UiRuntime::~UiRuntime() = default;
UiRuntime::UiRuntime(UiRuntime &&) noexcept = default;
UiRuntime &UiRuntime::operator=(UiRuntime &&) noexcept = default;

ScriptResult UiRuntime::load_pack_file(std::string_view code,
		std::string_view chunk_name) {
	return impl_->vm.do_string(code, chunk_name);
}

void UiRuntime::attach_session(net::ClientSession &session) {
	impl_->session = &session;
}

std::vector<std::string> UiRuntime::global_names() {
	return list_lua_globals(impl_->vm);
}

std::vector<std::string> UiRuntime::describe_api() {
	return describe_lua_surface(impl_->vm, { "ui", "client" });
}

void UiRuntime::open(std::string_view name, std::string_view ctx_json) {
	auto it = impl_->layout_fns.find(std::string(name));
	if (it == impl_->layout_fns.end()) {
		VB_WARN("script", "ui.open: '", name, "' was never defined via ui.define");
		return;
	}

	nlohmann::json parsed;
	try {
		parsed = ctx_json.empty() ? nlohmann::json::object()
								  : nlohmann::json::parse(ctx_json);
	} catch (const nlohmann::json::parse_error &) {
		parsed = nlohmann::json::object();
	}

	sol::object ctx_obj = json_to_lua(impl_->lua_state(), parsed);
	sol::table state = ctx_obj.get_type() == sol::type::table
			? ctx_obj.as<sol::table>()
			: impl_->lua_state().create_table();

	impl_->current_name = std::string(name);
	impl_->current_render_fn = it->second;
	impl_->current_state = state;
	impl_->current_on_close = sol::protected_function();
	impl_->widget_by_id.clear();
	impl_->widgets_vec.clear();
}

void UiRuntime::close() { impl_->do_close(); }

void UiRuntime::set_mouse_captured(bool captured) { impl_->mouse_captured = captured; }

std::optional<bool> UiRuntime::take_capture_request() {
	return std::exchange(impl_->capture_request, std::nullopt);
}

bool UiRuntime::is_open() const { return !impl_->current_name.empty(); }
const std::string &UiRuntime::current_name() const { return impl_->current_name; }

const std::vector<Widget> &UiRuntime::render_frame() {
	impl_->evaluate_frame();
	return impl_->widgets_vec;
}

const std::vector<Widget> &UiRuntime::widgets() const { return impl_->widgets_vec; }

void UiRuntime::report_click(const std::string &widget_id) {
	impl_->run_callback(widget_id, "on_click", sol::lua_nil, false,
			impl_->widget_by_id, false);
}

void UiRuntime::report_change(const std::string &widget_id,
		std::string_view text_value) {
	sol::object v = sol::make_object(impl_->lua_state(), std::string(text_value));
	impl_->run_callback(widget_id, "on_change", v, true, impl_->widget_by_id, false);
}

void UiRuntime::report_list_change(const std::string &widget_id, int new_index) {
	sol::object v = sol::make_object(impl_->lua_state(), new_index);
	impl_->run_callback(widget_id, "on_change", v, true, impl_->widget_by_id, false);
}

void UiRuntime::report_hud_click(const std::string &widget_id) {
	impl_->run_callback(widget_id, "on_click", sol::lua_nil, false,
			impl_->hud_widget_by_id, true);
}

void UiRuntime::report_hud_change(const std::string &widget_id,
		std::string_view text_value) {
	sol::object v = sol::make_object(impl_->lua_state(), std::string(text_value));
	impl_->run_callback(widget_id, "on_change", v, true, impl_->hud_widget_by_id, true);
}

void UiRuntime::report_hud_list_change(const std::string &widget_id, int new_index) {
	sol::object v = sol::make_object(impl_->lua_state(), new_index);
	impl_->run_callback(widget_id, "on_change", v, true, impl_->hud_widget_by_id, true);
}

void UiRuntime::set_break_progress(std::optional<float> fraction) {
	impl_->break_progress = fraction;
}

void UiRuntime::set_clock(double seconds) {
	impl_->clock_seconds = seconds;
}

void UiRuntime::set_mouse_position(float x, float y) {
	impl_->mouse_x = x;
	impl_->mouse_y = y;
}

void UiRuntime::set_screen_size(int width, int height) {
	impl_->screen_w = width;
	impl_->screen_h = height;
}

void UiRuntime::set_player_list(std::string own_name, std::vector<std::string> other_names) {
	impl_->own_player_name = std::move(own_name);
	impl_->other_player_names = std::move(other_names);
}

void UiRuntime::set_chat(std::vector<std::string> log, bool chat_open) {
	impl_->chat_log = std::move(log);
	impl_->chat_open = chat_open;
}

void UiRuntime::set_player_status(std::optional<StatusView> status) {
	impl_->player_status = status;
}

void UiRuntime::set_login(std::optional<LoginView> login) {
	if (login.has_value() == impl_->login.has_value() &&
			(!login || (login->name == impl_->login->name && login->subject == impl_->login->subject))) {
		return; // unchanged: keep the same table, so scripts can compare it
	}
	impl_->login = std::move(login);
	sol::state &lua = impl_->lua_state();
	if (!impl_->login) {
		impl_->login_table = sol::make_object(lua, sol::lua_nil);
		return;
	}
	// A proxy: reads go to the data table, writes raise an error.
	sol::table data = lua.create_table_with("name", impl_->login->name, "subject", impl_->login->subject);
	sol::table meta = lua.create_table();
	meta["__index"] = data;
	meta["__newindex"] = [](sol::this_state, sol::object, sol::object, sol::object) {
		throw sol::error("state.login is read-only");
	};
	meta["__metatable"] = false;
	sol::table proxy = lua.create_table();
	proxy[sol::metatable_key] = meta;
	impl_->login_table = proxy;
}

void UiRuntime::set_inventory(std::vector<InventorySlotView> slots, int selected_slot) {
	impl_->inventory = std::move(slots);
	impl_->selected_slot = selected_slot;
}

const std::vector<Widget> &UiRuntime::render_hud() {
	impl_->evaluate_hud_frame();
	return impl_->hud_widgets_vec;
}

} // namespace vb::script

#endif // VB_WITH_LUA
