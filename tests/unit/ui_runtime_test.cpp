#include <doctest/doctest.h>

#include <optional>
#include <ostream>

#include "vb/net/loopback.hpp"
#include "vb/net/session.hpp"
#include "vb/script/ui_runtime.hpp"

#if !VB_WITH_LUA

TEST_CASE("UiRuntime reports kDisabled when built without VB_WITH_LUA") {
	vb::script::UiRuntime ui;
	const auto r = ui.load_pack_file("return 1");
	CHECK_FALSE(r);
	CHECK(r.error == vb::core::ScriptError::kDisabled);
	CHECK_FALSE(ui.is_open());
	CHECK(ui.render_frame().empty());
	ui.set_break_progress(0.5f);
	ui.set_screen_size(1280, 720);
	CHECK(ui.render_hud().empty());
}

#else

using vb::script::UiRuntime;
using vb::script::WidgetType;

TEST_CASE("ui.define + open() + render_frame() produces the expected widget list") {
	UiRuntime ui;
	REQUIRE(ui.load_pack_file(R"(
		ui.define("greet", function(state)
			return {
				widgets = {
					{ id = "hello", type = "label", x = 1, y = 2, w = 30, h = 10,
					  text = "hi " .. state.name },
					{ id = "go", type = "button", x = 1, y = 20, w = 30, h = 10,
					  text = "Go" },
				}
			}
		end)
	)"));

	ui.open("greet", R"({"name":"Ada"})");
	REQUIRE(ui.is_open());
	CHECK(ui.current_name() == "greet");
	// Nothing is evaluated until the first render_frame() call.
	CHECK(ui.widgets().empty());

	const auto &widgets = ui.render_frame();
	REQUIRE(widgets.size() == 2);
	CHECK(widgets[0].id == "hello");
	CHECK(widgets[0].type == WidgetType::kLabel);
	CHECK(widgets[0].text == "hi Ada");
	CHECK(widgets[1].type == WidgetType::kButton);
	CHECK(&ui.widgets() == &widgets);
}

TEST_CASE("render_frame() re-evaluates every call, reflecting state mutated by a click handler") {
	UiRuntime ui;
	REQUIRE(ui.load_pack_file(R"(
		ui.define("counter", function(state)
			state.count = state.count or 0
			return {
				widgets = {
					{ id = "label", type = "label", x=0,y=0,w=1,h=1,
					  text = "count " .. state.count },
					{ id = "btn", type = "button", x=0,y=0,w=1,h=1, text = "+",
					  on_click = function() state.count = state.count + 1 end },
				}
			}
		end)
	)"));

	ui.open("counter", "{}");
	REQUIRE(ui.is_open());

	auto widgets = ui.render_frame();
	REQUIRE(widgets.size() == 2);
	CHECK(widgets[0].text == "count 0");

	// The click handler mutates `state`, which is the same table passed to
	// render_fn every frame -- the next render_frame() call must reflect it
	// without closing/reopening the screen.
	ui.report_click("btn");
	widgets = ui.render_frame();
	REQUIRE(widgets.size() == 2);
	CHECK(widgets[0].text == "count 1");
}

TEST_CASE("open() on an undefined name stays closed; render_frame() is a safe no-op") {
	UiRuntime ui;
	REQUIRE(ui.load_pack_file("-- no ui.define calls"));
	ui.open("nope", "{}");
	CHECK_FALSE(ui.is_open());
	CHECK(ui.render_frame().empty());
	CHECK(ui.widgets().empty());
}

TEST_CASE("report_click invokes the widget's on_click callback from the latest frame") {
	UiRuntime ui;
	REQUIRE(ui.load_pack_file(R"(
		clicked = false
		ui.define("test", function(state)
			return {
				widgets = {
					{ id = "btn", type = "button", x=0,y=0,w=1,h=1, text = "x",
					  on_click = function() clicked = true end },
				}
			}
		end)
	)"));
	ui.open("test", "{}");
	REQUIRE(ui.is_open());
	ui.render_frame();
	ui.report_click("btn");
	// send_event()/ui.close() on an unattached runtime (no attach_session
	// call) must be a safe no-op -- verified implicitly by not crashing.
	CHECK(ui.load_pack_file("assert(clicked == true)"));
}

TEST_CASE("close() with a session attached sends the close event instead of crashing") {
	// Regression: do_close() passed a state-less nil object to lua_to_json(), which
	// dereferenced a null lua_State -- but only once a session is attached, which no
	// other test here does (send_event() returns early without one).
	vb::net::LoopbackNetwork net;
	REQUIRE(net.server().listen(0));
	vb::net::Transport &transport = net.create_client();
	auto conn = transport.connect("loopback", 0);
	REQUIRE(conn);
	vb::net::ClientSession session(transport, *conn,
			vb::net::HandshakeClientConfig{ "Tester", "", "vb-test", 1 });

	UiRuntime ui;
	ui.attach_session(session);
	REQUIRE(ui.load_pack_file(R"(
		ui.define("test", function(state)
			return { widgets = { { id = "close", type = "button", x = 0, y = 0, w = 10, h = 10,
			                       text = "x", on_click = function() ui.close() end } } }
		end)
	)"));
	ui.open("test", "{}");
	ui.render_frame();
	ui.report_click("close"); // ui.close() from Lua -> do_close() -> send_event("close", nil)
	CHECK_FALSE(ui.is_open());

	ui.open("test", "{}");
	ui.close(); // and the C++ entry point
	CHECK_FALSE(ui.is_open());
}

TEST_CASE("ui.close() from the screen's own render function closes it after the frame") {
	// Regression: do_close() reset current_render_fn while that function was
	// still running, and the client exited without a word on the next frame.
	vb::net::LoopbackNetwork net;
	REQUIRE(net.server().listen(0));
	vb::net::Transport &transport = net.create_client();
	auto conn = transport.connect("loopback", 0);
	REQUIRE(conn);
	vb::net::ClientSession session(transport, *conn,
			vb::net::HandshakeClientConfig{ "Tester", "", "vb-test", 1 });

	UiRuntime ui;
	ui.attach_session(session);
	REQUIRE(ui.load_pack_file(R"(
		closed = 0
		after_close = nil
		ui.define("x:loading", function(state)
			if state.done then
				ui.close{ capture_mouse = true }
				after_close = state.done -- the render keeps running normally
				return { widgets = {}, on_close = function() closed = closed + 1 end }
			end
			return { widgets = { { id = "t", type = "text", x = 10, y = 10, text = "loading" } } }
		end)
	)"));
	ui.open("x:loading", "{}");
	CHECK(ui.render_frame().size() == 1);
	ui.open("x:loading", R"({"done": true})");
	CHECK(ui.render_frame().empty());
	CHECK_FALSE(ui.is_open());
	CHECK(ui.take_capture_request() == std::optional<bool>(true));
	CHECK(ui.load_pack_file("assert(after_close == true and closed == 1)"));
	CHECK(ui.render_frame().empty()); // nothing open: a no-op

	// A render that errors after closing still closes.
	REQUIRE(ui.load_pack_file(R"(
		ui.define("x:bad", function(state) ui.close(); error("boom") end)
	)"));
	ui.open("x:bad", "{}");
	ui.render_frame();
	CHECK_FALSE(ui.is_open());
}

TEST_CASE("close() invokes on_close (from the latest frame) exactly once and clears state") {
	UiRuntime ui;
	REQUIRE(ui.load_pack_file(R"(
		close_count = 0
		ui.define("test", function(state)
			return {
				on_close = function() close_count = close_count + 1 end,
				widgets = {},
			}
		end)
	)"));
	ui.open("test", "{}");
	REQUIRE(ui.is_open());
	ui.render_frame();
	ui.close();
	CHECK_FALSE(ui.is_open());
	CHECK(ui.widgets().empty());
	CHECK(ui.load_pack_file("assert(close_count == 1)"));

	// Closing again (already closed) must not re-invoke on_close.
	ui.close();
	CHECK(ui.load_pack_file("assert(close_count == 1)"));
}

TEST_CASE("ui.define_hud + render_hud() composes a progress bar entirely in "
		"Lua from the raw kRect primitive + client.break_progress()/"
		"screen_size() (Phase 6.16)") {
	UiRuntime ui;
	REQUIRE(ui.load_pack_file(R"(
		ui.define_hud(function(state)
			local widgets = {}
			local progress = client.break_progress()
			if progress then
				local screen = client.screen_size()
				local bar_w = 120
				local x = (screen.width - bar_w) / 2
				local y = screen.height / 2 + 24
				table.insert(widgets, {
					id = "bg", type = "rect", x = x, y = y, w = bar_w, h = 10,
					color = { 30, 30, 34, 200 }, border = { 90, 90, 100, 230 },
				})
				table.insert(widgets, {
					id = "fill", type = "rect", x = x, y = y,
					w = bar_w * progress, h = 10,
					color = { 220, 220, 220, 230 },
				})
			end
			return { widgets = widgets }
		end)
	)"));

	// Nothing breaking yet: the hud renders no widgets at all.
	ui.set_screen_size(800, 600);
	CHECK(ui.render_hud().empty());

	ui.set_break_progress(0.4f);
	const auto &widgets = ui.render_hud();
	REQUIRE(widgets.size() == 2);
	CHECK(widgets[0].id == "bg");
	CHECK(widgets[0].type == WidgetType::kRect);
	CHECK(widgets[0].x == doctest::Approx((800 - 120) / 2.0f));
	CHECK(widgets[0].y == doctest::Approx(600 / 2.0f + 24));
	CHECK(widgets[0].fill_r == 30);
	CHECK(widgets[0].border_a == 230);
	CHECK(widgets[1].id == "fill");
	CHECK(widgets[1].w == doctest::Approx(120 * 0.4f));
	CHECK(widgets[1].border_a == 0); // no `border` table given -> no outline

	// Clearing progress (nullopt) hides it again, without re-registering.
	ui.set_break_progress(std::nullopt);
	CHECK(ui.render_hud().empty());
}

TEST_CASE("ui.define with an \"icon\" widget carries its item id and tint "
		"(REMAINING_TASKS.md Phase 4's item grid widget gap)") {
	UiRuntime ui;
	REQUIRE(ui.load_pack_file(R"(
		ui.define("test", function(state)
			return {
				widgets = {
					{ id = "slot0", type = "icon", x = 10, y = 10, w = 32, h = 32, item = 5 },
					{ id = "slot1", type = "icon", x = 46, y = 10, w = 32, h = 32,
						item = 2, color = { 128, 128, 128, 200 } },
				},
			}
		end)
	)"));
	ui.open("test", "{}");
	const auto &widgets = ui.render_frame();
	REQUIRE(widgets.size() == 2);
	CHECK(widgets[0].type == WidgetType::kIcon);
	CHECK(widgets[0].item == vb::core::BlockId{ 5 });
	// No `color` given -> opaque white, i.e. no tint on the drawn texture.
	CHECK(widgets[0].fill_r == 255);
	CHECK(widgets[0].fill_a == 255);
	CHECK(widgets[1].item == vb::core::BlockId{ 2 });
	CHECK(widgets[1].fill_r == 128);
	CHECK(widgets[1].fill_a == 200);
}

// A missing `item` field (e.g. a plain "icon" widget the author forgot to
// fill in) defaults to air (id 0) rather than an uninitialized/garbage id --
// same "missing field -> the type's zero value" posture every other widget
// field already has (x/y/w/h default 0, text defaults empty, ...).
TEST_CASE("ui \"icon\" widget with no item field defaults to air") {
	UiRuntime ui;
	REQUIRE(ui.load_pack_file(R"(
		ui.define("test", function(state)
			return { widgets = { { id = "slot", type = "icon", x = 0, y = 0, w = 8, h = 8 } } }
		end)
	)"));
	ui.open("test", "{}");
	const auto &widgets = ui.render_frame();
	REQUIRE(widgets.size() == 1);
	CHECK(widgets[0].item == vb::core::BlockId::kAir);
}

TEST_CASE("ui.define with a \"sprite\" widget carries its texture, crop and tint") {
	UiRuntime ui;
	REQUIRE(ui.load_pack_file(R"(
		ui.define("test", function(state)
			return {
				widgets = {
					{ id = "whole", type = "sprite", x = 4, y = 6, w = 64, h = 64,
						texture = "textures/skins/red.png" },
					{ id = "frame", type = "sprite", x = 80, y = 6, w = 64, h = 64,
						texture = "textures/player.png", src = { 128, 0, 128, 128 },
						color = { 200, 100, 50, 255 } },
				},
			}
		end)
	)"));
	ui.open("test", "{}");
	const auto &widgets = ui.render_frame();
	REQUIRE(widgets.size() == 2);
	CHECK(widgets[0].type == WidgetType::kSprite);
	CHECK(widgets[0].texture == "textures/skins/red.png");
	// No `src`: a non-positive size means "the whole image".
	CHECK(widgets[0].src_w <= 0.0f);
	CHECK(widgets[0].fill_r == 255);
	CHECK(widgets[1].src_x == doctest::Approx(128.0f));
	CHECK(widgets[1].src_w == doctest::Approx(128.0f));
	CHECK(widgets[1].src_h == doctest::Approx(128.0f));
	CHECK(widgets[1].fill_r == 200);
	CHECK(widgets[1].fill_g == 100);
}

TEST_CASE("client.my_appearance() is nil when no session is attached") {
	UiRuntime ui;
	REQUIRE(ui.load_pack_file(R"(
		ui.define("test", function(state)
			local mine = client.my_appearance()
			return { widgets = { { id = mine == nil and "none" or "some", type = "label",
				x = 0, y = 0, w = 1, h = 1 } } }
		end)
	)"));
	ui.open("test", "{}");
	const auto &widgets = ui.render_frame();
	REQUIRE(widgets.size() == 1);
	CHECK(widgets[0].id == "none");
}

TEST_CASE("render_hud() is a safe no-op when no HUD was ever registered "
		"(Phase 6.16)") {
	UiRuntime ui;
	REQUIRE(ui.load_pack_file("-- no ui.define_hud call"));
	ui.set_break_progress(0.9f);
	CHECK(ui.render_hud().empty());
}

TEST_CASE("the HUD's state table persists across render_hud() calls, "
		"independent of any modal ui.define screen (Phase 6.16)") {
	UiRuntime ui;
	REQUIRE(ui.load_pack_file(R"(
		ui.define_hud(function(state)
			state.frames = (state.frames or 0) + 1
			return { widgets = { { id = "n", type = "label", x=0,y=0,w=1,h=1,
				text = tostring(state.frames) } } }
		end)
		ui.define("modal", function(state)
			return { widgets = {} }
		end)
	)"));

	CHECK(ui.render_hud()[0].text == "1");
	CHECK(ui.render_hud()[0].text == "2");

	// A modal screen opening/closing alongside it doesn't reset hud state.
	ui.open("modal", "{}");
	ui.render_frame();
	ui.close();
	CHECK(ui.render_hud()[0].text == "3");
}

TEST_CASE("client.inventory() entries carry name, count and raw item id") {
	UiRuntime ui;
	REQUIRE(ui.load_pack_file(R"(
		ui.define_hud(function(state)
			local s = client.inventory()[1]
			return { widgets = { { id = "s", type = "label", x=0,y=0,w=1,h=1,
				text = s.name .. ":" .. s.count .. ":" .. s.item } } }
		end)
	)"));
	ui.set_inventory({ { "base:stone", 5, 2 } }, 1);
	CHECK(ui.render_hud()[0].text == "base:stone:5:2");
}

TEST_CASE("client.time() and client.mouse_position() read back what was set") {
	UiRuntime ui;
	REQUIRE(ui.load_pack_file(R"(
		ui.define_hud(function(state)
			local m = client.mouse_position()
			return { widgets = { { id = "s", type = "label", x=0,y=0,w=1,h=1,
				text = string.format("%.1f %d,%d", client.time(), m.x, m.y) } } }
		end)
	)"));
	CHECK(ui.render_hud()[0].text == "0.0 0,0");
	ui.set_clock(12.5);
	ui.set_mouse_position(30.0f, 40.0f);
	CHECK(ui.render_hud()[0].text == "12.5 30,40");
}

TEST_CASE("client.health()/client.hunger() are nil until a status is set, "
		"then report current/max") {
	UiRuntime ui;
	REQUIRE(ui.load_pack_file(R"(
		ui.define_hud(function(state)
			local h, g = client.health(), client.hunger()
			return { widgets = { { id = "s", type = "label", x=0,y=0,w=1,h=1,
				text = (h and string.format("%d/%d", h.current, h.max) or "nil")
					.. " " .. (g and string.format("%d/%d", g.current, g.max) or "nil") } } }
		end)
	)"));
	CHECK(ui.render_hud()[0].text == "nil nil");
	ui.set_player_status(UiRuntime::StatusView{ 14.0f, 20.0f, 60.0f, 100.0f });
	CHECK(ui.render_hud()[0].text == "14/20 60/100");
	ui.set_player_status(std::nullopt);
	CHECK(ui.render_hud()[0].text == "nil nil");
}

TEST_CASE("report_hud_click invokes a HUD widget's on_click callback, "
		"distinct from a modal screen's own widget_by_id map (REMAINING_TASKS' "
		"'HUD widgets aren't wired to report_click/report_change' gap)") {
	UiRuntime ui;
	REQUIRE(ui.load_pack_file(R"(
		hud_clicked = false
		ui.define_hud(function(state)
			return {
				widgets = {
					{ id = "hud_btn", type = "button", x=0,y=0,w=1,h=1, text = "x",
					  on_click = function() hud_clicked = true end },
				}
			}
		end)
		ui.define("modal", function(state) return { widgets = {} } end)
	)"));

	ui.render_hud();
	// A same-named widget id on the modal side must not be reachable via
	// report_hud_click -- the two maps are genuinely separate.
	ui.report_click("hud_btn");
	CHECK(ui.load_pack_file("assert(hud_clicked == false)"));

	ui.report_hud_click("hud_btn");
	CHECK(ui.load_pack_file("assert(hud_clicked == true)"));
}

TEST_CASE("report_hud_change/report_hud_list_change invoke a HUD widget's "
		"on_change callback") {
	UiRuntime ui;
	REQUIRE(ui.load_pack_file(R"(
		last_text = nil
		last_index = nil
		ui.define_hud(function(state)
			return {
				widgets = {
					{ id = "box", type = "textbox", x=0,y=0,w=1,h=1, text = "",
					  on_change = function(v) last_text = v end },
					{ id = "list", type = "list", x=0,y=0,w=1,h=1, items = {"a","b"},
					  on_change = function(v) last_index = v end },
				}
			}
		end)
	)"));

	ui.render_hud();
	ui.report_hud_change("box", "typed");
	ui.report_hud_list_change("list", 1);
	CHECK(ui.load_pack_file(R"(
		assert(last_text == "typed")
		assert(last_index == 1)
	)"));
}

// Mouse capture is the pack's choice: nothing is requested unless a pack asks.
TEST_CASE("mouse capture: ui.close() requests nothing; ui.close{capture_mouse} and the layout field do") {
	UiRuntime ui;
	REQUIRE(ui.load_pack_file(R"(
		ui.define("plain", function(state)
			return { widgets = {
				{ id = "close", type = "button", x=0,y=0,w=1,h=1, text = "x",
				  on_click = function() ui.close() end },
				{ id = "back", type = "button", x=0,y=0,w=1,h=1, text = "x",
				  on_click = function() ui.close{ capture_mouse = true } end },
			} }
		end)
		ui.define("auto", function(state)
			return { capture_mouse_on_close = true, widgets = {
				{ id = "close", type = "button", x=0,y=0,w=1,h=1, text = "x",
				  on_click = function() ui.close() end },
				{ id = "stay", type = "button", x=0,y=0,w=1,h=1, text = "x",
				  on_click = function() ui.close{ capture_mouse = false } end },
			} }
		end)
		ui.define("veto", function(state)
			return { capture_mouse_on_close = true, widgets = {},
				on_close = function() client.capture_mouse(false) end }
		end)
	)"));

	SUBCASE("plain close keeps today's behaviour") {
		ui.open("plain", "{}");
		ui.render_frame();
		ui.report_click("close");
		CHECK_FALSE(ui.is_open());
		CHECK_FALSE(ui.take_capture_request());
	}
	SUBCASE("ui.close{ capture_mouse = true }") {
		ui.open("plain", "{}");
		ui.render_frame();
		ui.report_click("back");
		CHECK(ui.take_capture_request() == std::optional<bool>(true));
		CHECK_FALSE(ui.take_capture_request()); // taken once
	}
	SUBCASE("capture_mouse_on_close, and an explicit false overriding it") {
		ui.open("auto", "{}");
		ui.render_frame();
		ui.report_click("close");
		CHECK(ui.take_capture_request() == std::optional<bool>(true));
		ui.open("auto", "{}");
		ui.render_frame();
		ui.report_click("stay");
		CHECK(ui.take_capture_request() == std::optional<bool>(false));
	}
	SUBCASE("an on_close handler has the last word") {
		ui.open("veto", "{}");
		ui.render_frame();
		ui.close();
		CHECK(ui.take_capture_request() == std::optional<bool>(false));
	}
}

TEST_CASE("mouse capture: client.capture_mouse / client.mouse_captured") {
	UiRuntime ui;
	ui.set_mouse_captured(true);
	REQUIRE(ui.load_pack_file(R"(
		assert(client.mouse_captured() == true)
		client.capture_mouse(false)
		client.capture_mouse(true) -- the last request wins
	)"));
	CHECK(ui.take_capture_request() == std::optional<bool>(true));
	ui.set_mouse_captured(false);
	CHECK(ui.load_pack_file("assert(client.mouse_captured() == false)"));
}

#endif // VB_WITH_LUA
