// Phase C3 (docs/content-base-testing.md): Layer 2 coverage of
// content/base/ui/*.lua -- loaded into a real UiRuntime exactly as
// src/client/client_app.cpp's singleplayer path does (read straight off
// disk, sorted by filename, chunk-named "ui/<file>"), closing the "no test
// loads these files at all" gap the design doc calls out: before this file,
// a syntax error in hud.lua would pass the entire suite.

#include <doctest/doctest.h>

#include "content_base_fixture.hpp"

#if VB_WITH_LUA

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "vb/script/ui_runtime.hpp"

using vb::script::UiRuntime;
using vb::script::Widget;
using vb::script::WidgetType;

namespace {

// Mirrors client_app.cpp's singleplayer ui_sources loop: every *.lua file
// directly under content/base/ui/, chunk-named "ui/<filename>". Sorted so
// the test is deterministic regardless of the OS's directory_iterator order
// (client_app.cpp itself doesn't sort, but load order doesn't matter there
// either -- each file only ever calls ui.define/ui.define_hud for its own
// distinct name).
UiRuntime load_base_ui() {
	UiRuntime ui;
	const auto ui_dir = vb::test::content_base_dir() / "ui";
	std::vector<std::filesystem::path> files;
	for (const auto &entry : std::filesystem::directory_iterator(ui_dir)) {
		if (entry.path().extension() == ".lua") {
			files.push_back(entry.path());
		}
	}
	std::sort(files.begin(), files.end());
	for (const auto &path : files) {
		std::ifstream f(path, std::ios::binary);
		REQUIRE_MESSAGE(f, path.string());
		std::ostringstream ss;
		ss << f.rdbuf();
		const auto result = ui.load_pack_file(ss.str(), "ui/" + path.filename().string());
		REQUIRE_MESSAGE(result, path.string() << ": " << result.message);
	}
	return ui;
}

const Widget *find(const std::vector<Widget> &widgets, const std::string &id) {
	for (const auto &w : widgets) {
		if (w.id == id) {
			return &w;
		}
	}
	return nullptr;
}

} // namespace

TEST_CASE("content/base/ui: every file parses and registers its screen/HUD") {
	// The one test that stops a syntax error in any ui/*.lua file -- a
	// broken hud.lua, pause.lua, or inventory.lua fails load_base_ui()'s
	// REQUIRE above before this case's own body even runs.
	UiRuntime ui = load_base_ui();

	ui.open("base:pause", "{}");
	CHECK(ui.is_open());
	ui.close();

	ui.open("base:inventory", R"({"slots":[]})");
	CHECK(ui.is_open());
	ui.close();

	// define_hud() has no "is defined" query of its own -- render_hud()
	// returning a non-empty list (hotbar/player-list/chat content below
	// always draws at least the player-list header) is the only
	// observable proof hud.lua's ui.define_hud call actually ran.
	ui.set_player_list("A", {});
	CHECK_FALSE(ui.render_hud().empty());
}

TEST_CASE("content/base/ui: base:pause renders a title and a working Resume button") {
	UiRuntime ui = load_base_ui();
	ui.set_screen_size(1280, 720);
	ui.open("base:pause", "{}");
	REQUIRE(ui.is_open());

	const auto &widgets = ui.render_frame();
	const Widget *title = find(widgets, "title");
	REQUIRE(title != nullptr);
	CHECK(title->type == WidgetType::kText);
	CHECK(title->text == "Paused");

	const Widget *resume = find(widgets, "resume");
	REQUIRE(resume != nullptr);
	CHECK(resume->type == WidgetType::kButton);
	CHECK(resume->text == "Resume");
	CHECK(find(widgets, "signed_in") == nullptr); // no auth.lua, no login

	// U0: client.* is callable from a modal render (not just the HUD), and
	// the panel + backdrop are centered on the screen size.
	const Widget *backdrop = find(widgets, "backdrop");
	const Widget *panel = find(widgets, "panel");
	REQUIRE(backdrop != nullptr);
	REQUIRE(panel != nullptr);
	CHECK(backdrop->w == 1280);
	CHECK(backdrop->h == 720);
	CHECK(panel->x + panel->w / 2 == doctest::Approx(640).epsilon(0.01));
	CHECK(panel->y + panel->h / 2 == doctest::Approx(360).epsilon(0.01));
	CHECK(resume->x >= panel->x);
	CHECK(resume->x + resume->w <= panel->x + panel->w);

	// With a login (state.login), a "Signed in as" line. Last: render_frame() rebuilds the list
	// the pointers above point into.
	ui.set_login(UiRuntime::LoginView{ "alex#2", "sub-2" });
	const Widget *signed_in = find(ui.render_frame(), "signed_in");
	REQUIRE(signed_in != nullptr);
	CHECK(signed_in->text == "Signed in as alex#2");

	ui.report_click("resume");
	CHECK_FALSE(ui.is_open());
}

TEST_CASE("content/base/ui: base:inventory panel and backdrop are centered at any "
		"screen size, and highlight the selected slot") {
	UiRuntime ui = load_base_ui();
	const int sizes[][2] = { { 1280, 720 }, { 800, 600 } };
	for (const auto &sz : sizes) {
		ui.set_screen_size(sz[0], sz[1]);
		ui.set_inventory({ { "base:stone", 5, 2 }, { "base:planks", 2, 3 } }, /*selected_slot=*/2);
		ui.open("base:inventory", R"({"slots":[{"item":2,"count":5},{"item":3,"count":1}]})");
		REQUIRE(ui.is_open());
		const auto &widgets = ui.render_frame();

		const Widget *backdrop = find(widgets, "backdrop");
		const Widget *panel = find(widgets, "panel");
		REQUIRE(backdrop != nullptr);
		REQUIRE(panel != nullptr);
		CHECK(backdrop->w == sz[0]);
		CHECK(backdrop->h == sz[1]);
		CHECK(std::abs((panel->x + panel->w / 2.0f) - sz[0] / 2.0f) <= 1.0f);
		CHECK(std::abs((panel->y + panel->h / 2.0f) - sz[1] / 2.0f) <= 1.0f);

		// Panel is listed before the slots so it draws underneath them.
		const auto idx = [&](const std::string &id) {
			for (size_t i = 0; i < widgets.size(); ++i) {
				if (widgets[i].id == id) {
					return static_cast<int>(i);
				}
			}
			return -1;
		};
		CHECK(idx("backdrop") < idx("panel"));
		CHECK(idx("panel") < idx("slot_bg_1"));

		// Slots and Close sit inside the panel.
		const Widget *slot1 = find(widgets, "slot_bg_1");
		const Widget *close = find(widgets, "close");
		REQUIRE(slot1 != nullptr);
		REQUIRE(close != nullptr);
		CHECK(slot1->x >= panel->x);
		CHECK(close->y + close->h <= panel->y + panel->h);
		CHECK(std::abs((close->x + close->w / 2.0f) - (panel->x + panel->w / 2.0f)) <= 1.0f);

		// Selected slot (2) is outlined differently from slot 1.
		const Widget *slot2 = find(widgets, "slot_bg_2");
		REQUIRE(slot2 != nullptr);
		CHECK(slot1->border_g != slot2->border_g);

		ui.close();
	}
}

TEST_CASE("content/base/ui: base:inventory renders one row of widgets per slot, "
		"or an empty-state label with none") {
	UiRuntime ui = load_base_ui();
	ui.set_screen_size(1280, 720);

	ui.open("base:inventory",
			R"({"slots":[{"item":2,"count":5},{"item":3,"count":1},{"item":4,"count":9}]})");
	REQUIRE(ui.is_open());
	{
		const auto &widgets = ui.render_frame();
		for (int i = 1; i <= 3; ++i) {
			const std::string n = std::to_string(i);
			REQUIRE_MESSAGE(find(widgets, "slot_bg_" + n) != nullptr, n);
			REQUIRE_MESSAGE(find(widgets, "slot_icon_" + n) != nullptr, n);
			REQUIRE_MESSAGE(find(widgets, "slot_count_" + n) != nullptr, n);
		}
		// ctx_json's numbers parse as Lua floats; the screen formats counts
		// with %d so a snapshot-supplied "5.0" still shows as "5".
		CHECK(find(widgets, "slot_count_1")->text == "5");
		CHECK(find(widgets, "empty") == nullptr);
		REQUIRE(find(widgets, "close") != nullptr);
	}
	ui.report_click("close");
	CHECK_FALSE(ui.is_open());

	ui.open("base:inventory", R"({"slots":[]})");
	REQUIRE(ui.is_open());
	{
		const auto &widgets = ui.render_frame();
		CHECK(find(widgets, "slot_bg_1") == nullptr);
		REQUIRE(find(widgets, "empty") != nullptr);
		CHECK(find(widgets, "empty")->text == "(empty)");
	}
}

TEST_CASE("content/base/ui: the HUD renders the hotbar, chat log, player list, "
		"and break-progress bar from engine-supplied state") {
	UiRuntime ui = load_base_ui();
	ui.set_screen_size(1280, 720);

	ui.set_player_list("Alice", { "Bob", "Carol" });
	ui.set_chat({ "Alice: hi", "Bob: hey" }, false);
	ui.set_inventory({ { "base:stone", 5, 2 }, { "base:planks", 2, 3 }, { "air", 0, 0 } },
			/*selected_slot=*/1);
	ui.set_break_progress(0.25f);

	const auto &widgets = ui.render_hud();

	// Player list: header + self + each other name.
	REQUIRE(find(widgets, "player_list_header") != nullptr);
	CHECK(find(widgets, "player_list_header")->text == "players (3)");
	REQUIRE(find(widgets, "player_list_self") != nullptr);
	CHECK(find(widgets, "player_list_self")->text == "Alice");
	CHECK(find(widgets, "player_list_1")->text == "Bob");
	CHECK(find(widgets, "player_list_2")->text == "Carol");

	// Chat log: one text widget per line.
	REQUIRE(find(widgets, "chat_log_1") != nullptr);
	CHECK(find(widgets, "chat_log_1")->text == "Alice: hi");
	CHECK(find(widgets, "chat_log_2")->text == "Bob: hey");

	// Hotbar: a container, one bg per slot (selected slot's outlined
	// differently), and an icon + count only for non-empty slots.
	REQUIRE(find(widgets, "hotbar_container") != nullptr);
	REQUIRE(find(widgets, "hotbar_bg_1") != nullptr);
	CHECK(find(widgets, "hotbar_bg_1")->border_g != find(widgets, "hotbar_bg_2")->border_g);
	REQUIRE(find(widgets, "hotbar_icon_1") != nullptr);
	CHECK(find(widgets, "hotbar_icon_1")->item == vb::core::BlockId{ 2 });
	CHECK(find(widgets, "hotbar_count_1")->text == "5");
	CHECK(find(widgets, "hotbar_icon_2")->item == vb::core::BlockId{ 3 });
	REQUIRE(find(widgets, "hotbar_bg_3") != nullptr);
	CHECK(find(widgets, "hotbar_icon_3") == nullptr); // empty slot: bg only
	CHECK(find(widgets, "hotbar_count_3") == nullptr);

	// Break-progress bar: fill width tracks the fraction.
	const Widget *bg = find(widgets, "break_progress_bg");
	const Widget *fill = find(widgets, "break_progress_fill");
	REQUIRE(bg != nullptr);
	REQUIRE(fill != nullptr);
	CHECK(fill->w == doctest::Approx(bg->w * 0.25f));

	// No break in progress -> no bar at all.
	ui.set_break_progress(std::nullopt);
	const auto &widgets2 = ui.render_hud();
	CHECK(find(widgets2, "break_progress_bg") == nullptr);
	CHECK(find(widgets2, "break_progress_fill") == nullptr);
}

TEST_CASE("content/base/ui: base:hud draws health and hunger bars above the "
		"hotbar, scaled and colored by value, and nothing before status arrives") {
	UiRuntime ui = load_base_ui();
	ui.set_screen_size(1280, 720);
	ui.set_inventory({ { "base:stone", 5, 2 } }, 1);

	// No S2C_PlayerStatus yet -> no bars.
	{
		const auto &w = ui.render_hud();
		CHECK(find(w, "health_bg") == nullptr);
		CHECK(find(w, "hunger_bg") == nullptr);
	}

	ui.set_player_status(UiRuntime::StatusView{ 20.0f, 20.0f, 100.0f, 100.0f });
	const auto full = ui.render_hud();
	const Widget *bg = find(full, "health_bg");
	const Widget *fill = find(full, "health_fill");
	const Widget *hotbar = find(full, "hotbar_bg_1");
	REQUIRE(bg != nullptr);
	REQUIRE(fill != nullptr);
	REQUIRE(hotbar != nullptr);
	CHECK(bg->y + bg->h <= hotbar->y); // sits above the hotbar
	CHECK(fill->w == doctest::Approx(bg->w - 2));
	CHECK(find(full, "health_text")->text == "20 / 20");
	const Widget *hunger_bg = find(full, "hunger_bg");
	REQUIRE(hunger_bg != nullptr);
	CHECK(hunger_bg->x > bg->x + bg->w); // hunger is to the right
	// Anchored to the *full* kHotbarSlots-wide row, not today's 1-slot hotbar
	// -- hud.lua's hotbar_layout().full_x/full_w, so a near-empty inventory
	// doesn't squeeze the "cur / max" text together (content/base/ui/hud.lua's
	// push_status_bars() comment). Checked as "centered on screen", since with
	// only 1 slot the hotbar itself is centered on a much narrower row and so
	// no longer shares the bars' left edge.
	CHECK(bg->x == doctest::Approx(1280 - (hunger_bg->x + hunger_bg->w)));

	// Half health: fill is half-width and turns yellow; low turns red.
	ui.set_player_status(UiRuntime::StatusView{ 10.0f, 20.0f, 100.0f, 100.0f });
	const auto half = ui.render_hud();
	CHECK(find(half, "health_fill")->w == doctest::Approx((bg->w - 2) / 2));
	const auto mid_g = find(half, "health_fill")->fill_g;
	ui.set_player_status(UiRuntime::StatusView{ 4.0f, 20.0f, 100.0f, 100.0f });
	const auto low = ui.render_hud();
	CHECK(find(low, "health_fill")->fill_r > find(low, "health_fill")->fill_g);
	CHECK(find(low, "health_fill")->fill_g < mid_g);

	// Zero health: no fill rect at all, background still drawn.
	ui.set_player_status(UiRuntime::StatusView{ 0.0f, 20.0f, 100.0f, 100.0f });
	const auto dead = ui.render_hud();
	CHECK(find(dead, "health_bg") != nullptr);
	CHECK(find(dead, "health_fill") == nullptr);
}

TEST_CASE("content/base/ui: the hotbar shows at most the 9 selectable slots, "
		"centered, with the container wrapping them") {
	UiRuntime ui = load_base_ui();
	ui.set_screen_size(1280, 720);
	std::vector<UiRuntime::InventorySlotView> slots;
	for (int i = 0; i < 12; ++i) {
		slots.push_back({ "base:stone", 1, 2 });
	}
	ui.set_inventory(slots, 1);
	const auto &w = ui.render_hud();
	CHECK(find(w, "hotbar_bg_9") != nullptr);
	CHECK(find(w, "hotbar_bg_10") == nullptr);

	const Widget *first = find(w, "hotbar_bg_1");
	const Widget *last = find(w, "hotbar_bg_9");
	const Widget *box = find(w, "hotbar_container");
	REQUIRE(box != nullptr);
	CHECK(box->x < first->x);
	CHECK(box->x + box->w > last->x + last->w);
	CHECK(std::abs((box->x + box->w / 2.0f) - 640.0f) <= 1.0f);
}

TEST_CASE("content/base/ui: base:inventory shows the live inventory, falling "
		"back to the open-time snapshot only when there is none") {
	UiRuntime ui = load_base_ui();
	ui.set_screen_size(1280, 720);
	ui.open("base:inventory", R"({"slots":[{"item":2,"count":5}]})");
	REQUIRE(ui.is_open());

	// No live inventory yet -> the snapshot.
	CHECK(find(ui.render_frame(), "slot_bg_2") == nullptr);

	// Live inventory arrives while the screen is open -> it wins, next frame.
	ui.set_inventory({ { "base:stone", 7, 2 }, { "base:planks", 3, 3 } }, 1);
	const auto &w = ui.render_frame();
	REQUIRE(find(w, "slot_bg_2") != nullptr);
	CHECK(find(w, "slot_count_1")->text == "7");
	CHECK(find(w, "slot_icon_2")->item == vb::core::BlockId{ 3 });
}

TEST_CASE("content/base/ui: base_ui style table is defined and used by the screens") {
	UiRuntime ui = load_base_ui();
	ui.set_screen_size(1280, 720);
	// A restyle of one shared color reaches every screen: change it from
	// Lua and the pause panel follows.
	REQUIRE(ui.load_pack_file("base_ui.panel_bg = { 1, 2, 3, 4 }", "test"));
	ui.open("base:pause", "{}");
	const auto &w = ui.render_frame();
	const Widget *panel = find(w, "panel");
	REQUIRE(panel != nullptr);
	CHECK(panel->fill_r == 1);
	CHECK(panel->fill_a == 4);
}

TEST_CASE("content/base/ui: the HUD has a centered crosshair") {
	UiRuntime ui = load_base_ui();
	ui.set_screen_size(1280, 720);
	const auto &w = ui.render_hud();
	const Widget *h = find(w, "crosshair_h");
	const Widget *v = find(w, "crosshair_v");
	REQUIRE(h != nullptr);
	REQUIRE(v != nullptr);
	CHECK(h->x + h->w / 2 == doctest::Approx(640.0f));
	CHECK(h->y + h->h / 2 == doctest::Approx(360.0f));
	CHECK(v->x + v->w / 2 == doctest::Approx(640.0f));
	CHECK(v->y + v->h / 2 == doctest::Approx(360.0f));
}

TEST_CASE("content/base/ui: a health drop flashes the screen red and fades out; "
		"heals, the first status and no change do not") {
	UiRuntime ui = load_base_ui();
	ui.set_screen_size(1280, 720);
	ui.set_clock(100.0);

	// First status after joining: no previous value, no flash.
	ui.set_player_status(UiRuntime::StatusView{ 20.0f, 20.0f, 100.0f, 100.0f });
	CHECK(find(ui.render_hud(), "damage_flash") == nullptr);

	// Damage -> flash, drawn before everything else.
	ui.set_player_status(UiRuntime::StatusView{ 15.0f, 20.0f, 100.0f, 100.0f });
	const auto hit = ui.render_hud();
	const Widget *flash = find(hit, "damage_flash");
	REQUIRE(flash != nullptr);
	CHECK(hit.front().id == "damage_flash");
	CHECK(flash->w == 1280);
	CHECK(flash->h == 720);
	const int a0 = flash->fill_a;
	CHECK(a0 > 0);

	// Fades with the clock...
	ui.set_clock(100.2);
	const Widget *later = find(ui.render_hud(), "damage_flash");
	REQUIRE(later != nullptr);
	CHECK(later->fill_a < a0);
	// ...and is gone after the deadline, with health unchanged.
	ui.set_clock(101.0);
	CHECK(find(ui.render_hud(), "damage_flash") == nullptr);

	// A heal doesn't flash.
	ui.set_player_status(UiRuntime::StatusView{ 20.0f, 20.0f, 100.0f, 100.0f });
	CHECK(find(ui.render_hud(), "damage_flash") == nullptr);
}

TEST_CASE("content/base/ui: base:inventory shows an item-name tooltip under the cursor") {
	UiRuntime ui = load_base_ui();
	ui.set_screen_size(1280, 720);
	ui.set_inventory({ { "base:oak_planks", 3, 3 }, { "air", 0, 0 } }, 1);
	ui.open("base:inventory", R"({"slots":[]})");
	REQUIRE(ui.is_open());

	// Cursor away from any slot: no tooltip.
	ui.set_mouse_position(1.0f, 1.0f);
	CHECK(find(ui.render_frame(), "tooltip_bg") == nullptr);

	// Cursor on slot 1: tooltip with the prettified name, drawn last.
	const auto slots = ui.render_frame();
	const Widget *slot1 = find(slots, "slot_bg_1");
	REQUIRE(slot1 != nullptr);
	ui.set_mouse_position(slot1->x + 5, slot1->y + 5);
	const auto hover = ui.render_frame();
	const Widget *tip = find(hover, "tooltip_text");
	REQUIRE(tip != nullptr);
	CHECK(tip->text == "Oak planks");
	CHECK(hover.back().id == "tooltip_text");
	CHECK(find(hover, "tooltip_bg") != nullptr);

	// Empty slot 2: nothing to show.
	const Widget *slot2 = find(hover, "slot_bg_2");
	REQUIRE(slot2 != nullptr);
	ui.set_mouse_position(slot2->x + 5, slot2->y + 5);
	CHECK(find(ui.render_frame(), "tooltip_bg") == nullptr);
}

TEST_CASE("content/base/ui: base:death shows a cause-specific message and a "
		"working Continue button") {
	UiRuntime ui = load_base_ui();
	ui.set_screen_size(1280, 720);

	ui.open("base:death", R"({"cause":"fall"})");
	REQUIRE(ui.is_open());
	const auto &w = ui.render_frame();
	CHECK(find(w, "title")->text == "You died");
	CHECK(find(w, "reason")->text == "You fell from a high place.");
	const Widget *panel = find(w, "panel");
	REQUIRE(panel != nullptr);
	CHECK(panel->x + panel->w / 2 == doctest::Approx(640.0f));
	ui.report_click("continue");
	CHECK_FALSE(ui.is_open());

	// Unknown cause falls back to a generic line.
	ui.open("base:death", R"({"cause":"lava"})");
	CHECK(find(ui.render_frame(), "reason")->text == "You died.");
}

TEST_CASE("content/base/ui: the selected item's name shows above the bars when "
		"the selection changes, fades out, and does not show on join") {
	UiRuntime ui = load_base_ui();
	ui.set_screen_size(1280, 720);
	ui.set_clock(10.0);
	ui.set_player_status(UiRuntime::StatusView{ 20.0f, 20.0f, 100.0f, 100.0f });
	const std::vector<UiRuntime::InventorySlotView> inv = {
		{ "base:stone", 5, 2 }, { "base:oak_planks", 2, 3 }, { "air", 0, 0 }
	};

	// First frame only records the selection: no label on join.
	ui.set_inventory(inv, 1);
	CHECK(find(ui.render_hud(), "item_name") == nullptr);

	// Switching to slot 2 shows it, fully opaque, centered, above the status bars.
	ui.set_inventory(inv, 2);
	const auto shown = ui.render_hud();
	const Widget *label = find(shown, "item_name");
	REQUIRE(label != nullptr);
	CHECK(label->text == "Oak planks");
	CHECK(label->fill_a == 255);
	CHECK(label->x == doctest::Approx(640.0f));
	CHECK(label->y < find(shown, "health_bg")->y);

	// Still there mid-way, then fading in the last half second, then gone.
	ui.set_clock(11.0);
	CHECK(find(ui.render_hud(), "item_name")->fill_a == 255);
	ui.set_clock(11.8);
	const Widget *fading = find(ui.render_hud(), "item_name");
	REQUIRE(fading != nullptr);
	CHECK(fading->fill_a < 255);
	CHECK(fading->fill_a > 0);
	ui.set_clock(12.1);
	CHECK(find(ui.render_hud(), "item_name") == nullptr);

	// Re-rendering without changing the selection doesn't bring it back...
	CHECK(find(ui.render_hud(), "item_name") == nullptr);
	// ...selecting an empty slot shows nothing...
	ui.set_inventory(inv, 3);
	CHECK(find(ui.render_hud(), "item_name") == nullptr);
	// ...and changing the item in the selected slot does show the new name.
	ui.set_inventory({ inv[0], inv[1], { "base:stone", 1, 2 } }, 3);
	const Widget *relabel = find(ui.render_hud(), "item_name");
	REQUIRE(relabel != nullptr);
	CHECK(relabel->text == "Stone");
}

#endif // VB_WITH_LUA
