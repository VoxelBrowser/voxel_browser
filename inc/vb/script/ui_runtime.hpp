#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "vb/core/ids.hpp"
#include "vb/net/session.hpp"
#include "vb/script/vm.hpp"

// Client-side UI VM (spec §10.4, Phase 6.2): a second, separate Lua VM from
// the server's PackRuntime. A pack declares screens via `ui.define(name,
// render_fn)`; `render_fn(state)` is called once per UI frame (raygui is
// itself immediate-mode, so no virtual-DOM diffing is needed) and its
// returned widget list is exposed as plain data (Widget, no sol2) for
// vb::render::UiRenderer to draw with raygui. `state` is the same Lua table
// across every frame a screen stays open (seeded once from open()'s
// ctx_json), so a widget callback mutating `state` is naturally visible on
// the next render_frame() call -- that's the entire reactivity mechanism.
// Widget on_click/on_change/on_close callbacks may call
// ui.send_event(...)/ui.close(), which route straight to the attached
// ClientSession as a C2S_UiEvent -- game-meaningful logic stays
// server-authoritative; purely cosmetic state can stay Lua-local on `state`.

namespace vb::script {

enum class WidgetType : std::uint8_t {
	kLabel,
	kPanel,
	kButton,
	kTextBox,
	kList,
	// A raw filled rectangle, optionally outlined -- deliberately the only
	// *non*-interactive primitive here with no baked-in meaning (no "this is
	// a progress bar/health bar/divider" concept engine-side). Composing a
	// progress bar, a health bar, a divider line, or anything else purely
	// visual out of one or more of these is entirely a Lua/content concern
	// (Phase 6.16) -- the engine only ever draws what it's told: a filled
	// box at (x, y, w, h) in `fill_*`, plus a 1px outline in `border_*` when
	// `border_a > 0`.
	kRect,
	// A raw, non-raygui text draw (Phase 6.16 follow-up: migrating the
	// player list / chat log / hotbar off hardcoded engine drawing needed a
	// colored, alignable text primitive -- kLabel goes through raygui's
	// GuiLabel, which has no color parameter at all). Text metrics (needed
	// for `align`) require raylib's MeasureText, which vb::script doesn't
	// (and shouldn't) depend on -- that measurement happens in the
	// raylib-linked render layer (vb::render::UiRenderer), not here; this
	// struct only carries the request.
	kText,
	// A registered block/item id's real atlas texture drawn at (x, y, w, h)
	// (REMAINING_TASKS.md Phase 4's "item grid widget for UiRuntime" gap):
	// deliberately just "one item's icon," the same no-baked-in-meaning
	// posture kRect/kText already have -- an item *grid* (rows/columns,
	// slot borders, hover highlight, a count label per slot) is entirely a
	// Lua/content concern composed out of this plus kRect/kText, exactly
	// how 6.16 composed a progress bar out of kRect. `color` (default
	// opaque white, i.e. no tint) multiplies the drawn texture same as
	// raylib's own tint parameter -- lets content grey out an unusable
	// slot without a second widget on top.
	kIcon,
	// A pack texture (`texture`, a pack-relative path such as
	// "textures/skins/red.png") drawn at (x, y, w, h), optionally cropped to
	// the pixel rectangle `src = {x, y, w, h}` of the image (default: the
	// whole image) -- one frame of a spritesheet, a portrait, a wardrobe
	// swatch. `color` tints it like kIcon's. A paper-doll preview is several
	// of these stacked, one per layer of client.my_appearance().
	kSprite,
};

// kText only: `x` is the anchor edge/point `align` is relative to, not
// always the left edge (mirrors how a pack picks between anchoring a widget
// by its left edge vs. wanting it centered/right-aligned without knowing the
// rendered text's pixel width up front, e.g. right-aligning the player list
// against the screen edge).
enum class TextAlign : std::uint8_t {
	kLeft,
	kCenter,
	kRight,
};

struct Widget {
	std::string id;
	WidgetType type = WidgetType::kLabel;
	float x = 0, y = 0, w = 0, h = 0;
	std::string text; // label/panel/button text; textbox initial value; kText content
	std::vector<std::string> items; // list only
	int list_index = -1; // list only
	std::uint8_t fill_r = 255, fill_g = 255, fill_b = 255, fill_a = 255; // kRect/kText color
	std::uint8_t border_r = 0, border_g = 0, border_b = 0, border_a = 0; // kRect only; alpha 0 = no border
	int font_size = 16; // kText only
	TextAlign align = TextAlign::kLeft; // kText only
	core::BlockId item = core::BlockId::kAir; // kIcon only
	std::string texture; // kSprite only: pack-relative path
	float src_x = 0, src_y = 0, src_w = 0, src_h = 0; // kSprite only; src_w/src_h <= 0 = whole image
};

class UiRuntime {
public:
	explicit UiRuntime(VmLimits limits = {});
	~UiRuntime();
	UiRuntime(UiRuntime &&) noexcept;
	UiRuntime &operator=(UiRuntime &&) noexcept;
	UiRuntime(const UiRuntime &) = delete;
	UiRuntime &operator=(const UiRuntime &) = delete;

	// Runs one chunk of UI Lua source (registration only: ui.define calls).
	ScriptResult load_pack_file(std::string_view code,
			std::string_view chunk_name = "ui_pack");

	// Outgoing C2S_UiEvent frames (ui.send_event / ui.close) go through this
	// session once attached; unattached, they're silently dropped.
	void attach_session(net::ClientSession &session);

	// Looks up `name` (registered via ui.define) and initializes `state`
	// from `ctx_json` (parsed JSON -> Lua table); that same table is passed
	// to `render_fn` on every subsequent render_frame() call for as long as
	// this screen stays open. Logs and leaves the runtime closed if `name`
	// was never defined. Does not evaluate `render_fn` itself -- call
	// render_frame() to produce the first frame's widgets.
	void open(std::string_view name, std::string_view ctx_json);

	// Sends exactly one "close" C2S_UiEvent (spec: on_close always reaches
	// the server), then runs the pack's own on_close callback if present
	// (local cosmetic cleanup only), then clears state. No-op if not open.
	void close();

	bool is_open() const;
	const std::string &current_name() const;

	// Mouse capture, pack-controlled (default: the engine never recaptures on
	// its own). set_mouse_captured() feeds client.mouse_captured();
	// take_capture_request() returns and clears the latest request from
	// client.capture_mouse(on), ui.close{ capture_mouse = ... } or a screen's
	// capture_mouse_on_close. The client applies a capture only once no
	// screen and no chat box are open.
	void set_mouse_captured(bool captured);
	std::optional<bool> take_capture_request();

	// Evaluates render_fn(state) for the current frame (no-op, returning
	// the last -- likely empty -- list if no screen is open) and returns
	// the fresh widget list. Call once per UI frame, right before handing
	// the result to vb::render::UiRenderer::draw.
	const std::vector<Widget> &render_frame();

	// The widget list from the most recently completed render_frame() call.
	const std::vector<Widget> &widgets() const;

	// Called by vb::render::UiRenderer once per frame per widget that
	// reported an interaction this frame. Invokes that widget's Lua
	// on_click/on_change callback if present; a callback error is caught
	// and logged, never propagated.
	void report_click(const std::string &widget_id);
	void report_change(const std::string &widget_id, std::string_view text_value);
	void report_list_change(const std::string &widget_id, int new_index);

	// --- HUD: an always-on overlay independent of open()/close() ---------
	//
	// "Engine provides raw state, Lua handles presentation": the engine
	// computes things like block-break progress (input timing, reach,
	// target tracking) since that's gameplay logic, but never draws a pixel
	// of it -- a pack's `ui.define_hud(render_fn)` (registered like
	// `ui.define`, but always active, not tied to any open()/close() name)
	// decides whether/how to show it. `render_fn(state)` is called every UI
	// frame regardless of whether a modal screen (open()/close() above) is
	// also open, with its own persistent `state` table (no ctx_json --
	// there's no "opening" a HUD). Read the raw values a HUD might want via
	// e.g. `client.break_progress()`/`client.screen_size()` below. A HUD
	// widget's on_click/on_change callback can call ui.send_event(...) like
	// any modal-screen widget -- the resulting C2S_UiEvent carries
	// `ui_name = "hud"` (there's no modal screen name to use).

	// Sets the raw local state a HUD's render_fn can read back via
	// `client.break_progress()` -- nullopt when not currently breaking
	// anything. Call once per frame from the input-handling code that
	// already computes this (src/client/main.cpp), before render_hud().
	void set_break_progress(std::optional<float> fraction);

	// Sets the window size a HUD's render_fn can read back via
	// `client.screen_size()` (`{width=.., height=..}`) -- widgets take
	// absolute pixel positions (same as modal screens), so a HUD wanting to
	// center something needs this raw value rather than a hardcoded guess.
	void set_screen_size(int width, int height);

	// Raw per-frame client state for `client.time()` (monotonic seconds, for
	// presentation timing like fades) and `client.mouse_position()` (window
	// pixels, same space as widget x/y). Both are 0 until first set.
	void set_clock(double seconds);
	void set_mouse_position(float x, float y);

	// The remaining set_* calls below (player list, chat, inventory) close
	// REMAINING_TASKS.md's "player list / chat box / hotbar are still
	// hardcoded C++, not migrated to ui.define_hud" gap -- same "engine
	// provides raw state, Lua decides presentation" posture as
	// break_progress/screen_size above, just for the 3 other pieces of
	// always-on HUD content src/client/main.cpp used to draw directly.

	// Sets the state `client.player_name()`/`client.players()` read back --
	// `own_name` is this client's own name (drawn distinctly, matching the
	// old hardcoded green), `other_names` every other currently-playing
	// player's name in `S2C_PlayerList`/join/leave order. Neither list
	// includes the other.
	void set_player_list(std::string own_name, std::vector<std::string> other_names);

	// Sets the state `client.chat_log()`/`client.chat_open()` read back.
	// `chat_open` doesn't mean the HUD should draw the input box itself --
	// that's still a plain raygui `GuiTextBox` in src/client/main.cpp, not a
	// UiRuntime widget (real keyboard text-entry capture, same posture as
	// MainMenu) -- a HUD needs it only to know whether to leave room for it.
	void set_chat(std::vector<std::string> log, bool chat_open);

	// One resolved (name already looked up against the block registry, this
	// module has no registry access) inventory slot, for
	// `client.inventory()`.
	struct InventorySlotView {
		std::string name;
		std::uint32_t count = 0;
		// Raw block/item id (0 = empty slot) -- what an `icon` widget's
		// `item` field takes.
		std::uint32_t item = 0;
	};

	// Sets the state `client.inventory()`/`client.selected_slot()` read
	// back. `selected_slot` is 1-based, matching every other Lua-visible
	// slot index in this codebase (`PlayerHandle::get_selected_slot()`).
	void set_inventory(std::vector<InventorySlotView> slots, int selected_slot);

	// Sets the state `client.health()`/`client.hunger()` read back. Pass
	// nullopt before the server's first status arrives (both then return nil).
	struct StatusView {
		float health = 0.0f;
		float max_health = 0.0f;
		float hunger = 0.0f;
		float max_hunger = 0.0f;
	};
	void set_player_status(std::optional<StatusView> status);

	// Who this client is signed in as, for "Signed in as ..." labels: every
	// render_fn's `state.login` is a read-only `{ name = .., subject = .. }`
	// (the in-game name the server gave the player, and the identity
	// provider's stable account id), or nil when the server has no auth.lua.
	// Never tokens or claims (architecture_spec/auth.md §6). The engine owns
	// `state.login`: it is rewritten before every render.
	struct LoginView {
		std::string name;
		std::string subject;
	};
	void set_login(std::optional<LoginView> login);

	// Evaluates the registered HUD render_fn (no-op, returning the last --
	// likely empty -- list if none was ever registered via
	// ui.define_hud) and returns the fresh widget list. Call once per UI
	// frame, unconditionally (unlike render_frame(), not gated on is_open()).
	const std::vector<Widget> &render_hud();

	// HUD counterparts of report_click/report_change/report_list_change
	// above -- called by vb::render::UiRenderer's draw() result for the HUD's
	// own widget list (a separate id->callback map from whatever modal screen
	// is or isn't currently open, since a HUD is always active independent of
	// open()/close()).
	void report_hud_click(const std::string &widget_id);
	void report_hud_change(const std::string &widget_id, std::string_view text_value);
	void report_hud_list_change(const std::string &widget_id, int new_index);

	// Dotted names of the `ui.*`/`client.*` surface (API drift test).
	std::vector<std::string> describe_api();

	// Names of the Lua globals that exist now (see PackRuntime::global_names).
	std::vector<std::string> global_names();

	struct Impl;

private:
	std::unique_ptr<Impl> impl_;
};

} // namespace vb::script
