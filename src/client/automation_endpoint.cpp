#include "automation_endpoint.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <optional>

#include <raylib.h>

#include "vb/core/math.hpp"
#include "vb/world/raycast.hpp"

namespace vb::client {

using nlohmann::json;
using vb::automation::Reply;
using vb::automation::Request;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kReach = 5.0; // the same distance the client's own targeting uses
constexpr double kFramesPerSecond = 60.0; // run loop's fixed dt (see main.cpp)

long long ms_to_frames(long long ms) {
	return std::max<long long>(1, static_cast<long long>(std::llround(static_cast<double>(ms) * kFramesPerSecond / 1000.0)));
}

bool read_vec3(const json &j, double out[3]) {
	if (!j.is_array() || j.size() != 3) {
		return false;
	}
	for (std::size_t i = 0; i < 3; ++i) {
		if (!j[i].is_number()) {
			return false;
		}
		out[i] = j[i].get<double>();
	}
	return true;
}

bool read_ivec3(const json &j, vb::core::IVec3 &out) {
	double v[3];
	if (!read_vec3(j, v)) {
		return false;
	}
	out = { static_cast<int>(std::floor(v[0])), static_cast<int>(std::floor(v[1])),
		static_cast<int>(std::floor(v[2])) };
	return true;
}

json ivec3_json(const vb::core::IVec3 &p) {
	return json::array({ p.x, p.y, p.z });
}

// Names a test may use for a key. Logical names track the player's bindings
// (so a test survives a rebind); anything else is a literal key.
std::optional<int> resolve_key(const std::string &raw, const vb::render::MovementBindings &b) {
	std::string n;
	for (char c : raw) {
		n += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	if (n == "forward")
		return b.forward;
	if (n == "back")
		return b.back;
	if (n == "left")
		return b.left;
	if (n == "right")
		return b.right;
	if (n == "jump")
		return b.jump;
	if (n == "sprint")
		return b.sprint;
	if (n == "inventory")
		return KEY_E; // kCustomKeybinds "base:inventory"
	if (n == "pause" || n == "escape")
		return KEY_ESCAPE;
	if (n == "chat" || n == "enter")
		return KEY_ENTER;
	if (n == "tab")
		return KEY_TAB;
	if (n == "space")
		return KEY_SPACE;
	if (n == "shift" || n == "left_shift")
		return KEY_LEFT_SHIFT;
	if (n == "ctrl" || n == "left_control")
		return KEY_LEFT_CONTROL;
	if (n.size() == 1 && n[0] >= 'a' && n[0] <= 'z')
		return KEY_A + (n[0] - 'a');
	if (n.size() == 1 && n[0] >= '1' && n[0] <= '9')
		return KEY_ONE + (n[0] - '1');
	if (n.size() == 5 && n.rfind("slot", 0) == 0 && n[4] >= '1' && n[4] <= '9') {
		return KEY_ONE + (n[4] - '1');
	}
	return std::nullopt;
}

std::optional<int> resolve_button(const std::string &n) {
	if (n == "left" || n == "primary")
		return MOUSE_BUTTON_LEFT;
	if (n == "right" || n == "secondary")
		return MOUSE_BUTTON_RIGHT;
	if (n == "middle")
		return MOUSE_BUTTON_MIDDLE;
	return std::nullopt;
}

// Yaw/pitch (degrees, the camera's own convention) that looks from `eye` at
// `target`. Inverse of core::forward_from_yaw_pitch.
void aim_angles(const vb::core::Vec3d &eye, double tx, double ty, double tz, double &yaw,
		double &pitch) {
	const double dx = tx - eye.x;
	const double dy = ty - eye.y;
	const double dz = tz - eye.z;
	const double len = std::sqrt(dx * dx + dy * dy + dz * dz);
	yaw = std::atan2(dx, -dz) * 180.0 / kPi;
	pitch = len > 1e-9 ? std::asin(dy / len) * 180.0 / kPi : 0.0;
}

const char *widget_type_name(vb::script::WidgetType t) {
	using T = vb::script::WidgetType;
	switch (t) {
		case T::kLabel:
			return "label";
		case T::kPanel:
			return "panel";
		case T::kButton:
			return "button";
		case T::kTextBox:
			return "textbox";
		case T::kList:
			return "list";
		case T::kRect:
			return "rect";
		case T::kText:
			return "text";
		case T::kIcon:
			return "icon";
		case T::kSprite:
			return "sprite";
	}
	return "unknown";
}

json widgets_json(const std::vector<vb::script::Widget> &ws) {
	json out = json::array();
	for (const auto &w : ws) {
		json j{ { "id", w.id }, { "type", widget_type_name(w.type) }, { "text", w.text },
			{ "x", w.x }, { "y", w.y }, { "w", w.w }, { "h", w.h } };
		if (w.type == vb::script::WidgetType::kList) {
			j["items"] = w.items;
			j["list_index"] = w.list_index;
		}
		out.push_back(std::move(j));
	}
	return out;
}

const vb::script::Widget *find_widget(const std::vector<vb::script::Widget> &ws, const std::string &id) {
	for (const auto &w : ws) {
		if (w.id == id) {
			return &w;
		}
	}
	return nullptr;
}

} // namespace

// --- tasks -----------------------------------------------------------------

struct ClientAutomationEndpoint::Ctx {
	ClientApp &app;
	vb::render::SyntheticInput &in;
	long long frame;

	const vb::net::ClientSession *session() const { return app.session(); }
	void ensure_captured() const {
		if (!app.mouse_captured_state() && !app.ui().is_open() && !app.chat_box_open()) {
			app.set_mouse_captured(true);
		}
	}
	std::optional<std::string> block_name(const vb::core::IVec3 &p) const {
		const vb::net::ClientSession *c = session();
		if (c == nullptr) {
			return std::nullopt;
		}
		const auto &store = c->chunk_store();
		if (!store.has(vb::core::chunk_of(p))) {
			return std::nullopt;
		}
		const vb::core::BlockId id = store.block_at(p);
		return store.registry().contains(id) ? std::optional<std::string>(store.registry().get(id).name)
											 : std::nullopt;
	}
	vb::world::VoxelRayHit look_ray() const {
		return vb::world::raycast_voxel(session()->chunk_store(), app.eye(),
				vb::core::forward_from_yaw_pitch(app.yaw(), app.pitch()), kReach);
	}
};

struct ClientAutomationEndpoint::Task {
	json id;
	virtual ~Task() = default;
	// Most actions work through the connected session; a screenshot of the menu doesn't.
	virtual bool needs_session() const { return true; }
	virtual void pre(Ctx &) {}
	// nullopt = still running.
	virtual std::optional<Reply> post(Ctx &) = 0;
};

namespace {

using Task = ClientAutomationEndpoint::Task;
using Ctx = ClientAutomationEndpoint::Ctx;

// Keeps an already-queued key/button held and answers after `frames` frames.
struct HoldTask final : Task {
	long long frames;
	long long seen = 0;
	explicit HoldTask(long long n) :
			frames(n) {}
	std::optional<Reply> post(Ctx &) override {
		if (++seen >= frames) {
			return Reply::success(json{ { "frames", frames } });
		}
		return std::nullopt;
	}
};

struct SelectSlotTask final : Task {
	int slot; // 0-based
	explicit SelectSlotTask(int s) :
			slot(s) {}
	void pre(Ctx &c) override {
		c.ensure_captured();
		c.in.press_key(KEY_ONE + slot);
	}
	std::optional<Reply> post(Ctx &c) override {
		if (c.app.selected_hotbar_slot() == slot) {
			return Reply::success(json{ { "slot", slot + 1 } });
		}
		return Reply::error("failed", "hotbar slot did not change (UI or chat open?)",
				json{ { "selected", c.app.selected_hotbar_slot() + 1 } });
	}
};

struct WalkToTask final : Task {
	double tx, tz;
	std::optional<double> ty;
	double tol;
	long long deadline;
	long long start = -1;
	double best = 1e18;
	long long stuck = 0;
	WalkToTask(double x, std::optional<double> y, double z, double t, long long timeout_frames) :
			tx(x), tz(z), ty(y), tol(t), deadline(timeout_frames) {}

	double horizontal(const Ctx &c) const {
		const auto f = c.session()->predicted_feet();
		return std::hypot(tx - f.x, tz - f.z);
	}
	void pre(Ctx &c) override {
		if (start < 0) {
			start = c.frame;
		}
		c.ensure_captured();
		const auto f = c.session()->predicted_feet();
		const double d = horizontal(c);
		if (d <= tol) {
			return;
		}
		double yaw = 0, pitch = 0;
		aim_angles({ f.x, f.y, f.z }, tx, f.y, tz, yaw, pitch);
		c.app.set_look(yaw, 0.0);
		c.in.hold_key(c.app.bindings().forward, 1);
		// Walked into a one-block rise? Hop; the shared movement code handles the rest.
		if (d > best - 0.01) {
			++stuck;
		} else {
			stuck = 0;
			best = d;
		}
		if (stuck > 10) {
			c.in.hold_key(c.app.bindings().jump, 1);
		}
	}
	std::optional<Reply> post(Ctx &c) override {
		const auto f = c.session()->predicted_feet();
		const bool near_xz = horizontal(c) <= tol;
		const bool near_y = !ty || std::abs(*ty - f.y) <= 1.0;
		if (near_xz && near_y) {
			return Reply::success(json{ { "feet", json::array({ f.x, f.y, f.z }) } });
		}
		if (c.frame - start >= deadline) {
			return Reply::error("timeout", "walk_to did not arrive in time",
					json{ { "last", json{ { "feet", json::array({ f.x, f.y, f.z }) } } } });
		}
		return std::nullopt;
	}
};

// Shared by break/place: look at `target`, and complain early if something
// other than the intended voxel/face is what the player would actually hit.
struct AimedTask : Task {
	// The server runs the pack's player_input hook for a command *before* it applies that
	// command's look direction, so a click sent in the same frame as a look change punches
	// along the previous direction. Hold the aim steady this many frames (two server ticks
	// plus loopback latency) before clicking. A human can't look and click within one 16 ms
	// frame; automation can, and would silently whiff.
	static constexpr int kSettleFrames = 8;
	long long deadline;
	long long start = -1;
	json fail;
	double last_yaw = 1e9, last_pitch = 1e9;
	int stable = 0;

	explicit AimedTask(long long timeout_frames) :
			deadline(timeout_frames) {}

	bool settled() const { return stable >= kSettleFrames; }

	// Returns false (and sets `fail`) if the aim point is out of reach.
	bool aim(Ctx &c, double tx, double ty, double tz) {
		const vb::core::Vec3d eye = c.app.eye();
		const double dist = std::sqrt((tx - eye.x) * (tx - eye.x) + (ty - eye.y) * (ty - eye.y) +
				(tz - eye.z) * (tz - eye.z));
		if (dist > kReach) {
			fail = json{ { "code", "out_of_reach" }, { "distance", dist }, { "reach", kReach } };
			return false;
		}
		double yaw = 0, pitch = 0;
		aim_angles(eye, tx, ty, tz, yaw, pitch);
		c.app.set_look(yaw, pitch);
		// 0.05 degrees is ~4 mm at five blocks: invisible to the server, but exact equality never holds while
		// server corrections are still nudging the predicted position (a slow, sanitized server).
		stable = (std::abs(yaw - last_yaw) < 0.05 && std::abs(pitch - last_pitch) < 0.05) ? stable + 1 : 0;
		last_yaw = yaw;
		last_pitch = pitch;
		return true;
	}
	std::optional<Reply> failure() const {
		return Reply::error(fail.value("code", std::string("failed")), "action could not be performed", fail);
	}
};

// Both actions below send ONE click and then wait for the server's answer, because the
// answer takes a round trip: clicking every other frame meanwhile would land several
// extra punches/placements before the client even sees the first result (which broke
// the blocks behind the target and placed stones in the player's own cell).
struct BreakBlockTask final : AimedTask {
	vb::core::IVec3 pos;
	std::string before;
	bool waiting = false;
	std::uint16_t punches_at_click = 0;
	// Wall time from each click to the client seeing the server's answer
	// (a punch count change, or the block gone): the punch round trip.
	std::chrono::steady_clock::time_point clicked_at;
	json confirm_ms = json::array();
	void confirmed() {
		const auto elapsed = std::chrono::steady_clock::now() - clicked_at;
		confirm_ms.push_back(std::chrono::duration<double, std::milli>(elapsed).count());
	}
	BreakBlockTask(vb::core::IVec3 p, long long timeout_frames) :
			AimedTask(timeout_frames), pos(p) {}

	static std::uint16_t punches(const Ctx &c, const vb::core::IVec3 &p) {
		const auto &damage = c.session()->block_damage();
		const auto it = damage.find(p);
		return it == damage.end() ? std::uint16_t{ 0 } : it->second;
	}

	void pre(Ctx &c) override {
		if (start < 0) {
			start = c.frame;
			before = c.block_name(pos).value_or("");
		}
		c.ensure_captured();
		if (!aim(c, pos.x + 0.5, pos.y + 0.5, pos.z + 0.5)) {
			return;
		}
		if (!waiting && settled()) {
			// One punch per rising edge (content/base/mechanics.lua). A block with
			// max_damage > 0 needs several; each is confirmed by S2C_BlockDamage.
			c.in.hold_mouse_button(MOUSE_BUTTON_LEFT, 1);
			waiting = true;
			punches_at_click = punches(c, pos);
			clicked_at = std::chrono::steady_clock::now();
		}
	}
	std::optional<Reply> post(Ctx &c) override {
		if (!fail.is_null()) {
			return failure();
		}
		const auto now = c.block_name(pos);
		if (before.empty()) {
			return Reply::error("not_loaded", "target chunk is not loaded");
		}
		if (now && *now != before) {
			if (waiting) {
				confirmed();
			}
			return Reply::success(json{ { "block_before", before }, { "block_after", *now },
					{ "confirm_ms", confirm_ms } });
		}
		// Free to punch again only once the server confirmed the last one. No blind retry on
		// silence: an instant-break block gives no feedback but vanishing, and a slow server
		// (a sanitizer build) answering late would turn a retry into a second punch that breaks
		// the block *behind* the target. Input batches repeat recent commands, so a click is
		// effectively never lost; if it is, the timeout says so.
		if (waiting && punches(c, pos) != punches_at_click) {
			waiting = false;
			confirmed();
		}
		if (c.frame - start >= deadline) {
			const auto hit = c.look_ray();
			json last{ { "block", now.value_or("") }, { "punches", punches(c, pos) } };
			if (hit.hit) {
				last["looking_at"] = ivec3_json(hit.voxel);
			}
			return Reply::error("timeout", "block did not break in time", json{ { "last", last } });
		}
		return std::nullopt;
	}
};

struct PlaceBlockTask final : AimedTask {
	vb::core::IVec3 pos, face, dest;
	bool clicked = false;
	std::string before;
	PlaceBlockTask(vb::core::IVec3 p, vb::core::IVec3 f, long long timeout_frames) :
			AimedTask(timeout_frames), pos(p), face(f), dest{ p.x + f.x, p.y + f.y, p.z + f.z } {}

	void pre(Ctx &c) override {
		if (start < 0) {
			start = c.frame;
			before = c.block_name(dest).value_or("");
		}
		c.ensure_captured();
		if (!aim(c, pos.x + 0.5 + 0.5 * face.x, pos.y + 0.5 + 0.5 * face.y, pos.z + 0.5 + 0.5 * face.z)) {
			return;
		}
		if (!clicked && settled()) {
			// Exactly one click, no retry: a second one could place a second block.
			c.in.hold_mouse_button(MOUSE_BUTTON_RIGHT, 1);
			clicked = true;
		}
	}
	std::optional<Reply> post(Ctx &c) override {
		if (!fail.is_null()) {
			return failure();
		}
		const auto now = c.block_name(dest);
		if (before.empty()) {
			return Reply::error("not_loaded", "target chunk is not loaded");
		}
		if (now && *now != before) {
			return Reply::success(json{ { "placed", *now }, { "at", ivec3_json(dest) } });
		}
		if (c.frame - start >= deadline) {
			const auto hit = c.look_ray();
			json last{ { "block", now.value_or("") } };
			if (hit.hit) {
				last["looking_at"] = ivec3_json(hit.voxel);
				last["face"] = ivec3_json(hit.normal);
			}
			return Reply::error("timeout", "block was not placed in time", json{ { "last", last } });
		}
		return std::nullopt;
	}
};

// Completes once the frame that was asked to save a screenshot has done so.
struct ScreenshotTask final : Task {
	std::string path;
	long long start = -1;
	explicit ScreenshotTask(std::string p) :
			path(std::move(p)) {}
	bool needs_session() const override { return false; }
	std::optional<Reply> post(Ctx &c) override {
		if (start < 0) {
			start = c.frame;
		}
		switch (c.app.screenshot_state()) {
			case ClientApp::Screenshot::kDone:
				return Reply::success(json{ { "path", path }, { "width", c.app.screenshot_width() },
						{ "height", c.app.screenshot_height() } });
			case ClientApp::Screenshot::kFailed:
				return Reply::error("failed", "could not write the screenshot", json{ { "path", path } });
			default:
				break;
		}
		if (c.frame - start > 600) {
			return Reply::error("timeout", "the screenshot was never taken");
		}
		return std::nullopt;
	}
};

} // namespace

// --- endpoint --------------------------------------------------------------

ClientAutomationEndpoint::ClientAutomationEndpoint(ClientApp &app, vb::automation::Host &host) :
		app_(app), host_(host) {
	app_.set_headless_ui_eval(true);
}

ClientAutomationEndpoint::~ClientAutomationEndpoint() = default;

void ClientAutomationEndpoint::begin_frame() {
	Ctx ctx{ app_, input_, frame_ };
	for (auto &t : tasks_) {
		if (!t->needs_session() || app_.session() != nullptr) {
			t->pre(ctx);
		}
	}
}

void ClientAutomationEndpoint::end_frame() {
	Ctx ctx{ app_, input_, frame_ };
	for (auto it = tasks_.begin(); it != tasks_.end();) {
		std::optional<Reply> done;
		if ((*it)->needs_session() && app_.session() == nullptr) {
			done = Reply::error("not_playing", "the client has no session");
		} else {
			done = (*it)->post(ctx);
		}
		if (done) {
			host_.respond((*it)->id, *done);
			it = tasks_.erase(it);
		} else {
			++it;
		}
	}
	++frame_;
}

json ClientAutomationEndpoint::state() {
	const vb::net::ClientSession *c = app_.session();
	json s{
		{ "app_state", ClientApp::app_state_name(app_.app_state()) },
		{ "joined", c != nullptr && c->joined() },
		{ "chat", json(std::vector<std::string>(app_.chat().begin(), app_.chat().end())) },
		{ "entities", json::array() },
		{ "chunks_loaded", 0 },
		{ "mouse_captured", app_.mouse_captured_state() },
		{ "chat_open", app_.chat_box_open() },
		{ "selected_slot", app_.selected_hotbar_slot() + 1 },
		{ "busy_actions", tasks_.size() },
	};
	if (app_.app_state() == AppState::kMenu) {
		s["menu"] = { { "recent_shown", app_.menu_recent_shown() }, { "recent_total", app_.recent_server_count() } };
	}
	if (app_.app_state() == AppState::kError) {
		s["error_message"] = app_.error_text();
	}
#if defined(VB_WITH_AUTH)
	if (const auto *si = app_.sign_in_state()) {
		// What the sign-in screen / re-auth banner would show (docs/automation-protocol.md).
		using Phase = vb::auth::SignInCoordinator::Phase;
		const char *phase = "idle";
		switch (si->phase()) {
			case Phase::kIdle:
				phase = "idle";
				break;
			case Phase::kChoosing:
				phase = "choosing";
				break;
			case Phase::kWorking:
				phase = "working";
				break;
			case Phase::kFinished:
				phase = "finished";
				break;
		}
		s["auth"] = json{ { "phase", phase }, { "needs_trust", si->needs_trust() },
			{ "reauth_prompt", si->reauth_prompt_active() }, { "error", si->last_error() } };
	}
#endif
	if (c == nullptr) {
		return s;
	}
	if (const auto &ja = c->join_accept()) {
		s["net_id"] = static_cast<std::uint32_t>(ja->your_net_id);
	}
	if (app_.ui().is_open()) {
		s["ui"] = json{ { "name", app_.ui().current_name() }, { "widgets", widgets_json(app_.ui().widgets()) } };
	}
	s["hud"] = json{ { "widgets", widgets_json(app_.hud_widgets()) } };
	// The local player's own status (S2C_PlayerStatus); absent until the first one arrives.
	if (const auto &st = c->player_status()) {
		s["health"] = st->health;
		s["max_health"] = st->max_health;
		s["hunger"] = st->hunger;
		s["max_hunger"] = st->max_hunger;
	}
	if (c->joined()) {
		const auto feet = c->predicted_feet();
		s["feet"] = { feet.x, feet.y, feet.z };
		s["on_ground"] = c->predicted_state().on_ground;
		s["yaw"] = app_.yaw();
		s["pitch"] = app_.pitch();
		const vb::world::VoxelRayHit hit = vb::world::raycast_voxel(c->chunk_store(), app_.eye(),
				vb::core::forward_from_yaw_pitch(app_.yaw(), app_.pitch()), kReach);
		if (hit.hit) {
			json t{ { "pos", ivec3_json(hit.voxel) }, { "normal", ivec3_json(hit.normal) } };
			const auto &store = c->chunk_store();
			const auto id = store.block_at(hit.voxel);
			if (store.registry().contains(id)) {
				t["block"] = store.registry().get(id).name;
			}
			s["target_block"] = std::move(t);
		}
	}
	s["chunks_loaded"] = c->chunk_store().size();
	if (const auto rtt = c->rtt_seconds()) {
		s["rtt_ms"] = *rtt * 1000.0; // absent until GNS has measured one
	}
	const auto &registry = c->chunk_store().registry();
	json inv = json::array();
	for (const auto &slot : c->inventory()) {
		inv.push_back({ { "item", registry.contains(slot.item) ? registry.get(slot.item).name : std::string("?") },
				{ "count", slot.count } });
	}
	s["inventory"] = std::move(inv);
	const auto &names = c->players();
	for (const auto &[id, rec] : c->remote_entities()) {
		const auto it = names.find(id);
		s["entities"].push_back({
				{ "net_id", static_cast<std::uint32_t>(id) },
				{ "name", it != names.end() ? it->second : std::string() },
				{ "pos", { rec.pos.x, rec.pos.y, rec.pos.z } },
		});
	}
	return s;
}

std::optional<std::string> ClientAutomationEndpoint::block_name_at(int x, int y, int z) {
	Ctx ctx{ app_, input_, frame_ };
	return ctx.block_name(vb::core::IVec3{ x, y, z });
}

bool ClientAutomationEndpoint::chunk_loaded_at(int x, int y, int z) {
	const vb::net::ClientSession *c = app_.session();
	return c != nullptr && c->chunk_store().has(vb::core::chunk_of(vb::core::IVec3{ x, y, z }));
}

std::optional<Reply> ClientAutomationEndpoint::command(const Request &req) {
	const json &a = req.args;
	auto bad = [](std::string msg) { return Reply::error("bad_request", std::move(msg)); };
	auto start_task = [&](std::unique_ptr<Task> t) {
		t->id = req.id;
		tasks_.push_back(std::move(t));
		return Reply::defer();
	};
	const bool playing = app_.session() != nullptr && app_.session()->joined() &&
			app_.app_state() == AppState::kPlaying;
	auto need_playing = [&]() -> std::optional<Reply> {
		if (playing) {
			return std::nullopt;
		}
		return Reply::error("not_playing", "client is not in the playing state",
				json{ { "app_state", ClientApp::app_state_name(app_.app_state()) } });
	};

	// --- raw input ---------------------------------------------------------
	if (req.cmd == "key.press" || req.cmd == "key.hold") {
		if (!a.contains("key") || !a["key"].is_string()) {
			return bad(req.cmd + ": needs string 'key'");
		}
		const auto key = resolve_key(a["key"].get<std::string>(), app_.bindings());
		if (!key) {
			return bad("unknown key '" + a["key"].get<std::string>() + "'");
		}
		const long long frames = req.cmd == "key.press" ? 1 : a.value("frames", 0LL);
		if (frames < 1 || frames > 100000) {
			return bad("key.hold: 'frames' must be 1..100000");
		}
		input_.hold_key(*key, static_cast<int>(frames));
		return start_task(std::make_unique<HoldTask>(frames));
	}
	if (req.cmd == "mouse.press" || req.cmd == "mouse.hold") {
		const auto button = resolve_button(a.value("button", std::string("left")));
		if (!button) {
			return bad(req.cmd + ": 'button' must be left|right|middle");
		}
		const long long frames = req.cmd == "mouse.press" ? 1 : a.value("frames", 0LL);
		if (frames < 1 || frames > 100000) {
			return bad("mouse.hold: 'frames' must be 1..100000");
		}
		input_.hold_mouse_button(*button, static_cast<int>(frames));
		return start_task(std::make_unique<HoldTask>(frames));
	}
	if (req.cmd == "mouse.capture") {
		if (auto err = need_playing()) {
			return err;
		}
		const bool on = a.value("on", true);
		if (on && (app_.ui().is_open() || app_.chat_box_open())) {
			return Reply::error("failed", "cannot capture the mouse while a UI screen or the chat box is open");
		}
		app_.set_mouse_captured(on);
		return Reply::success(json{ { "mouse_captured", on } });
	}
	if (req.cmd == "look") {
		if (auto err = need_playing()) {
			return err;
		}
		if (!a.contains("yaw") || !a["yaw"].is_number() || !a.contains("pitch") || !a["pitch"].is_number()) {
			return bad("look: needs numeric 'yaw' and 'pitch' (degrees)");
		}
		app_.set_look(a["yaw"].get<double>(), a["pitch"].get<double>());
		return Reply::success(json{ { "yaw", app_.yaw() }, { "pitch", app_.pitch() } });
	}
	if (req.cmd == "look_at") {
		if (auto err = need_playing()) {
			return err;
		}
		double p[3];
		if (!read_vec3(a.value("pos", json()), p)) {
			return bad("look_at: needs 'pos' [x,y,z]");
		}
		double yaw = 0, pitch = 0;
		aim_angles(app_.eye(), p[0], p[1], p[2], yaw, pitch);
		app_.set_look(yaw, pitch);
		return Reply::success(json{ { "yaw", app_.yaw() }, { "pitch", app_.pitch() } });
	}

	// --- high-level actions -----------------------------------------------
	if (req.cmd == "select_slot") {
		if (auto err = need_playing()) {
			return err;
		}
		const long long n = a.value("n", 0LL);
		if (n < 1 || n > 9) {
			return bad("select_slot: 'n' must be 1..9");
		}
		return start_task(std::make_unique<SelectSlotTask>(static_cast<int>(n - 1)));
	}
	if (req.cmd == "walk_to") {
		if (auto err = need_playing()) {
			return err;
		}
		const json &pos = a.value("pos", json());
		if (!pos.is_array() || pos.size() != 3 || !pos[0].is_number() || !pos[2].is_number() ||
				!(pos[1].is_number() || pos[1].is_null())) {
			return bad("walk_to: needs 'pos' [x, y|null, z]");
		}
		std::optional<double> y;
		if (pos[1].is_number()) {
			y = pos[1].get<double>();
		}
		const double tol = a.value("tolerance", 0.5);
		if (tol < 0.05) {
			return bad("walk_to: 'tolerance' must be >= 0.05");
		}
		return start_task(std::make_unique<WalkToTask>(pos[0].get<double>(), y, pos[2].get<double>(), tol,
				ms_to_frames(a.value("timeout_ms", 15000LL))));
	}
	if (req.cmd == "break_block") {
		if (auto err = need_playing()) {
			return err;
		}
		vb::core::IVec3 p;
		if (!read_ivec3(a.value("pos", json()), p)) {
			return bad("break_block: needs 'pos' [x,y,z]");
		}
		return start_task(std::make_unique<BreakBlockTask>(p, ms_to_frames(a.value("timeout_ms", 5000LL))));
	}
	if (req.cmd == "place_block") {
		if (auto err = need_playing()) {
			return err;
		}
		vb::core::IVec3 p, f;
		if (!read_ivec3(a.value("pos", json()), p) || !read_ivec3(a.value("face", json()), f)) {
			return bad("place_block: needs 'pos' [x,y,z] (existing block) and 'face' [nx,ny,nz]");
		}
		if (std::abs(f.x) + std::abs(f.y) + std::abs(f.z) != 1) {
			return bad("place_block: 'face' must be a unit axis vector, e.g. [0,1,0]");
		}
		return start_task(std::make_unique<PlaceBlockTask>(p, f, ms_to_frames(a.value("timeout_ms", 5000LL))));
	}
	if (req.cmd == "chat.send") {
		if (auto err = need_playing()) {
			return err;
		}
		if (!a.contains("text") || !a["text"].is_string() || a["text"].get<std::string>().empty()) {
			return bad("chat.send: needs non-empty string 'text'");
		}
		app_.submit_chat(a["text"].get<std::string>());
		return Reply::success();
	}

	// --- windowed-only: menu, screenshots, typing ----------------------------------
	if (req.cmd == "menu.set_name" || req.cmd == "menu.connect" || req.cmd == "menu.loopback" || req.cmd == "menu.singleplayer") {
		if (!app_.menu_active()) {
			return Reply::error("not_in_menu", "the client is not on the main menu",
					json{ { "app_state", ClientApp::app_state_name(app_.app_state()) } });
		}
		const std::string name = a.value("name", std::string());
		if (req.cmd == "menu.set_name") {
			if (name.empty()) {
				return bad("menu.set_name: needs non-empty string 'name'");
			}
			app_.menu_set_name(name);
		} else if (req.cmd == "menu.loopback" || req.cmd == "menu.singleplayer") {
			app_.menu_singleplayer(name);
		} else {
			if (!a.contains("host") || !a["host"].is_string() || !a.contains("port") || !a["port"].is_number_integer()) {
				return bad("menu.connect: needs string 'host' and integer 'port'");
			}
			app_.menu_connect(a["host"].get<std::string>(), a["port"].get<int>(), name);
		}
		return Reply::success(); // the transition shows up in app_state over the next frames
	}
	if (req.cmd == "signin.trust" || req.cmd == "signin.browser" || req.cmd == "signin.cancel") {
		const std::string choice = req.cmd.substr(std::string_view("signin.").size());
		if (!app_.sign_in_choose(choice)) {
			return Reply::error("unavailable", "that button is not on the sign-in screen (see the snapshot's 'auth')");
		}
		return Reply::success(); // acted on by the next frame, like a click
	}
	if (req.cmd == "screenshot") {
		if (!a.contains("path") || !a["path"].is_string() || a["path"].get<std::string>().empty()) {
			return bad("screenshot: needs string 'path'");
		}
		const std::string path = a["path"].get<std::string>();
		if (!app_.request_screenshot(path)) {
			return Reply::error("unsupported", "no framebuffer to capture (headless client, or one is already pending)");
		}
		return start_task(std::make_unique<ScreenshotTask>(path));
	}
	if (req.cmd == "type") {
		if (auto err = need_playing()) {
			return err;
		}
		if (!a.contains("text") || !a["text"].is_string() || a["text"].get<std::string>().empty()) {
			return bad("type: needs non-empty string 'text'");
		}
		if (!app_.type_chat(a["text"].get<std::string>())) {
			return Reply::error("failed", "cannot type here (a UI screen is open, or the chat box is full)");
		}
		return Reply::success();
	}

	// --- pack UI -----------------------------------------------------------
	if (req.cmd == "ui.click" || req.cmd == "ui.fill" || req.cmd == "ui.select" || req.cmd == "hud.click" ||
			req.cmd == "hud.fill" || req.cmd == "hud.select") {
		if (auto err = need_playing()) {
			return err;
		}
		const bool hud = req.cmd.rfind("hud.", 0) == 0;
		if (!hud && !app_.ui().is_open()) {
			return Reply::error("no_ui", "no UI screen is open");
		}
		if (!a.contains("id") || !a["id"].is_string()) {
			return bad(req.cmd + ": needs string 'id'");
		}
		const std::string id = a["id"].get<std::string>();
		const auto &widgets = hud ? app_.hud_widgets() : app_.ui().widgets();
		const vb::script::Widget *w = find_widget(widgets, id);
		if (w == nullptr) {
			json ids = json::array();
			for (const auto &x : widgets) {
				ids.push_back(x.id);
			}
			return Reply::error("no_widget", "no widget with id '" + id + "'", json{ { "widgets", ids } });
		}
		using T = vb::script::WidgetType;
		const std::string verb = req.cmd.substr(req.cmd.find('.') + 1);
		if (verb == "click") {
			if (w->type != T::kButton) {
				return Reply::error("bad_widget", "widget '" + id + "' is not a button");
			}
			app_.ui_click(id, hud);
		} else if (verb == "fill") {
			if (w->type != T::kTextBox) {
				return Reply::error("bad_widget", "widget '" + id + "' is not a textbox");
			}
			if (!a.contains("text") || !a["text"].is_string()) {
				return bad(req.cmd + ": needs string 'text'");
			}
			const std::string text = a["text"].get<std::string>();
			app_.ui_change(id, text, hud);
		} else {
			if (w->type != T::kList) {
				return Reply::error("bad_widget", "widget '" + id + "' is not a list");
			}
			const long long idx = a.value("index", -2LL);
			if (idx < 0 || idx >= static_cast<long long>(w->items.size())) {
				return bad(req.cmd + ": 'index' out of range 0.." + std::to_string(w->items.size()));
			}
			app_.ui_list(id, static_cast<int>(idx), hud);
		}
		return Reply::success();
	}
	return std::nullopt;
}

} // namespace vb::client
