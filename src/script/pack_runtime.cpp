#include "vb/script/pack_runtime.hpp"

#if !VB_WITH_LUA

// Stub build: scripting compiled out. Every entry point is a no-op / reports
// kDisabled, mirroring vm.cpp's disabled-build pattern.

namespace vb::script {

struct PackRuntime::Impl {};

PackRuntime::PackRuntime(net::Transport &, world::BlockRegistry &,
		std::filesystem::path, VmLimits) : impl_(nullptr) {}
PackRuntime::~PackRuntime() = default;
PackRuntime::PackRuntime(PackRuntime &&) noexcept = default;
PackRuntime &PackRuntime::operator=(PackRuntime &&) noexcept = default;

#if defined(VB_WITH_AUTOMATION)
bool PackRuntime::admin_give(core::NetId, std::string_view, std::uint16_t) {
	return false;
}
#endif
ScriptResult PackRuntime::load_pack_file(std::string_view, std::string_view) {
	return { false, core::ScriptError::kDisabled,
		"scripting disabled (built without VB_WITH_LUA)" };
}
void PackRuntime::freeze() {}
ScriptResult PackRuntime::validate_worldgen() const { return ScriptResult::success(); }
void PackRuntime::install_join_veto(net::HandshakeServerHost &) {}
void PackRuntime::set_auth_required(bool) {}
void PackRuntime::install_keybind_registry(net::HandshakeServerHost &) {}
void PackRuntime::install_entity_kind_registry(net::HandshakeServerHost &) {}
void PackRuntime::attach_world(net::WorldReplicator &) {}
void PackRuntime::attach_session(net::ServerSession &) {}
void PackRuntime::set_server_config(const core::ServerConfig &) {}
void PackRuntime::set_pack_modules(std::unordered_map<std::string, std::string>) {}
physics::MoveParams PackRuntime::effective_move_params(physics::MoveParams base) const {
	return base;
}
net::ServerSession::PunchParams PackRuntime::effective_punch_params(
		net::ServerSession::PunchParams base) const {
	return base;
}
// Real pre-existing gap found while adding effective_hunger_params() just
// below: effective_action_params() had no stub at all in this !VB_WITH_LUA
// block, even though src/server/main.cpp/src/client/main.cpp call it
// unconditionally -- a real link error waiting for anyone who actually
// configures -DVB_WITH_LUA=OFF, never caught because no CI workflow does
// (checked: build_linux/macos/windows.yml all hardcode VB_WITH_LUA=ON).
net::ActionParams PackRuntime::effective_action_params(net::ActionParams base) const {
	return base;
}
net::ServerSession::HungerParams PackRuntime::effective_hunger_params(
		net::ServerSession::HungerParams base) const {
	return base;
}
std::optional<world::DayNightCurve> PackRuntime::effective_day_night_curve() const {
	return std::nullopt;
}
double PackRuntime::effective_day_length_seconds(double base) const {
	return base;
}
std::shared_ptr<const worldgen::PackWorldGenPipeline> PackRuntime::build_worldgen_pipeline(
		const worldgen::WorldGenParams &) const {
	return nullptr;
}
void PackRuntime::dispatch_player_join_completed(const net::SessionPlayerJoined &) {}
void PackRuntime::dispatch_player_leave(const net::SessionPlayerLeft &) {}
void PackRuntime::dispatch_login_changed(const net::SessionLoginChanged &) {}
void PackRuntime::dispatch_tick(double) {}
net::ServerSession::ChatHookResult PackRuntime::dispatch_chat(
		core::NetId, std::string_view) {
	return {};
}
bool PackRuntime::dispatch_player_interact(core::NetId, core::IVec3) {
	return true;
}
void PackRuntime::dispatch_ui_event(core::NetId, const protocol::C2SUiEvent &) {}
bool PackRuntime::storage_dirty() const { return false; }
void PackRuntime::flush_storage() {}
std::uint64_t PackRuntime::storage_revision() const { return 0; }
std::vector<std::string> PackRuntime::describe_api() { return {}; }
std::vector<std::string> PackRuntime::global_names() { return {}; }

} // namespace vb::script

#else

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "vb/core/log.hpp"
#include "vb/core/sha256.hpp"
#include "vb/ecs/components.hpp"
#include "vb/protocol/chat.hpp"
#include "vb/protocol/input.hpp"
#include "vb/protocol/inventory.hpp"
#include "vb/script/api_surface.hpp"
#include "vb/script/block_def.hpp"
#include "vb/script/db.hpp"
#include "vb/script/structure_def.hpp"
#include "vb/script/vm_internal.hpp"
#include "vb/world/raycast.hpp"
#include "vb/worldgen/fastnoise2_compile.hpp"
#include "vb/worldgen/noise_graph.hpp"

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

// Deep, read-only view of `o` for data handed to pack scripts (auth.md §6:
// "frozen table"). Tables are copied and wrapped in a proxy whose metatable
// rejects writes and hides itself, so a pack can neither mutate nor swap out
// the login it was given.
sol::object freeze_deep(sol::state_view lua, const sol::object &o) {
	if (o.get_type() != sol::type::table) {
		return o;
	}
	const sol::table src = o.as<sol::table>();
	sol::table data = lua.create_table();
	for (const auto &kv : src) {
		data[kv.first] = freeze_deep(lua, kv.second);
	}
	sol::table proxy = lua.create_table();
	sol::table mt = lua.create_table();
	mt["__index"] = data;
	mt["__newindex"] = [](sol::this_state) { throw sol::error("login data is read-only"); };
	mt["__len"] = [data]() { return data.size(); };
	mt["__pairs"] = [data](sol::this_state ts) {
		sol::state_view l(ts);
		return std::make_tuple(l["next"].get<sol::object>(), data, sol::object(sol::lua_nil));
	};
	mt["__metatable"] = false;
	proxy[sol::metatable_key] = mt;
	return proxy;
}

// { provider, subject, name, claims = {...allowlisted...} } -- user data only;
// the issuer, timestamps and token never reach a script VM.
sol::object login_to_lua(sol::state_view lua, const net::LoginData &login) {
	sol::table t = lua.create_table();
	t["provider"] = login.provider;
	t["subject"] = login.subject;
	t["name"] = login.name;
	const nlohmann::json claims = nlohmann::json::parse(login.claims_json, nullptr, false);
	t["claims"] = claims.is_object() ? json_to_lua(lua, claims)
									 : sol::object(lua.create_table());
	return freeze_deep(lua, t);
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

constexpr int kMaxTimerCatchUpFires = 8; // anti-stall guard for vb.every after a stall

// Phase 6.3: builds the Lua-facing `input` table vb.on("player_input", ...)
// receives, from the current working values (not the raw wire bitmask) --
// only registered keybind names ever appear as `keybinds` keys.
sol::table build_input_table(sol::state &lua, core::Vec3f move, float yaw,
		float pitch, std::uint8_t buttons, std::uint32_t keybinds,
		const std::vector<std::string> &keybind_names, double dt,
		std::uint8_t selected_slot) {
	sol::table t = lua.create_table();
	sol::table move_t = lua.create_table();
	move_t["x"] = move.x;
	move_t["y"] = move.y;
	move_t["z"] = move.z;
	t["move"] = move_t;
	t["yaw"] = yaw;
	t["pitch"] = pitch;
	// The wall-clock time this cmd covers -- needed by any pack-side policy
	// that accrues something (e.g. hold-to-break progress) per real second
	// rather than per tick, since a client can batch/resend multiple cmds
	// per server tick. Read-only in practice: nothing in run_player_input
	// reconstructs InputCmd::dt from a handler's returned table, unlike
	// move/yaw/pitch/buttons/keybinds below.
	t["dt"] = dt;

	sol::table buttons_t = lua.create_table();
	buttons_t["jump"] = (buttons & protocol::kInputJump) != 0;
	buttons_t["sprint"] = (buttons & protocol::kInputSprint) != 0;
	buttons_t["primary"] = (buttons & protocol::kInputPrimary) != 0;
	buttons_t["secondary"] = (buttons & protocol::kInputSecondary) != 0;
	buttons_t["fly_up"] = (buttons & protocol::kInputFlyUp) != 0;
	buttons_t["fly_down"] = (buttons & protocol::kInputFlyDown) != 0;
	t["buttons"] = buttons_t;

	sol::table keybinds_t = lua.create_table();
	for (std::size_t i = 0; i < keybind_names.size(); ++i) {
		keybinds_t[keybind_names[i]] = (keybinds & (1u << i)) != 0;
	}
	t["keybinds"] = keybinds_t;

	// Entity-management follow-up (held item / hotbar selection): 1-based to
	// match player:get_inventory()'s own 1-based array, unlike the wire's
	// 0-based InputCmd::selected_slot.
	t["selected_slot"] = static_cast<int>(selected_slot) + 1;
	return t;
}

// Reconstructs a buttons bitmask from a handler's returned `buttons` table
// (if present); any field the pack omits keeps its bit from `fallback`.
std::uint8_t buttons_from_table(const sol::table &t, std::uint8_t fallback) {
	sol::object bo = t["buttons"];
	if (bo.get_type() != sol::type::table) {
		return fallback;
	}
	sol::table bt = bo.as<sol::table>();
	std::uint8_t out = 0;
	if (bt.get_or("jump", (fallback & protocol::kInputJump) != 0)) {
		out |= protocol::kInputJump;
	}
	if (bt.get_or("sprint", (fallback & protocol::kInputSprint) != 0)) {
		out |= protocol::kInputSprint;
	}
	if (bt.get_or("primary", (fallback & protocol::kInputPrimary) != 0)) {
		out |= protocol::kInputPrimary;
	}
	if (bt.get_or("secondary", (fallback & protocol::kInputSecondary) != 0)) {
		out |= protocol::kInputSecondary;
	}
	if (bt.get_or("fly_up", (fallback & protocol::kInputFlyUp) != 0)) {
		out |= protocol::kInputFlyUp;
	}
	if (bt.get_or("fly_down", (fallback & protocol::kInputFlyDown) != 0)) {
		out |= protocol::kInputFlyDown;
	}
	return out;
}

// Same idea for the `keybinds` sub-table, only ever consulting registered
// names -- an unregistered key can't be represented here any more than on
// the wire.
std::uint32_t keybinds_from_table(const sol::table &t, std::uint32_t fallback,
		const std::vector<std::string> &keybind_names) {
	sol::object ko = t["keybinds"];
	if (ko.get_type() != sol::type::table) {
		return fallback;
	}
	sol::table kt = ko.as<sol::table>();
	std::uint32_t out = 0;
	for (std::size_t i = 0; i < keybind_names.size(); ++i) {
		const bool was_set = (fallback & (1u << i)) != 0;
		if (kt.get_or(keybind_names[i], was_set)) {
			out |= (1u << i);
		}
	}
	return out;
}

// Reconstructs InputCmd::selected_slot (0-based) from a handler's returned
// `selected_slot` field (1-based, matching build_input_table's own
// convention) -- absent/non-number keeps `fallback` (already 0-based)
// unchanged.
std::uint8_t selected_slot_from_table(const sol::table &t, std::uint8_t fallback) {
	const sol::optional<int> one_based = t.get<sol::optional<int>>("selected_slot");
	if (!one_based || *one_based < 1) {
		return fallback;
	}
	return static_cast<std::uint8_t>(*one_based - 1);
}

// Named frame-size variants, exactly architecture_spec/rendering.md §11.3's
// 2026-09-17 table -- an authoring/validation convenience, not an engine
// type: only the resolved {frame_width, frame_height} pixel pair travels the
// wire (protocol::EntityVisualDef), never the variant name itself.
const std::pair<std::uint16_t, std::uint16_t> *entity_visual_variant(const std::string &name) {
	static const std::unordered_map<std::string, std::pair<std::uint16_t, std::uint16_t>> kVariants{
		{ "small", { 128, 128 } },
		{ "tall", { 128, 256 } },
		{ "flat", { 256, 128 } },
		{ "medium", { 256, 256 } },
		{ "medium_tall", { 256, 512 } },
		{ "medium_flat", { 512, 256 } },
		{ "large", { 512, 512 } },
		{ "large_tall", { 512, 1024 } },
		{ "large_flat", { 1024, 512 } },
	};
	const auto it = kVariants.find(name);
	return it == kVariants.end() ? nullptr : &it->second;
}

// Parses and shape-validates `vb.register_entity{visual = {...}}` (spec
// architecture_spec/rendering.md §11.3). Only the pack's *declared* shape is
// checked here -- no image decoding happens on this (pack-runtime, headless)
// side; the real PNG's pixel dimensions are validated client-side against
// this def by render::build_entity_visual_layout() once the texture is
// actually decoded.
// `layer` in a visual / visual_override / attach_to table: an integer in
// [-8, 8] (see protocol::EntityVisualDef::layer).
std::int8_t parse_layer(const std::string &where, const sol::object &obj) {
	if (obj.get_type() != sol::type::number) {
		throw sol::error(where + " must be an integer in [-8, 8]");
	}
	const double v = obj.as<double>();
	if (!(v >= -8.0 && v <= 8.0) || v != std::floor(v)) {
		throw sol::error(where + " must be an integer in [-8, 8]");
	}
	return static_cast<std::int8_t>(v);
}

bool parse_bool(const std::string &where, const sol::object &obj) {
	if (obj.get_type() != sol::type::boolean) {
		throw sol::error(where + " must be a boolean");
	}
	return obj.as<bool>();
}

protocol::EntityVisualDef parse_entity_visual(const sol::table &t) {
	const std::string variant_name = t.get_or("variant", std::string{});
	const auto *variant = entity_visual_variant(variant_name);
	if (variant == nullptr) {
		throw sol::error("vb.register_entity: visual.variant '" + variant_name +
				"' is not a recognized frame-size variant");
	}
	const std::string texture = t.get_or("texture", std::string{});
	if (texture.empty()) {
		throw sol::error("vb.register_entity: visual.texture is required");
	}
	const int facings = t.get_or("facings", 8);
	if (facings != 4 && facings != 8) {
		throw sol::error("vb.register_entity: visual.facings must be 4 or 8");
	}
	const bool mirror = t.get_or("mirror", true);
	float origin_x = 0.5f;
	float origin_y = 1.0f;
	const sol::optional<sol::table> origin_table = t["origin"];
	if (origin_table) {
		origin_x = origin_table->get_or("x", 0.5f);
		origin_y = origin_table->get_or("y", 1.0f);
	}
	if (origin_x < 0.0f || origin_x > 1.0f || origin_y < 0.0f || origin_y > 1.0f) {
		throw sol::error("vb.register_entity: visual.origin.x/y must each be in [0, 1]");
	}
	const sol::optional<sol::table> clips_table = t["clips"];
	if (!clips_table || clips_table->size() == 0) {
		throw sol::error("vb.register_entity: visual.clips must be a non-empty array");
	}

	protocol::EntityVisualDef visual;
	visual.texture = texture;
	visual.frame_width = variant->first;
	visual.frame_height = variant->second;
	visual.facings = static_cast<std::uint8_t>(facings);
	visual.origin_x = origin_x;
	visual.origin_y = origin_y;
	visual.mirror = mirror;

	for (std::size_t i = 1; i <= clips_table->size(); ++i) {
		const sol::table entry = (*clips_table)[i];
		const std::string clip_name = entry.get_or("clip", std::string{});
		if (clip_name.empty()) {
			throw sol::error("vb.register_entity: every visual.clips entry needs a non-empty 'clip' name");
		}
		const int frames = entry.get_or("frames", 0);
		if (frames <= 0) {
			throw sol::error("vb.register_entity: visual.clips['" + clip_name + "'].frames must be positive");
		}
		const float fps = entry.get_or("fps", 0.0f);
		if (fps <= 0.0f) {
			throw sol::error("vb.register_entity: visual.clips['" + clip_name + "'].fps must be positive");
		}
		visual.clips.push_back({ clip_name, static_cast<std::uint16_t>(frames), fps });
	}
	if (const sol::object v = t["layer"]; v.get_type() != sol::type::lua_nil) {
		visual.layer = parse_layer("vb.register_entity: visual.layer", v);
	}
	if (const sol::object v = t["through_walls"]; v.get_type() != sol::type::lua_nil) {
		visual.through_walls = parse_bool("vb.register_entity: visual.through_walls", v);
	}
	return visual;
}

// Parses `vb.world.spawn(kind, pos, {visual_override = {...}})`'s option
// table (spec architecture_spec/rendering.md §11.3's "Per-instance
// override"). Unlike parse_entity_visual above, every field here is
// optional -- an absent one inherits the entity's kind default unchanged
// (render::merge_visual_override does that merge client-side). Still
// shape-validated the same way (variant name recognized, facings 4/8, origin
// in [0,1], clips non-empty with positive frames/fps) whenever a field *is*
// given, so a malformed override fails loudly at spawn time rather than
// silently misrendering later.
protocol::EntityVisualOverride parse_entity_visual_override(const sol::table &t) {
	protocol::EntityVisualOverride out;
	const sol::optional<std::string> variant_name = t["variant"];
	if (variant_name) {
		const auto *variant = entity_visual_variant(*variant_name);
		if (variant == nullptr) {
			throw sol::error("visual_override: variant '" + *variant_name +
					"' is not a recognized frame-size variant");
		}
		out.frame_width = variant->first;
		out.frame_height = variant->second;
	}
	const sol::optional<std::string> texture = t["texture"];
	if (texture) {
		if (texture->empty()) {
			throw sol::error("visual_override: texture must not be empty");
		}
		out.texture = *texture;
	}
	const sol::optional<int> facings = t["facings"];
	if (facings) {
		if (*facings != 4 && *facings != 8) {
			throw sol::error("visual_override: facings must be 4 or 8");
		}
		out.facings = static_cast<std::uint8_t>(*facings);
	}
	const sol::optional<bool> mirror = t["mirror"];
	if (mirror) {
		out.mirror = *mirror;
	}
	const sol::optional<sol::table> origin_table = t["origin"];
	if (origin_table) {
		const float origin_x = origin_table->get_or("x", 0.5f);
		const float origin_y = origin_table->get_or("y", 1.0f);
		if (origin_x < 0.0f || origin_x > 1.0f || origin_y < 0.0f || origin_y > 1.0f) {
			throw sol::error("visual_override: origin.x/y must each be in [0, 1]");
		}
		out.origin_x = origin_x;
		out.origin_y = origin_y;
	}
	const sol::optional<sol::table> clips_table = t["clips"];
	if (clips_table) {
		if (clips_table->size() == 0) {
			throw sol::error("visual_override: clips must be a non-empty array");
		}
		std::vector<protocol::EntityClipDef> clips;
		for (std::size_t i = 1; i <= clips_table->size(); ++i) {
			const sol::table entry = (*clips_table)[i];
			const std::string clip_name = entry.get_or("clip", std::string{});
			if (clip_name.empty()) {
				throw sol::error(
						"visual_override: every clips entry needs a non-empty 'clip' name");
			}
			const int frames = entry.get_or("frames", 0);
			if (frames <= 0) {
				throw sol::error(
						"visual_override: clips['" + clip_name + "'].frames must be positive");
			}
			const float fps = entry.get_or("fps", 0.0f);
			if (fps <= 0.0f) {
				throw sol::error(
						"visual_override: clips['" + clip_name + "'].fps must be positive");
			}
			clips.push_back({ clip_name, static_cast<std::uint16_t>(frames), fps });
		}
		out.clips = std::move(clips);
	}
	if (const sol::object v = t["layer"]; v.get_type() != sol::type::lua_nil) {
		out.layer = parse_layer("visual_override: layer", v);
	}
	if (const sol::object v = t["through_walls"]; v.get_type() != sol::type::lua_nil) {
		out.through_walls = parse_bool("visual_override: through_walls", v);
	}
	return out;
}

// --- entity text labels (vb.world.spawn's `text`, entity:set_text) --------

bool is_valid_utf8(std::string_view s) {
	std::size_t i = 0;
	while (i < s.size()) {
		const auto c = static_cast<unsigned char>(s[i]);
		std::size_t len = 0;
		std::uint32_t cp = 0;
		if (c < 0x80) {
			++i;
			continue;
		} else if ((c & 0xE0) == 0xC0) {
			len = 2;
			cp = c & 0x1F;
		} else if ((c & 0xF0) == 0xE0) {
			len = 3;
			cp = c & 0x0F;
		} else if ((c & 0xF8) == 0xF0) {
			len = 4;
			cp = c & 0x07;
		} else {
			return false;
		}
		if (i + len > s.size()) {
			return false;
		}
		for (std::size_t k = 1; k < len; ++k) {
			const auto cc = static_cast<unsigned char>(s[i + k]);
			if ((cc & 0xC0) != 0x80) {
				return false;
			}
			cp = (cp << 6) | (cc & 0x3F);
		}
		// Reject overlong encodings, surrogates and out-of-range code points.
		if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) ||
				(len == 4 && cp < 0x10000) || cp > 0x10FFFF ||
				(cp >= 0xD800 && cp <= 0xDFFF)) {
			return false;
		}
		i += len;
	}
	return true;
}

std::string check_text_value(const std::string &context, std::string value) {
	if (value.size() > protocol::kMaxEntityTextBytes) {
		throw sol::error(context + ": text is " + std::to_string(value.size()) +
				" bytes; the limit is " + std::to_string(protocol::kMaxEntityTextBytes));
	}
	if (!is_valid_utf8(value)) {
		throw sol::error(context + ": text is not valid UTF-8");
	}
	return value;
}

// `{r, g, b}` or `{r, g, b, a}`, each an integer in [0, 255].
std::array<std::uint8_t, 4> parse_text_rgba(const std::string &context,
		const char *field, const sol::object &obj) {
	const std::string where = context + ": text." + field;
	if (obj.get_type() != sol::type::table) {
		throw sol::error(where + " must be {r, g, b} or {r, g, b, a}");
	}
	const sol::table t = obj.as<sol::table>();
	const std::size_t n = t.size();
	if (n != 3 && n != 4) {
		throw sol::error(where + " must have 3 or 4 components, got " + std::to_string(n));
	}
	std::array<std::uint8_t, 4> out{ 0, 0, 0, 255 };
	for (std::size_t i = 1; i <= n; ++i) {
		const sol::object c = t[i];
		if (c.get_type() != sol::type::number) {
			throw sol::error(where + " components must be numbers");
		}
		const double v = c.as<double>();
		if (!(v >= 0.0 && v <= 255.0) || v != std::floor(v)) {
			throw sol::error(where + " components must be integers in [0, 255]");
		}
		out[i - 1] = static_cast<std::uint8_t>(v);
	}
	return out;
}

float parse_text_number(const std::string &context, const char *field,
		const sol::object &obj, float min, float max, bool min_exclusive) {
	const std::string where = context + ": text." + field;
	if (obj.get_type() != sol::type::number) {
		throw sol::error(where + " must be a number");
	}
	const double v = obj.as<double>();
	if (!std::isfinite(v) || v > max || v < min || (min_exclusive && v == min)) {
		throw sol::error(where + " must be in " + (min_exclusive ? "(" : "[") +
				std::to_string(min) + ", " + std::to_string(max) + "]");
	}
	return static_cast<float>(v);
}

// Applies a `text = {...}` table's fields over `base`, validating each one
// that is present. `value` is left as `base.value` when the table omits it.
// A non-table, non-string `obj` is rejected; a plain string sets only the
// value, keeping every style field of `base`.
protocol::EntityText parse_entity_text(const std::string &context,
		const sol::object &obj, protocol::EntityText base) {
	if (obj.get_type() == sol::type::string) {
		base.value = check_text_value(context, obj.as<std::string>());
		return base;
	}
	if (obj.get_type() != sol::type::table) {
		throw sol::error(context + ": text must be a string or a table");
	}
	const sol::table t = obj.as<sol::table>();
	if (const sol::object v = t["value"]; v.get_type() != sol::type::lua_nil) {
		if (v.get_type() != sol::type::string) {
			throw sol::error(context + ": text.value must be a string");
		}
		base.value = check_text_value(context, v.as<std::string>());
	}
	if (const sol::object v = t["color"]; v.get_type() != sol::type::lua_nil) {
		base.color = parse_text_rgba(context, "color", v);
	}
	if (const sol::object v = t["background"]; v.get_type() != sol::type::lua_nil) {
		if (v.get_type() == sol::type::boolean && !v.as<bool>()) {
			base.background.reset(); // `background = false` turns an inherited panel off
		} else {
			base.background = parse_text_rgba(context, "background", v);
		}
	}
	if (const sol::object v = t["size"]; v.get_type() != sol::type::lua_nil) {
		base.size = parse_text_number(context, "size", v, 0.0f, 16.0f, true);
	}
	if (const sol::object v = t["offset_y"]; v.get_type() != sol::type::lua_nil) {
		base.offset_y = parse_text_number(context, "offset_y", v, -64.0f, 64.0f, false);
	}
	if (const sol::object v = t["max_distance"]; v.get_type() != sol::type::lua_nil) {
		base.max_distance = parse_text_number(context, "max_distance", v, 0.0f, 1024.0f, false);
	}
	if (const sol::object v = t["through_walls"]; v.get_type() != sol::type::lua_nil) {
		if (v.get_type() != sol::type::boolean) {
			throw sol::error(context + ": text.through_walls must be a boolean");
		}
		base.through_walls = v.as<bool>();
	}
	return base;
}

} // namespace

struct BlockDef {
	std::string name;
	core::BlockId id = core::BlockId::kAir;
	sol::protected_function on_break;
	sol::protected_function on_place;
};

struct ItemDef {
	std::string name;
	sol::table raw;
};

struct EntityKindDef {
	std::string name;
	core::EntityKindId id = core::EntityKindId::kInvalid;
	sol::protected_function on_spawn;
	sol::protected_function on_tick;
	sol::protected_function on_hit;
	sol::protected_function on_death;
	// Billboard footprint, metres -- mirrors render::EntityRenderer's
	// placeholder quad dimensions until real per-kind sprite art exists (see
	// REMAINING_TASKS' still-open `visual = {...}` item). Overridable via
	// `vb.register_entity{width=, height=}`; sent to clients as
	// protocol::EntityKindRegistryRecord by install_entity_kind_registry().
	float width = 0.8f;
	float height = 1.8f;
	// Opt-in health tracking (`vb.register_entity{health=...}`). Unset means
	// this kind never gets a Health primitive at all -- entity:damage() stays
	// pure notification (on_hit only, no despawn), matching every kind
	// spawned before this existed. Server-side bookkeeping only, never
	// replicated -- no client HUD reads a script entity's health.
	std::optional<float> max_health;
	// Real per-kind spritesheet (`vb.register_entity{visual = {...}}`,
	// entity-management follow-up to the width/height item above -- spec
	// architecture_spec/rendering.md §11.3's 2026-09-17 schema). Unset means
	// this kind keeps the flat width/height placeholder, exactly as before
	// this existed. Validated for shape (not real pixel dimensions -- no
	// image decoding happens on this, the pack-runtime, side) at
	// registration time; sent to clients as
	// protocol::EntityKindRegistryRecord::visual by
	// install_entity_kind_registry().
	std::optional<protocol::EntityVisualDef> visual;
	// `vb.register_entity{visual = false}`: no sprite or placeholder quad,
	// for a text-only kind (protocol::EntityKindRegistryRecord::hidden).
	bool hidden = false;
	// `vb.register_entity{text = {...}}`: the style (and optional default
	// value) every instance's label starts from. Its offset_y defaults to
	// just above the kind's height (0 for a hidden kind).
	protocol::EntityText text_style;
};

// Phase 6.1: one spawned `vb.world.spawn(kind, pos)` instance. `self` is a
// plain Lua table (the spec's `ScriptState`) that on_spawn/on_tick/on_hit/
// on_death all receive as their first argument and that persists across
// calls, so it works as real instance state -- not just a fresh handle
// rebuilt per call the way PlayerHandle is. Its metatable's __index points at
// the shared `entity_methods` table (built once in install_bindings), so
// base-component accessors (get_pos/set_pos/...) are reachable as
// `self:get_pos()` while arbitrary fields (`self.hp = 10`) live directly on
// the table with no collision unless a pack picks a method's exact name --
// the "arbitrary fields for custom data, accessors for engine-owned
// components" split REMAINING_TASKS.md 6.1 leaned toward.
//
// No generic EnTT registry backs this (same posture as ItemDropSystem,
// world/item_drops.hpp): kind/self/position live in this map, replicated
// through ServerSession::spawn_script_entity's interest-grid entry exactly
// like a dropped item is, with no dedicated wire message.
struct ScriptEntity {
	std::size_t kind_index = 0; // index into Impl::entity_kinds
	sol::table self;
	core::Vec3d pos{};
	// Current health, only set if this instance's kind opted into
	// `vb.register_entity{health=...}` -- unset (nullopt) for every other
	// kind, so entity:damage()/get_health() can tell "not tracked" apart
	// from "tracked and at 0" (which is mid-despawn, not a valid steady state).
	std::optional<float> health;
	// The label this instance currently shows (already sent to
	// ServerSession::set_script_entity_text); nullopt = none.
	std::optional<protocol::EntityText> text;
	// entity:attach_to's parent link (mirrors ServerSession's copy), kept here
	// so attach_to can reject a cycle.
	std::optional<protocol::EntityAttachment> attach;
};

struct BiomeDef {
	std::string name;
	sol::table raw;
};

struct CraftDef {
	sol::table raw;
};

struct PackRuntime::Impl {
	net::Transport &transport;
	world::BlockRegistry &registry;
	std::filesystem::path storage_path;
	// Phase 6.4: vb.db, a generic per-key store distinct from the single
	// pack-global `storage` blob below -- see vb/script/db.hpp. Declared
	// after storage_path (construction order == declaration order) so its
	// root can be derived from storage_path's parent directory.
	ScriptDb db;
	Vm vm;
	bool frozen = false;
	net::WorldReplicator *replicator = nullptr;
	net::ServerSession *session = nullptr;

	nlohmann::json storage;
	bool storage_dirty_flag = false;
	std::uint64_t storage_revision_counter = 0;

	std::vector<BlockDef> blocks;
	std::vector<ItemDef> items;
	std::vector<EntityKindDef> entity_kinds;
	// Entity-management follow-up: `vb.register_entity{represents=...}` lets a
	// kind opt into standing in for an engine-managed entity that doesn't go
	// through vb.world.spawn at all (players, ItemDropSystem drops) purely for
	// replication-time cosmetics (S2C_EntityKindRegistry's width/height/
	// visual) -- see PackRuntime::attach_session(), which forwards these into
	// ServerSession::set_player_visual_kind()/set_item_drop_visual_kind().
	// nullopt (the default) means no pack claimed the role, and both session
	// setters are simply never called -- exact pre-existing behavior.
	std::optional<core::EntityKindId> player_kind_id;
	std::optional<core::EntityKindId> item_drop_kind_id;
	std::vector<BiomeDef> biomes;
	// Structure editor S1: every vb.register_structure call, in order.
	std::vector<worldgen::StructureSpec> structure_specs;
	std::vector<CraftDef> crafts;
	// Phase 6.3: vb.register_keybind names, order == bit index into every
	// InputCmd::keybinds -- capped at S2CKeybindRegistry::kMaxKeybinds so the
	// bitset always fits one uint32_t.
	std::vector<std::string> keybind_names;
	// Phase 6.7: the raw table passed to vb.physics.set_params{...}, if a
	// pack ever calls it. Read field-by-field (with get_or) in
	// effective_move_params() rather than converted eagerly, so a field the
	// pack didn't set naturally falls back to whatever base MoveParams the
	// caller passes in at that point -- not a fixed literal baked in here.
	std::optional<sol::table> move_params_table;

	// Phase 6.18: the raw table passed to vb.combat.set_params{...}, if a
	// pack ever calls it -- same "capture the table, parse lazily" posture
	// as move_params_table above.
	std::optional<sol::table> combat_params_table;

	// Phase 6.21: the raw table passed to vb.action.set_params{reach=...}, if
	// a pack ever calls it -- same "capture the table, parse lazily" posture
	// as move_params_table/combat_params_table above. Unifies what used to be
	// combat_params_table's own 'reach' field and WorldReplicator's hardcoded
	// constant into the one value effective_action_params() resolves.
	std::optional<sol::table> action_params_table;

	// REMAINING_TASKS.md's hunger gap: the raw table passed to
	// vb.hunger.set_params{...}, if a pack ever calls it -- same
	// "capture the table, parse lazily" posture as the params tables above.
	std::optional<sol::table> hunger_params_table;

	// Phase 6.8: the raw table passed to vb.daynight.set_curve{keyframes =
	// {...}}, if a pack ever calls it. Parsed into a world::DayNightCurve in
	// effective_day_night_curve() rather than eagerly, matching
	// move_params_table's posture above.
	std::optional<sol::table> day_night_curve_table;
	// Phase 6.8: the value passed to vb.daynight.set_day_length(seconds), if
	// a pack ever calls it.
	std::optional<double> day_length_seconds_override;

	// Phase 7.2: the raw table passed to vb.render.set_fog{start=, end=}, if
	// a pack ever calls it -- same "capture the table, parse lazily" posture
	// as move_params_table/day_night_curve_table above.
	std::optional<sol::table> fog_params_table;

	// Phase 6.13: the operator's ServerConfig, if set_server_config() was ever
	// called (real servers call it; --singleplayer's in-process PackRuntime
	// never does, so vb.config.get returns nil there). Read-only from Lua --
	// no setter is exposed to a pack, unlike 6.6-6.11's override tables above.
	std::optional<core::ServerConfig> server_config;

	// Phase 6.14: the raw table passed to vb.worldgen.set_pipeline{...}, if a
	// pack ever calls it -- read once (not per-voxel) in
	// build_worldgen_pipeline(), same "capture the table, parse lazily"
	// posture as move_params_table/day_night_curve_table above.
	// `set_pipeline` itself eagerly validates 'height' is present (same
	// "validate at the call site" posture 6.8's set_curve uses for its own
	// required 'keyframes' field) since a pack that opts in without a height
	// field almost certainly has a bug, not an intentional no-op.
	std::optional<sol::table> worldgen_pipeline_table;

	// Phase 6.1: spawned vb.register_entity instances, keyed by the NetId
	// ServerSession::spawn_script_entity handed back. entity_mt is the shared
	// metatable (__index = entity_methods) applied to every spawned `self`.
	std::unordered_map<core::NetId, ScriptEntity> entities;
	sol::table entity_methods;
	sol::table entity_mt;

	std::unordered_map<std::string, std::vector<sol::protected_function>> handlers;

	struct Timer {
		double remaining;
		double period;
		bool repeating;
		sol::protected_function fn;
		bool cancelled = false;
	};
	std::vector<Timer> timers;

	std::unordered_map<core::NetId, ecs::Inventory> inventories;
	std::unordered_map<core::NetId, std::string> player_names;
	// Where a leaving player was when the session dropped them; set only
	// while player_leave runs, so get_pos() can still answer.
	std::unordered_map<core::NetId, physics::MoveState> leaving_states;
	// External auth (auth.md §6). `logins` is filled at join and kept until
	// after player_leave fires, so every callback that gets a Player can read
	// its login even once the session has dropped the connection.
	bool auth_required = false;
	std::unordered_map<core::NetId, std::shared_ptr<const net::LoginData>> logins;
	std::unordered_map<core::NetId, sol::object> login_tables;

	std::shared_ptr<const net::LoginData> login_for(core::NetId id) const {
		if (auto it = logins.find(id); it != logins.end()) {
			return it->second;
		}
		return session != nullptr ? session->player_login(id) : nullptr;
	}
	sol::object login_object(core::NetId id, sol::state_view lua) {
		const auto login = login_for(id);
		if (!login) {
			return sol::make_object(lua, sol::lua_nil);
		}
		if (auto it = login_tables.find(id); it != login_tables.end()) {
			return it->second;
		}
		sol::object o = login_to_lua(lua, *login);
		login_tables[id] = o;
		return o;
	}

	Impl(net::Transport &t, world::BlockRegistry &reg,
			std::filesystem::path path, VmLimits limits);

	sol::state &lua_state() { return vm.native_impl().lua; }

	void install_bindings();
	void dispatch_tick(double dt);
	void flush_storage();
	bool on_block_edit_before(core::NetId editor, core::IVec3 pos,
			core::BlockId existing, core::BlockId new_block, bool is_break);
	void on_block_edit_after(core::NetId editor, core::IVec3 pos,
			core::BlockId removed, core::BlockId placed, bool is_break);
	// Phase 5.1: push a full S2C_Inventory snapshot to `id`'s connection, if
	// one exists. No-op (not an error) if the session/connection isn't ready
	// yet -- same posture as send_message/open_ui below.
	void sync_inventory(core::NetId id);
	// Phase 6.9: the one place that actually adds items to an inventory --
	// fills existing under-cap slots for `item` first (registry's
	// `max_stack`, or the engine default for an id the registry doesn't
	// know), then starts as many new slots as needed for the remainder.
	// Shared by PlayerHandle::give() and the item-pickup handler
	// (attach_session()) so picking something up stacks identically to a
	// script handing it to you directly. Does not call sync_inventory --
	// callers push their own snapshot once, after any other bookkeeping.
	void give_item(core::NetId id, core::BlockId item, std::uint16_t count);
	// Phase 6.6: spawns every slot of `id`'s inventory as a dropped item at
	// `pos` and empties it. Called by run_respawn_handler when a
	// vb.on("player_death", ...) handler's returned table asks for
	// drop_inventory = true.
	void drop_all_items(core::NetId id, core::Vec3d pos);
	// Phase 6.6: calls the first registered vb.on("player_death", ...)
	// handler (if any) and turns its returned table into a
	// ServerSession::RespawnDecision; falls back to a full heal at the join
	// spawn point if no handler is registered or none returns a table.
	net::ServerSession::RespawnDecision run_respawn_handler(
			core::NetId id, std::string_view cause, float health_before);

	// Phase 6.3: runs every vb.on("player_input", handler) in registration
	// order, chaining replacements (each handler sees the prior one's
	// output) and short-circuiting on the first `false` veto. Builds the
	// Lua-facing input table (move/yaw/pitch/buttons/keybinds-by-name) fresh
	// per handler call from the current working values.
	net::ServerSession::InputHookResult run_player_input(
			core::NetId id, const protocol::InputCmd &cmd);

	// Phase 6.10: runs every vb.on("chat", handler) in registration order,
	// same chaining shape as run_player_input but for a single string field
	// instead of a table -- a handler returns `false` to veto, a string to
	// replace the text seen by the next handler (and ultimately broadcast),
	// or true/nil/anything else to pass the current text through unchanged.
	net::ServerSession::ChatHookResult run_chat(
			core::NetId sender, std::string_view text);

	// Phase 6.5 (spec §10.7): shared block-damage breaking. See
	// net::ServerSession::BlockBreakHooks for the calling contract each of
	// these implements.
	bool run_block_break_begin(core::NetId player, core::IVec3 pos);
	float run_block_break_tick(
			core::NetId player, core::IVec3 pos, std::uint16_t max_damage);
	std::optional<float> run_block_health_tick(core::IVec3 pos, float damage,
			std::uint16_t max_damage, std::uint64_t ticks_since_last_hit);

	// Phase 7.3: fires vb.on("region_enter"/"region_exit", player, pos,
	// block_name) -- generic occupancy notification, no veto/return value
	// (see net::ServerSession::RegionHooks for the calling contract).
	void run_region_event(
			const std::string &event, core::NetId player, core::IVec3 pos, core::BlockId block);

	// Phase 6.22: fires vb.on("player_landed", player, impact_speed) -- raw
	// notification only (see net::ServerSession::set_landed_hook's own
	// comment for why the engine computes and reports this value but ships no
	// fall-damage formula itself).
	void run_landed_event(core::NetId player, double impact_speed);

	// Phase 6.1: vb.world.spawn / self:damage / self:remove dispatch. See
	// ScriptEntity's comment above for the overall design.
	core::NetId self_net_id(const sol::table &self) const;
	void dispatch_entity_tick(double dt);
	void dispatch_entity_hit(core::NetId id, double amount, std::string_view cause);
	void despawn_entity(core::NetId id, std::string_view cause);

	// Phase 6.14: parses one vb.noise.*-built node table (recursively, for
	// fbm/remap/combine's nested `source`/`a`/`b` fields) into the pure-C++
	// worldgen::NoiseNode IR. `salt` is a shared, ever-incrementing counter
	// across one whole parse so no two nodes in the same graph sample
	// identically (see NoiseNode::salt's own comment).
	worldgen::NoiseNodePtr parse_noise_node(
			const sol::table &t, std::uint64_t &salt) const;
	// Phase 6.14: compiles worldgen_pipeline_table + every registered biome
	// into an immutable worldgen::PackWorldGenPipeline, once. nullptr if
	// set_pipeline was never called (caller keeps WorldGenerator on its fixed
	// default path). See PackRuntime::build_worldgen_pipeline's own comment.
	std::shared_ptr<const worldgen::PackWorldGenPipeline> build_worldgen_pipeline(
			const worldgen::WorldGenParams &base) const;
	// Structure editor S1: resolves registered structures and parses every
	// biome's `decoration` against the (frozen) block registry. Returns an
	// error string naming the offender, or empty on success.
	std::string build_decoration(std::vector<worldgen::StructureDef> &structures,
			std::vector<std::vector<worldgen::PlacementRule>> &decoration) const;

	template <typename... Args>
	void fire(const std::string &event, Args &&...args) {
		auto it = handlers.find(event);
		if (it == handlers.end()) {
			return;
		}
		for (auto &fn : it->second) {
			if (!fn.valid()) {
				continue;
			}
			vm.begin_call_budget();
			sol::protected_function_result r = fn(args...);
			if (!r.valid()) {
				const sol::error e = r;
				VB_WARN("script", "vb.on('", event, "') handler error: ", e.what());
			}
		}
	}

	template <typename... Args>
	bool run_veto(const std::string &event, Args &&...args) {
		auto it = handlers.find(event);
		if (it == handlers.end()) {
			return true;
		}
		for (auto &fn : it->second) {
			if (!fn.valid()) {
				continue;
			}
			vm.begin_call_budget();
			sol::protected_function_result r = fn(args...);
			if (!r.valid()) {
				const sol::error e = r;
				VB_WARN("script", "vb.on('", event, "') handler error: ", e.what());
				continue;
			}
			const sol::object ret = r;
			if (ret.valid() && ret.get_type() == sol::type::boolean &&
					!ret.as<bool>()) {
				return false;
			}
		}
		return true;
	}
};

// Lightweight Lua-visible handle for a connected player. Not a persistent
// object -- constructed fresh per dispatch call, so it can never dangle
// (Impl outlives every dispatch call). No generic non-player "entity"
// concept exists yet (Phase 3.1 defers the EnTT registry), so this one type
// covers both the spec's `entity:` and `player:` method surfaces.
struct PlayerHandle {
	core::NetId net_id = core::NetId::kInvalid;
	PackRuntime::Impl *rt = nullptr;

	sol::object get_pos(sol::this_state ts) const {
		sol::state_view lua(ts);
		if (rt->session == nullptr) {
			throw sol::error("entity:get_pos(): session not attached yet");
		}
		auto st = rt->session->player_move_state(net_id);
		if (!st) {
			if (const auto it = rt->leaving_states.find(net_id); it != rt->leaving_states.end()) {
				st = it->second; // player_leave: the last known position
			}
		}
		if (!st) {
			throw sol::error("entity:get_pos(): entity is gone");
		}
		sol::table t = lua.create_table();
		t["x"] = st->position.x;
		t["y"] = st->position.y;
		t["z"] = st->position.z;
		return t;
	}

	// Teleport: feet position, velocity zeroed, look direction kept. The
	// owning client snaps to it like any authoritative correction.
	void set_pos(double x, double y, double z) const {
		if (rt->session == nullptr) {
			throw sol::error("player:set_pos(): session not attached yet");
		}
		if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
			throw sol::error("player:set_pos(): coordinates must be finite numbers");
		}
		if (!rt->session->teleport_player(net_id, { x, y, z })) {
			throw sol::error("player:set_pos(): player is gone");
		}
	}

	// The feet position this player was granted at join -- where the
	// engine's default respawn puts them.
	sol::object get_spawn_pos(sol::this_state ts) const {
		sol::state_view lua(ts);
		if (rt->session == nullptr) {
			throw sol::error("player:get_spawn_pos(): session not attached yet");
		}
		if (!rt->session->player_move_state(net_id)) {
			throw sol::error("player:get_spawn_pos(): player is gone");
		}
		const core::Vec3d p = rt->session->spawn_point(net_id);
		sol::table t = lua.create_table();
		t["x"] = p.x;
		t["y"] = p.y;
		t["z"] = p.z;
		return t;
	}

	void set_velocity(double x, double y, double z) const {
		if (rt->session == nullptr) {
			throw sol::error("entity:set_velocity(): session not attached yet");
		}
		rt->session->set_player_velocity(net_id, { x, y, z });
	}

	void remove() const {
		VB_WARN("script", "entity:remove() is a no-op for player-backed "
						  "handles -- no generic entity registry exists yet "
						  "(Phase 3.1)");
	}

	sol::object get_inventory(sol::this_state ts) const {
		sol::state_view lua(ts);
		const ecs::Inventory &inv = rt->inventories[net_id];
		sol::table t = lua.create_table();
		int i = 1;
		for (const auto &stack : inv.slots) {
			sol::table s = lua.create_table();
			s["item"] = static_cast<std::uint16_t>(stack.item);
			s["count"] = stack.count;
			t[i++] = s;
		}
		return t;
	}

	// Entity-management follow-up (held item / hotbar selection, Phase
	// 6.20): 1-based, matching get_inventory()'s own 1-based array -- a pack
	// never has to think in the wire's 0-based InputCmd::selected_slot.
	int get_selected_slot() const {
		if (rt->session == nullptr) {
			return 1;
		}
		return static_cast<int>(rt->session->selected_slot(net_id)) + 1;
	}

	// The inventory slot currently selected, or nil if that slot is out of
	// range or empty -- resolves get_selected_slot() against this player's
	// actual inventory (the engine has no idea a "hotbar" exists, only a
	// selected index; a pack decides what holding an empty slot means).
	sol::object get_held_item(sol::this_state ts) const {
		sol::state_view lua(ts);
		const std::vector<ecs::ItemStack> &slots = rt->inventories[net_id].slots;
		const int index = get_selected_slot() - 1;
		if (index < 0 || static_cast<std::size_t>(index) >= slots.size()) {
			return sol::lua_nil;
		}
		const ecs::ItemStack &stack = slots[static_cast<std::size_t>(index)];
		if (stack.item == core::BlockId::kAir || stack.count == 0) {
			return sol::lua_nil;
		}
		sol::table s = lua.create_table();
		s["item"] = static_cast<std::uint16_t>(stack.item);
		s["count"] = stack.count;
		return s;
	}

	void send_message(std::string_view text) const {
		if (rt->session == nullptr) {
			return;
		}
		const net::ConnId conn = rt->session->conn_for_player(net_id);
		if (conn == net::ConnId::kInvalid) {
			return;
		}
		net::send_message(rt->transport, conn, protocol::S2CChat{ std::string(text) });
	}

	void open_ui(std::string_view name, sol::optional<sol::table> ctx) const {
		if (rt->session == nullptr) {
			return;
		}
		const net::ConnId conn = rt->session->conn_for_player(net_id);
		if (conn == net::ConnId::kInvalid) {
			return;
		}
		std::string ctx_json = "{}";
		if (ctx) {
			ctx_json = lua_to_json(*ctx).dump();
		}
		net::send_message(rt->transport, conn,
				protocol::S2COpenUi{ std::string(name), std::move(ctx_json) });
	}

	void give(sol::table itemstack) const {
		const auto item = itemstack.get_or("item", static_cast<std::uint16_t>(0));
		const auto count = itemstack.get_or("count", static_cast<std::uint16_t>(0));
		rt->give_item(net_id, static_cast<core::BlockId>(item), count);
		rt->sync_inventory(net_id);
	}

	// The generic counterpart to give() -- removes up to `count` of `item`
	// across however many slots hold it, only if the player has enough in
	// total (all-or-nothing, no partial consumption). Not in the original
	// spec (§10.3 lists only `give`), added because content-side systems
	// like crafting (content/base/crafting.lua) need a way to spend
	// ingredients; this stays a generic inventory primitive, not anything
	// crafting-specific -- the engine has no idea what a "recipe" is.
	bool take(sol::table itemstack) const {
		const auto item =
				static_cast<core::BlockId>(itemstack.get_or("item", static_cast<std::uint16_t>(0)));
		const auto count = itemstack.get_or("count", static_cast<std::uint16_t>(0));
		std::vector<ecs::ItemStack> &slots = rt->inventories[net_id].slots;
		std::uint32_t available = 0;
		for (const auto &s : slots) {
			if (s.item == item) {
				available += s.count;
			}
		}
		if (available < count) {
			return false;
		}
		std::uint16_t remaining = count;
		for (auto it = slots.begin(); it != slots.end() && remaining > 0;) {
			if (it->item != item) {
				++it;
				continue;
			}
			const std::uint16_t taken = std::min(remaining, it->count);
			it->count -= taken;
			remaining -= taken;
			if (it->count == 0) {
				it = slots.erase(it);
			} else {
				++it;
			}
		}
		rt->sync_inventory(net_id);
		return true;
	}

	// nil when the server isn't authenticating; otherwise a frozen table of
	// user data (auth.md §6). Never nil on an authenticating server.
	sol::object get_login(sol::this_state ts) const {
		return rt->login_object(net_id, sol::state_view(ts));
	}

	std::string get_name() const {
		if (rt->session != nullptr) {
			if (const std::string_view name = rt->session->player_name(net_id); !name.empty()) {
				return std::string(name);
			}
		}
		// Kept until after player_leave, when the session no longer has the conn.
		if (const auto it = rt->player_names.find(net_id); it != rt->player_names.end()) {
			return it->second;
		}
		return {};
	}

	// Phase 6.6: the one way to reduce a player's health from Lua. `cause` is
	// an opaque string (e.g. "fall", "pvp") threaded through unchanged to a
	// vb.on("player_death", ...) handler once health reaches 0 -- the engine
	// takes no position on what "fall"/"pvp" mean.
	void damage(float amount, sol::optional<std::string> cause) const {
		if (rt->session == nullptr) {
			throw sol::error("entity:damage(): session not attached yet");
		}
		rt->session->damage_player(net_id, amount, cause.value_or(std::string{}));
	}

	// REMAINING_TASKS.md's hunger gap: reads the current value of the
	// per-player Hunger stat ServerSession::update_hunger() owns. nil if
	// the player is already gone (same "nil, not an error, for a
	// disappeared target" posture entity:get_health() uses for an
	// untracked script-entity kind).
	sol::object get_hunger(sol::this_state ts) const {
		sol::state_view lua(ts);
		if (rt->session == nullptr) {
			throw sol::error("entity:get_hunger(): session not attached yet");
		}
		const auto h = rt->session->player_hunger(net_id);
		if (!h) {
			return sol::lua_nil;
		}
		return sol::make_object(lua, *h);
	}

	// `{current=, max=}`, the same shape entity:get_health() returns for a
	// script entity, or nil if the player is already gone. Read-only: lowering
	// goes through damage() (so death/respawn hooks fire), there is no setter.
	sol::object get_health(sol::this_state ts) const {
		sol::state_view lua(ts);
		if (rt->session == nullptr) {
			throw sol::error("entity:get_health(): session not attached yet");
		}
		const auto h = rt->session->player_health(net_id);
		if (!h) {
			return sol::lua_nil;
		}
		sol::table t = lua.create_table();
		t["current"] = h->first;
		t["max"] = h->second;
		return t;
	}

	// A pack-facing way to spend or restore hunger directly (eating food,
	// a sprint cost, ...) without waiting on decay_per_second -- clamped to
	// [0, max] by ServerSession::add_player_hunger(), a negative `amount`
	// spends hunger the same way a positive one restores it.
	void add_hunger(float amount) const {
		if (rt->session == nullptr) {
			throw sol::error("entity:add_hunger(): session not attached yet");
		}
		rt->session->add_player_hunger(net_id, amount);
	}

	// Phase 6.17: breaking a block is no longer something the engine does on
	// its own -- the client only ever reports raw input (buttons.primary,
	// where it's looking via yaw/pitch); a pack decides whether/when that
	// actually removes a block. This runs the exact same validated pipeline
	// a real C2S_BlockEdit would (reach check, before/after block-edit hooks,
	// per-block on_break, item drops, relight, delta fan-out to every
	// mirroring client) instead of a shortcut -- it's just triggered from
	// Lua instead of decoded off the wire. Returns whether the edit was
	// actually accepted (false on an out-of-reach/invalid target, same as a
	// rejected C2S_BlockEdit).
	bool break_block(int x, int y, int z) const {
		if (rt->session == nullptr) {
			throw sol::error("entity:break_block(): session not attached yet");
		}
		return rt->session->apply_script_block_edit(
				net_id, protocol::BlockEditAction::kBreak, { x, y, z });
	}

	// Right-click placing used to be the one remaining hardcoded block edit
	// (src/client/main.cpp sent a C2S_BlockEdit{kPlace, base_block::stone}
	// directly off MOUSE_BUTTON_RIGHT, with no pack seam at all -- the exact
	// gap 6.17's own writeup flagged as "Placing (RMB, always instant) is
	// unaffected"). Same shape as break_block() above: the engine only
	// exposes the validated primitive (reach check, hooks, fan-out), a pack
	// decides *when* to place and *what* block (content/base/mechanics.lua
	// does both from a rising edge of input.buttons.secondary, same
	// was_down/edge-detect idiom the punch handler already uses).
	bool place_block(int x, int y, int z, int block) const {
		if (rt->session == nullptr) {
			throw sol::error("entity:place_block(): session not attached yet");
		}
		return rt->session->apply_script_block_edit(net_id,
				protocol::BlockEditAction::kPlace, { x, y, z },
				static_cast<core::BlockId>(block));
	}

	// Phase 6.18 (Growtopia-style combat): the one call a pack needs to
	// throw a discrete punch -- call it once per rising edge of whatever key
	// a pack binds to "attack" (a vb.on("player_input", ...) handler
	// deciding *when*, exactly like content/base/mechanics.lua does); the
	// engine decides *what got hit* (raycasts blocks and nearby players
	// along this player's authoritative look direction, picks whichever is
	// closer) and applies the default policy (instant PvP damage, or an
	// accumulating per-block punch count that breaks the block once it
	// reaches BlockType::max_damage -- 0 there still means "one punch").
	// Returns a table describing what happened: {hit_player=bool,
	// target=<Player>|nil, hit_block=bool, x=,y=,z=, punches=, broken=bool}
	// -- a pack that wants swing VFX/sound or a hit-marker reads this;
	// one that doesn't can ignore the return value entirely.
	//
	// `block_damage` (optional, default 1): REMAINING_TASKS.md's "vary
	// break time by block/tool" gap -- the engine has no tool/hardness
	// concept of its own, so this is the one knob it exposes: a pack
	// deciding how much a given swing counts for against
	// BlockType::max_damage (e.g. looking up the puncher's own
	// player:get_held_item() against a Lua-side tool table) gets a real
	// faster-or-slower break without the engine needing to know what a
	// "tool" even is. PvP damage is unaffected by this -- that's
	// vb.combat.set_params's player_damage, a separate knob.
	sol::table punch(sol::this_state ts, sol::optional<int> block_damage) const {
		sol::state_view lua(ts);
		if (rt->session == nullptr) {
			throw sol::error("entity:punch(): session not attached yet");
		}
		const int dmg = block_damage.value_or(1);
		if (dmg < 1 || dmg > 0xFFFF) {
			throw sol::error("entity:punch(): block_damage must be between 1 and 65535");
		}
		const auto r = rt->session->punch(net_id, static_cast<std::uint16_t>(dmg));
		sol::table t = lua.create_table();
		t["hit_player"] = r.hit_player;
		if (r.hit_player) {
			t["target"] = PlayerHandle{ r.target, rt };
		}
		t["hit_block"] = r.hit_block;
		if (r.hit_block) {
			t["x"] = r.block_pos.x;
			t["y"] = r.block_pos.y;
			t["z"] = r.block_pos.z;
			t["punches"] = r.block_punches;
			t["broken"] = r.block_broken;
		}
		return t;
	}
};

PackRuntime::Impl::Impl(net::Transport &t, world::BlockRegistry &reg,
		std::filesystem::path path, VmLimits limits) : transport(t),
													   registry(reg),
													   storage_path(std::move(path)),
													   db(storage_path.parent_path() / "db"),
													   vm(limits) {
	std::ifstream in(storage_path);
	if (in) {
		try {
			in >> storage;
		} catch (const std::exception &e) {
			VB_WARN("script", "vb.storage: '", storage_path.string(),
					"' is malformed JSON (", e.what(), "), starting empty");
			storage = nlohmann::json::object();
		}
	} else {
		storage = nlohmann::json::object();
	}
	install_bindings();

	// Phase 6.19: pre-register the engine's own movement/action names into
	// the same Phase 6.3 keybind registry vb.register_keybind() writes into,
	// before any pack script runs -- so client/src/client/main.cpp's
	// sample_input_cmd() can find them by name in the S2C_KeybindRegistry
	// this pushes out, and any pack's vb.on("player_input", ...) can read
	// e.g. input.keybinds["jump"] the same way it reads a custom keybind.
	// Purely additive: InputCmd::buttons/move (and their input.buttons/move
	// Lua exposure) are unchanged, so nothing that already reads those
	// breaks. A pack calling vb.register_keybind() with one of these names
	// just gets the same index back (register_keybind is idempotent by
	// name), it can't shadow or duplicate these.
	for (const char *name : { "move_forward", "move_back", "move_left",
				 "move_right", "jump", "sprint", "primary", "secondary" }) {
		keybind_names.emplace_back(name);
	}
}

void PackRuntime::Impl::install_bindings() {
	sol::state &lua = lua_state();

	lua.new_usertype<PlayerHandle>("Player", "get_pos", &PlayerHandle::get_pos,
			"set_pos", &PlayerHandle::set_pos, "get_spawn_pos",
			&PlayerHandle::get_spawn_pos, "set_velocity", &PlayerHandle::set_velocity, "remove",
			&PlayerHandle::remove, "get_inventory", &PlayerHandle::get_inventory,
			"send_message", &PlayerHandle::send_message, "open_ui",
			&PlayerHandle::open_ui, "give", &PlayerHandle::give, "take",
			&PlayerHandle::take, "get_name", &PlayerHandle::get_name, "get_login",
			&PlayerHandle::get_login, "damage",
			&PlayerHandle::damage, "break_block", &PlayerHandle::break_block,
			"place_block", &PlayerHandle::place_block, "punch",
			&PlayerHandle::punch, "get_selected_slot",
			&PlayerHandle::get_selected_slot, "get_held_item",
			&PlayerHandle::get_held_item, "get_hunger", &PlayerHandle::get_hunger,
			"add_hunger", &PlayerHandle::add_hunger, "get_health",
			&PlayerHandle::get_health);

	sol::table vb = lua.create_named_table("vb");

	// vb.auth.required(): true iff auth.lua is active on this server.
	{
		sol::table auth = lua.create_table();
		auth["required"] = [this]() { return auth_required; };
		vb["auth"] = auth;
	}

	vb["register_block"] = [this](sol::table def) -> std::uint16_t {
		if (frozen) {
			throw sol::error("vb.register_block: registry already frozen");
		}
		const std::string name = def.get_or("name", std::string{});
		if (name.empty()) {
			throw sol::error("vb.register_block: 'name' is required");
		}
		world::BlockType type = parse_block_type(def);
		const core::BlockId id = register_block_type(registry, type);
		auto it = std::find_if(blocks.begin(), blocks.end(),
				[&](const BlockDef &b) { return b.name == name; });
		if (it == blocks.end()) {
			blocks.push_back({ name, id, {}, {} });
			it = blocks.end() - 1;
		}
		it->id = id;
		it->on_break = def.get_or("on_break", sol::protected_function{});
		it->on_place = def.get_or("on_place", sol::protected_function{});
		return static_cast<std::uint16_t>(id);
	};

	vb["register_item"] = [this](sol::table def) {
		if (frozen) {
			throw sol::error("vb.register_item: registry already frozen");
		}
		items.push_back({ def.get_or("name", std::string{}), def });
	};

	vb["register_entity"] = [this](sol::table def) -> std::uint16_t {
		if (frozen) {
			throw sol::error("vb.register_entity: registry already frozen");
		}
		const std::string name = def.get_or("name", std::string{});
		if (name.empty()) {
			throw sol::error("vb.register_entity: 'name' is required");
		}
		for (const auto &e : entity_kinds) {
			if (e.name == name) {
				return static_cast<std::uint16_t>(e.id);
			}
		}
		EntityKindDef e;
		e.name = name;
		e.id = static_cast<core::EntityKindId>(entity_kinds.size() + 1);
		e.on_spawn = def.get_or("on_spawn", sol::protected_function{});
		e.on_tick = def.get_or("on_tick", sol::protected_function{});
		e.on_hit = def.get_or("on_hit", sol::protected_function{});
		e.on_death = def.get_or("on_death", sol::protected_function{});
		e.width = def.get_or("width", e.width);
		e.height = def.get_or("height", e.height);
		const sol::optional<float> health = def["health"];
		if (health && *health <= 0.0f) {
			throw sol::error("vb.register_entity: 'health' must be positive");
		}
		if (health) {
			e.max_health = *health;
		}
		const sol::object visual_obj = def["visual"];
		if (visual_obj.get_type() == sol::type::boolean && !visual_obj.as<bool>()) {
			e.hidden = true;
		} else if (visual_obj.get_type() == sol::type::table) {
			e.visual = parse_entity_visual(visual_obj.as<sol::table>());
		} else if (visual_obj.get_type() != sol::type::lua_nil) {
			throw sol::error("vb.register_entity: 'visual' must be a table or false");
		}
		e.text_style.offset_y = e.hidden ? 0.0f : e.height + 0.25f;
		if (const sol::object text_obj = def["text"];
				text_obj.get_type() != sol::type::lua_nil) {
			if (text_obj.get_type() != sol::type::table) {
				throw sol::error("vb.register_entity: 'text' must be a table");
			}
			e.text_style = parse_entity_text("vb.register_entity", text_obj, e.text_style);
		}
		const sol::optional<std::string> represents = def["represents"];
		if (represents) {
			if (*represents == "player") {
				if (player_kind_id) {
					throw sol::error(
							"vb.register_entity: 'player' is already claimed by another kind's represents=");
				}
				player_kind_id = e.id;
			} else if (*represents == "item_drop") {
				if (item_drop_kind_id) {
					throw sol::error(
							"vb.register_entity: 'item_drop' is already claimed by another kind's represents=");
				}
				item_drop_kind_id = e.id;
			} else {
				throw sol::error("vb.register_entity: 'represents' must be 'player' or 'item_drop', got '" +
						*represents + "'");
			}
		}
		entity_kinds.push_back(std::move(e));
		return static_cast<std::uint16_t>(entity_kinds.back().id);
	};

	vb["register_biome"] = [this](sol::table def) {
		if (frozen) {
			throw sol::error("vb.register_biome: registry already frozen");
		}
		biomes.push_back({ def.get_or("name", std::string{}), def });
	};

	vb["register_structure"] = [this](sol::table def) {
		if (frozen) {
			throw sol::error("vb.register_structure: registry already frozen");
		}
		worldgen::StructureSpec spec = parse_structure(def);
		for (const auto &existing : structure_specs) {
			if (existing.name == spec.name) {
				throw sol::error("vb.register_structure: structure '" + spec.name +
						"' is already registered");
			}
		}
		structure_specs.push_back(std::move(spec));
	};

	vb["register_craft"] = [this](sol::table def) {
		if (frozen) {
			throw sol::error("vb.register_craft: registry already frozen");
		}
		crafts.push_back({ def });
	};

	// Phase 6.3: closed-schema custom keybind. Idempotent by name (like
	// register_entity), order-assigned index = bit position in every
	// InputCmd::keybinds -- capped so the bitset always fits one uint32_t.
	vb["register_keybind"] = [this](const std::string &name) -> std::uint16_t {
		if (frozen) {
			throw sol::error("vb.register_keybind: registry already frozen");
		}
		if (name.empty()) {
			throw sol::error("vb.register_keybind: 'name' is required");
		}
		for (std::size_t i = 0; i < keybind_names.size(); ++i) {
			if (keybind_names[i] == name) {
				return static_cast<std::uint16_t>(i);
			}
		}
		if (keybind_names.size() >= protocol::S2CKeybindRegistry::kMaxKeybinds) {
			throw sol::error("vb.register_keybind: at most " +
					std::to_string(protocol::S2CKeybindRegistry::kMaxKeybinds) +
					" keybinds may be registered");
		}
		keybind_names.push_back(name);
		return static_cast<std::uint16_t>(keybind_names.size() - 1);
	};

	// Phase 6.7: overrides the engine's physics::MoveParams tunables
	// (gravity, walk/sprint speed, jump, step height, fly speed, ...).
	// Global, not per-entity-kind -- no entity kind besides the player
	// runs step_movement today, so a per-kind table would have nowhere to
	// apply beyond the one kind that exists. Only whichever fields the
	// table actually sets are used (see effective_move_params()); calling
	// it more than once replaces the whole table, it doesn't merge with an
	// earlier call.
	sol::table physics_tbl = lua.create_table();
	vb["physics"] = physics_tbl;
	physics_tbl["set_params"] = [this](sol::table def) {
		if (frozen) {
			throw sol::error("vb.physics.set_params: registry already frozen");
		}
		move_params_table = def;
	};
	// Phase 6.21: read-back of the *effective* (post pack-override) values,
	// so Lua-side code (e.g. content/base/mechanics.lua's placing raycast)
	// never has to hardcode/guess an engine default that might not match
	// what this same pack already overrode via set_params above. Applies
	// move_params_table over the engine's own default-constructed
	// physics::MoveParams, same base every real call site
	// (src/server/main.cpp, src/client/main.cpp) starts from.
	physics_tbl["get_params"] = [this]() -> sol::table {
		const physics::MoveParams p = [this] {
			physics::MoveParams out{};
			if (move_params_table) {
				const sol::table &def = *move_params_table;
				out.half_width = def.get_or("half_width", out.half_width);
				out.height = def.get_or("height", out.height);
				out.eye_height = def.get_or("eye_height", out.eye_height);
				out.walk_speed = def.get_or("walk_speed", out.walk_speed);
				out.sprint_speed = def.get_or("sprint_speed", out.sprint_speed);
				out.accel = def.get_or("accel", out.accel);
				out.air_accel = def.get_or("air_accel", out.air_accel);
				out.friction = def.get_or("friction", out.friction);
				out.gravity = def.get_or("gravity", out.gravity);
				out.jump_speed = def.get_or("jump_speed", out.jump_speed);
				out.terminal_velocity =
						def.get_or("terminal_velocity", out.terminal_velocity);
				out.step_height = def.get_or("step_height", out.step_height);
				out.fly_speed = def.get_or("fly_speed", out.fly_speed);
				out.fly = def.get_or("fly", out.fly);
			}
			return out;
		}();
		sol::table t = lua_state().create_table();
		t["half_width"] = p.half_width;
		t["height"] = p.height;
		t["eye_height"] = p.eye_height;
		t["walk_speed"] = p.walk_speed;
		t["sprint_speed"] = p.sprint_speed;
		t["accel"] = p.accel;
		t["air_accel"] = p.air_accel;
		t["friction"] = p.friction;
		t["gravity"] = p.gravity;
		t["fall_gravity_scale"] = physics::kFallGravityScale;
		t["jump_speed"] = p.jump_speed;
		t["terminal_velocity"] = p.terminal_velocity;
		t["step_height"] = p.step_height;
		t["fly_speed"] = p.fly_speed;
		t["fly"] = p.fly;
		return t;
	};

	// Phase 6.18: overrides player:punch()'s default hit_radius/
	// player_damage/heal_after_seconds/heal_interval_seconds
	// (ServerSession::PunchParams) -- same "override individual fields on
	// top of a built-in default" shape as vb.physics.set_params above.
	// Unlike 6.5's BlockDamageSystem (which ships zero heal policy until a
	// pack supplies one), punching's self-heal is a real engine default --
	// only its rate is a pack-facing knob, not whether it exists at all.
	// `punch_cooldown_seconds` (REMAINING_TASKS.md's "no punch-rate cooldown
	// enforced engine-side" gap) is the opposite posture: the engine ships
	// no cap at all (0.0, disabled) until a pack sets one -- there's no
	// obviously-correct default swing rate the way there is for a heal
	// timer, so this stays opt-in.
	// Phase 6.21: `reach` moved out to vb.action.set_params below -- it isn't
	// combat-specific (block-edit reach uses the exact same value), so a pack
	// author hunting for the mining-reach knob shouldn't have to look here.
	sol::table combat_tbl = lua.create_table();
	vb["combat"] = combat_tbl;
	combat_tbl["set_params"] = [this](sol::table def) {
		if (frozen) {
			throw sol::error("vb.combat.set_params: registry already frozen");
		}
		combat_params_table = def;
	};

	// Phase 6.21: the one shared "how far can this player interact" knob --
	// unifies what used to be WorldReplicator's own hardcoded, non-overridable
	// block-edit reach constant and vb.combat.set_params's independent (but
	// pack-overridable) punch reach into a single value both
	// WorldReplicator::in_reach()/apply_block_edit() and ServerSession::punch()
	// read (net::ActionParams, effective_action_params()). Deliberately its
	// own namespace, not folded into vb.combat, so a pack author looking for
	// "the block-mining-reach knob" doesn't have to think to search "combat"
	// for it -- vb.action is left open as the natural home for any other
	// future cross-cutting player-action primitive.
	sol::table action_tbl = lua.create_table();
	vb["action"] = action_tbl;
	action_tbl["set_params"] = [this](sol::table def) {
		if (frozen) {
			throw sol::error("vb.action.set_params: registry already frozen");
		}
		action_params_table = def;
	};
	action_tbl["get_params"] = [this]() -> sol::table {
		net::ActionParams p{};
		if (action_params_table) {
			p.reach = action_params_table->get_or("reach", p.reach);
		}
		sol::table t = lua_state().create_table();
		t["reach"] = p.reach;
		return t;
	};

	// REMAINING_TASKS.md's "hunger has no primitive at all yet" gap: a
	// per-player Hunger stat (see vb/ecs/components.hpp), decayed and
	// (optionally) converted to starvation damage by ServerSession's own
	// update_hunger() -- same "engine ships the mechanism at a value that
	// changes nothing until a pack opts in" posture as
	// vb.combat.set_params's punch_cooldown_seconds. Its own namespace,
	// not folded into vb.combat, matching vb.action's own reasoning: a
	// pack author looking for "the hunger knob" shouldn't have to search
	// combat for it.
	sol::table hunger_tbl = lua.create_table();
	vb["hunger"] = hunger_tbl;
	hunger_tbl["set_params"] = [this](sol::table def) {
		if (frozen) {
			throw sol::error("vb.hunger.set_params: registry already frozen");
		}
		hunger_params_table = def;
	};
	hunger_tbl["get_params"] = [this]() -> sol::table {
		net::ServerSession::HungerParams p{};
		if (hunger_params_table) {
			p.decay_per_second =
					hunger_params_table->get_or("decay_per_second", p.decay_per_second);
			p.starvation_damage_per_second = hunger_params_table->get_or(
					"starvation_damage_per_second", p.starvation_damage_per_second);
		}
		sol::table t = lua_state().create_table();
		t["decay_per_second"] = p.decay_per_second;
		t["starvation_damage_per_second"] = p.starvation_damage_per_second;
		return t;
	};

	// Phase 6.8: overrides the engine's default 4-keyframe day/night sky
	// gradient (vb::world::default_day_night_curve()). `keyframes` is a plain
	// array of {tick, brightness, color = {r, g, b}} tables, parsed lazily in
	// effective_day_night_curve() -- calling this more than once replaces the
	// whole curve, it doesn't merge with an earlier call (same posture as
	// vb.physics.set_params above).
	sol::table daynight_tbl = lua.create_table();
	vb["daynight"] = daynight_tbl;
	daynight_tbl["set_curve"] = [this](sol::table def) {
		if (frozen) {
			throw sol::error("vb.daynight.set_curve: registry already frozen");
		}
		sol::optional<sol::table> keyframes = def["keyframes"];
		if (!keyframes || keyframes->size() == 0) {
			throw sol::error(
					"vb.daynight.set_curve: 'keyframes' must be a non-empty array");
		}
		day_night_curve_table = def;
	};
	// Phase 6.8: overrides the real seconds one in-game day/night cycle takes
	// (ServerConfig::day_length_seconds is the base this stacks on top of).
	daynight_tbl["set_day_length"] = [this](double seconds) {
		if (frozen) {
			throw sol::error("vb.daynight.set_day_length: registry already frozen");
		}
		if (!(seconds > 0.0)) {
			throw sol::error("vb.daynight.set_day_length: 'seconds' must be > 0");
		}
		day_length_seconds_override = seconds;
	};

	// Phase 7.2: overrides the engine's default distance-fog fade (which
	// otherwise matches each client's own view_distance -- the server
	// doesn't know that value, so there's no default to advertise unless a
	// pack sets one). Deliberately no 'color' field for *above-water* fog:
	// it always reads as "distance to the current sky color"
	// (vb::world::sky_color_for_time()), never an independently drifting
	// tint (decided 2026-09-19, REMAINING_TASKS.md Phase 7.2).
	// Phase 7.5 (decided 2026-09-23): underwater fog is the one exception --
	// an optional 'underwater_tint = {r=, g=, b=}' table (each 0-255)
	// overrides the client's own texture-average default for the currently
	// submerged liquid block.
	sol::table render_tbl = lua.create_table();
	vb["render"] = render_tbl;
	render_tbl["set_fog"] = [this](sol::table def) {
		if (frozen) {
			throw sol::error("vb.render.set_fog: registry already frozen");
		}
		const sol::optional<double> start = def["start"];
		const sol::optional<double> end = def["end"];
		if (!start || !end) {
			throw sol::error("vb.render.set_fog: 'start' and 'end' are required");
		}
		if (!(*end > *start)) {
			throw sol::error("vb.render.set_fog: 'end' must be greater than 'start'");
		}
		const sol::optional<sol::table> tint = def["underwater_tint"];
		if (tint) {
			const sol::optional<double> r = (*tint)["r"];
			const sol::optional<double> g = (*tint)["g"];
			const sol::optional<double> b = (*tint)["b"];
			if (!r || !g || !b) {
				throw sol::error(
						"vb.render.set_fog: 'underwater_tint' needs 'r', 'g', and 'b'");
			}
			const auto in_range = [](double v) { return v >= 0.0 && v <= 255.0; };
			if (!in_range(*r) || !in_range(*g) || !in_range(*b)) {
				throw sol::error(
						"vb.render.set_fog: 'underwater_tint' channels must be 0-255");
			}
		}
		fog_params_table = def;
	};

	// Phase 6.14: vb.noise.* -- small builder functions that just stamp a
	// `type` field onto a plain Lua table and return it. The real work (a
	// recursive parse into the pure-C++ worldgen::NoiseNode IR) happens once,
	// lazily, in build_worldgen_pipeline() -- not here -- so it's fine that
	// these run at pack-load time and return ordinary tables rather than
	// opaque handles.
	sol::table noise_tbl = lua.create_table();
	vb["noise"] = noise_tbl;
	noise_tbl["constant"] = [this](double value) {
		sol::table t = lua_state().create_table();
		t["type"] = "constant";
		t["value"] = value;
		return t;
	};
	noise_tbl["value"] = [this](sol::optional<double> frequency) {
		sol::table t = lua_state().create_table();
		t["type"] = "value";
		t["frequency"] = frequency.value_or(1.0);
		return t;
	};
	noise_tbl["cellular"] = [this](sol::optional<double> frequency) {
		sol::table t = lua_state().create_table();
		t["type"] = "cellular";
		t["frequency"] = frequency.value_or(1.0);
		return t;
	};
	noise_tbl["fbm"] = [this](sol::table def) {
		sol::table t = lua_state().create_table();
		t["type"] = "fbm";
		t["source"] = def.get_or("source", sol::object(sol::lua_nil));
		t["frequency"] = def.get_or("frequency", 1.0);
		t["octaves"] = def.get_or("octaves", 4);
		t["lacunarity"] = def.get_or("lacunarity", 2.0);
		t["gain"] = def.get_or("gain", 0.5);
		return t;
	};
	noise_tbl["remap"] = [this](sol::table def) {
		sol::table t = lua_state().create_table();
		t["type"] = "remap";
		t["source"] = def.get_or("source", sol::object(sol::lua_nil));
		t["in_min"] = def.get_or("in_min", 0.0);
		t["in_max"] = def.get_or("in_max", 1.0);
		t["out_min"] = def.get_or("out_min", 0.0);
		t["out_max"] = def.get_or("out_max", 1.0);
		return t;
	};
	noise_tbl["combine"] = [this](sol::table def) {
		sol::table t = lua_state().create_table();
		t["type"] = "combine";
		t["a"] = def.get_or("a", sol::object(sol::lua_nil));
		t["b"] = def.get_or("b", sol::object(sol::lua_nil));
		t["op"] = def.get_or("op", std::string("add"));
		return t;
	};

	// Phase 6.14: vb.worldgen.set_pipeline{...} -- pack-load-time only
	// (frozen guard, same as every other registration function). 'height' is
	// validated eagerly (a vb.noise.* node is required) since opting in
	// without one is almost certainly a bug, not an intentional no-op --
	// same "validate at the call site" posture 6.8's set_curve uses for its
	// own required 'keyframes' field.
	sol::table worldgen_tbl = lua.create_table();
	vb["worldgen"] = worldgen_tbl;
	worldgen_tbl["set_pipeline"] = [this](sol::table def) {
		if (frozen) {
			throw sol::error("vb.worldgen.set_pipeline: registry already frozen");
		}
		const sol::optional<sol::table> height = def["height"];
		if (!height) {
			throw sol::error(
					"vb.worldgen.set_pipeline: 'height' (a vb.noise.* node) is required");
		}
		worldgen_pipeline_table = def;
	};

	// Phase 6.13: read-only visibility into the operator's server.toml/CLI
	// settings -- deliberately not an override surface like 6.6-6.11's
	// set_params/set_curve tables above (a pack should not be able to
	// silently change max_players out from under the operator running the
	// server). Returns nil for every key when set_server_config() was never
	// called (e.g. --singleplayer's in-process PackRuntime) or for an
	// unrecognised key.
	sol::table config_tbl = lua.create_table();
	vb["config"] = config_tbl;
	config_tbl["get"] = [this](const std::string &key) -> sol::object {
		if (!server_config) {
			return sol::lua_nil;
		}
		const core::ServerConfig &c = *server_config;
		if (key == "bind_address") {
			return sol::make_object(lua_state(), c.bind_address);
		}
		if (key == "port") {
			return sol::make_object(lua_state(), c.port);
		}
		if (key == "content_pack") {
			return sol::make_object(lua_state(), c.content_pack);
		}
		if (key == "max_players") {
			return sol::make_object(lua_state(), c.max_players);
		}
		if (key == "view_distance") {
			return sol::make_object(lua_state(), c.view_distance);
		}
		if (key == "tick_rate") {
			return sol::make_object(lua_state(), c.tick_rate);
		}
		if (key == "world_seed") {
			return sol::make_object(lua_state(), c.world_seed);
		}
		if (key == "gravity") {
			return sol::make_object(lua_state(), c.gravity);
		}
		if (key == "void_kill_y") {
			return sol::make_object(lua_state(), c.void_kill_y);
		}
		if (key == "day_length_seconds") {
			return sol::make_object(lua_state(), c.day_length_seconds);
		}
		if (key == "asset_max_file_mb") {
			return sol::make_object(lua_state(), c.asset_max_file_mb);
		}
		if (key == "asset_max_total_mb") {
			return sol::make_object(lua_state(), c.asset_max_total_mb);
		}
		if (key == "max_connections_per_ip") {
			return sol::make_object(lua_state(), c.max_connections_per_ip);
		}
		if (key == "max_messages_per_second") {
			return sol::make_object(lua_state(), c.max_messages_per_second);
		}
		if (key == "auth_mode") {
			return sol::make_object(lua_state(),
					c.auth_mode == core::ConfigAuthMode::kToken ? std::string("token")
																: std::string("none"));
		}
		if (key == "motd") {
			return sol::make_object(lua_state(), c.motd);
		}
		return sol::lua_nil;
	};

	sol::table world_tbl = lua.create_table();
	vb["world"] = world_tbl;

	world_tbl["get_block"] = [this](int x, int y, int z) -> std::uint16_t {
		if (replicator == nullptr) {
			throw sol::error("vb.world.get_block: world not attached yet");
		}
		return static_cast<std::uint16_t>(
				replicator->world().get_block({ x, y, z }));
	};

	// Known limitation: doesn't run apply_block_edit's relight cascade, so a
	// scripted edit can desync lighting until something else touches the
	// chunk. No consumer exists this phase.
	world_tbl["set_block"] = [this](int x, int y, int z, std::uint16_t id) {
		if (replicator == nullptr) {
			throw sol::error("vb.world.set_block: world not attached yet");
		}
		const auto bid = static_cast<core::BlockId>(id);
		if (!registry.contains(bid)) {
			throw sol::error("vb.world.set_block: unknown block id");
		}
		replicator->world().set_block({ x, y, z }, bid);
	};

	world_tbl["raycast"] = [this](sol::table origin, sol::table dir,
								   double max_dist,
								   sol::this_state ts) -> sol::object {
		sol::state_view sv(ts);
		if (replicator == nullptr) {
			throw sol::error("vb.world.raycast: world not attached yet");
		}
		const core::Vec3d o{ origin.get_or("x", 0.0), origin.get_or("y", 0.0),
			origin.get_or("z", 0.0) };
		const core::Vec3d d{ dir.get_or("x", 0.0), dir.get_or("y", 0.0),
			dir.get_or("z", 0.0) };
		const world::VoxelRayHit hit =
				world::raycast_voxel(replicator->world(), o, d, max_dist);
		if (!hit.hit) {
			return sol::make_object(sv, sol::lua_nil);
		}
		sol::table t = sv.create_table();
		t["hit"] = true;
		t["x"] = hit.voxel.x;
		t["y"] = hit.voxel.y;
		t["z"] = hit.voxel.z;
		t["nx"] = hit.normal.x;
		t["ny"] = hit.normal.y;
		t["nz"] = hit.normal.z;
		return t;
	};

	// Phase 5.1 dropped-item entity: a real, working world spawn ahead of the
	// generic EnTT-backed `vb.world.spawn` above (still a no-op -- waits on
	// Phase 3.1). Deliberately its own binding, not routed through `spawn`,
	// since it isn't a `vb.register_entity` kind at all -- just a hardcoded
	// ItemDropSystem entry (vb::world::ItemDropSystem, src/net/session.cpp).
	world_tbl["spawn_item_drop"] = [this](sol::table pos, std::uint16_t item,
										   std::uint16_t count) {
		if (session == nullptr) {
			throw sol::error("vb.world.spawn_item_drop: session not attached yet");
		}
		const core::Vec3d p{ pos.get_or("x", 0.0), pos.get_or("y", 0.0),
			pos.get_or("z", 0.0) };
		session->spawn_item_drop(p, static_cast<core::BlockId>(item), count);
	};

	// Phase 6.1: shared instance-method table for every spawned `self`, set
	// as the metatable of each so `self:get_pos()`/`self:damage(...)`/... work
	// while arbitrary fields (`self.hp = 10`) stay free on the table itself.
	entity_methods = lua.create_table();
	entity_methods["get_pos"] = [this](sol::table self, sol::this_state ts) -> sol::object {
		sol::state_view sv(ts);
		auto it = entities.find(self_net_id(self));
		if (it == entities.end()) {
			throw sol::error("entity:get_pos(): entity is gone");
		}
		// An attached entity is moved by ServerSession every tick.
		if (session != nullptr) {
			if (const auto p = session->script_entity_pos(it->first)) {
				it->second.pos = *p;
			}
		}
		sol::table t = sv.create_table();
		t["x"] = it->second.pos.x;
		t["y"] = it->second.pos.y;
		t["z"] = it->second.pos.z;
		return t;
	};
	entity_methods["set_pos"] = [this](sol::table self, double x, double y, double z) {
		const core::NetId id = self_net_id(self);
		auto it = entities.find(id);
		if (it == entities.end()) {
			throw sol::error("entity:set_pos(): entity is gone");
		}
		it->second.pos = { x, y, z };
		if (session != nullptr) {
			session->set_script_entity_state(id, it->second.pos);
		}
	};
	entity_methods["get_kind"] = [this](sol::table self) -> std::string {
		auto it = entities.find(self_net_id(self));
		if (it == entities.end()) {
			return {};
		}
		return entity_kinds[it->second.kind_index].name;
	};
	// Always fires the kind's on_hit, exactly as before -- a pack that never
	// opted into `vb.register_entity{health=...}` sees no behavior change at
	// all (notification-only, no despawn). If the kind did opt in,
	// dispatch_entity_hit also decrements the tracked health and auto-despawns
	// at 0 (see its own comment).
	entity_methods["damage"] = [this](sol::table self, double amount,
									   sol::optional<std::string> cause) {
		dispatch_entity_hit(self_net_id(self), amount, cause.value_or(std::string{}));
	};
	entity_methods["remove"] = [this](sol::table self, sol::optional<std::string> cause) {
		despawn_entity(self_net_id(self), cause.value_or(std::string{}));
	};
	// nil if this instance's kind never set `health=` in vb.register_entity --
	// lets a pack tell "not tracked" apart from "tracked and full/empty".
	entity_methods["get_health"] = [this](sol::table self, sol::this_state ts) -> sol::object {
		sol::state_view sv(ts);
		const auto it = entities.find(self_net_id(self));
		if (it == entities.end() || !it->second.health) {
			return sol::make_object(sv, sol::lua_nil);
		}
		const EntityKindDef &kind = entity_kinds[it->second.kind_index];
		sol::table t = sv.create_table();
		t["current"] = *it->second.health;
		t["max"] = *kind.max_health;
		return t;
	};
	// Errors if the kind never opted into health tracking -- there's no
	// sensible "max" to clamp against otherwise. Clamped to [0, max]; reaching
	// 0 despawns exactly like damage() running health out does, so a pack
	// script can implement healing/instakill without duplicating the despawn
	// call itself.
	entity_methods["set_health"] = [this](sol::table self, double value) {
		const core::NetId id = self_net_id(self);
		const auto it = entities.find(id);
		if (it == entities.end() || !it->second.health) {
			throw sol::error(
					"entity:set_health(): kind never set health= in vb.register_entity");
		}
		const EntityKindDef &kind = entity_kinds[it->second.kind_index];
		it->second.health = std::clamp(
				static_cast<float>(value), 0.0f, *kind.max_health);
		if (*it->second.health <= 0.0f) {
			despawn_entity(id, "set_health");
		}
	};
	// World-space label: a string changes only the value (keeping the current
	// style, or the kind's if there is no label yet); a table re-applies the
	// kind's style with its own fields on top (an omitted `value` keeps the
	// current text); nil or "" removes the label. Replicated as a small
	// S2C_EntityProps delta -- the entity keeps its net id.
	entity_methods["set_text"] = [this](sol::table self, sol::object arg) {
		const core::NetId id = self_net_id(self);
		const auto it = entities.find(id);
		if (it == entities.end()) {
			throw sol::error("entity:set_text(): entity is gone");
		}
		const EntityKindDef &kind = entity_kinds[it->second.kind_index];
		std::optional<protocol::EntityText> text;
		if (arg.get_type() == sol::type::string) {
			text = parse_entity_text("entity:set_text()", arg,
					it->second.text.value_or(kind.text_style));
		} else if (arg.get_type() == sol::type::table) {
			protocol::EntityText base = kind.text_style;
			if (it->second.text) {
				base.value = it->second.text->value;
			}
			text = parse_entity_text("entity:set_text()", arg, std::move(base));
		} else if (arg.get_type() != sol::type::lua_nil) {
			throw sol::error("entity:set_text(): expected a string, a table or nil");
		}
		if (text && text->value.empty()) {
			text.reset();
		}
		it->second.text = text;
		if (session != nullptr) {
			session->set_script_entity_text(id, std::move(text));
		}
	};
	// Forces the animation clip every client plays for this entity (any name
	// its sheet declares, e.g. "open"); nil returns to the automatic choice
	// from velocity/on_ground. Sticky and replicated like set_text.
	entity_methods["set_clip"] = [this](sol::table self, sol::object arg) {
		const core::NetId id = self_net_id(self);
		if (entities.find(id) == entities.end()) {
			throw sol::error("entity:set_clip(): entity is gone");
		}
		std::optional<std::string> clip;
		if (arg.get_type() == sol::type::string) {
			clip = arg.as<std::string>();
			if (clip->empty() || clip->size() > protocol::kMaxEntityClipNameBytes) {
				throw sol::error("entity:set_clip(): clip name must be 1-" +
						std::to_string(protocol::kMaxEntityClipNameBytes) + " bytes");
			}
		} else if (arg.get_type() != sol::type::lua_nil) {
			throw sol::error("entity:set_clip(): expected a clip name or nil");
		}
		if (session != nullptr) {
			session->set_script_entity_clip(id, std::move(clip));
		}
	};
	// Glues this entity to `parent` (another entity, or a player): it then
	// follows the parent every tick on the server and every frame on clients
	// (no trailing). Despawned with cause "parent_removed" when the parent
	// goes away. opts: offset = {x, y, z} (default 0), face_offset = bool
	// (turn the offset with the parent's facing), layer = -8..8 (overrides
	// the visual's layer while attached).
	entity_methods["attach_to"] = [this](sol::table self, sol::object parent,
										  sol::optional<sol::table> opts) {
		const core::NetId id = self_net_id(self);
		const auto it = entities.find(id);
		if (it == entities.end()) {
			throw sol::error("entity:attach_to(): entity is gone");
		}
		protocol::EntityAttachment a;
		if (parent.is<PlayerHandle>()) {
			a.parent = parent.as<PlayerHandle &>().net_id;
			if (session == nullptr || !session->player_move_state(a.parent)) {
				throw sol::error("entity:attach_to(): that player is not in the game");
			}
		} else if (parent.get_type() == sol::type::table) {
			a.parent = self_net_id(parent.as<sol::table>());
			if (entities.find(a.parent) == entities.end()) {
				throw sol::error("entity:attach_to(): parent entity is gone");
			}
			if (a.parent == id) {
				throw sol::error("entity:attach_to(): an entity can't attach to itself");
			}
			// Walk up the parent's chain: attaching to a descendant is a cycle.
			core::NetId cur = a.parent;
			for (int depth = 0; depth < 64; ++depth) {
				const auto p = entities.find(cur);
				if (p == entities.end() || !p->second.attach) {
					break;
				}
				cur = p->second.attach->parent;
				if (cur == id) {
					throw sol::error("entity:attach_to(): that would make an attachment cycle");
				}
			}
		} else {
			throw sol::error("entity:attach_to(): parent must be an entity or a player");
		}
		if (opts) {
			if (const sol::object off = (*opts)["offset"]; off.get_type() != sol::type::lua_nil) {
				if (off.get_type() != sol::type::table) {
					throw sol::error("entity:attach_to(): offset must be {x=, y=, z=}");
				}
				const sol::table t = off.as<sol::table>();
				const auto axis = [&](const char *k) {
					const sol::object v = t[k];
					if (v.get_type() == sol::type::lua_nil) {
						return 0.0f;
					}
					if (v.get_type() != sol::type::number || !std::isfinite(v.as<double>()) ||
							std::abs(v.as<double>()) > 64.0) {
						throw sol::error(std::string("entity:attach_to(): offset.") + k +
								" must be a number in [-64, 64]");
					}
					return static_cast<float>(v.as<double>());
				};
				a.offset = core::Vec3f{ axis("x"), axis("y"), axis("z") };
			}
			if (const sol::object v = (*opts)["face_offset"]; v.get_type() != sol::type::lua_nil) {
				a.face_offset = parse_bool("entity:attach_to(): face_offset", v);
			}
			if (const sol::object v = (*opts)["layer"]; v.get_type() != sol::type::lua_nil) {
				a.layer = parse_layer("entity:attach_to(): layer", v);
			}
		}
		it->second.attach = a;
		if (session != nullptr) {
			session->set_script_entity_attachment(id, a);
		}
	};
	// Undoes attach_to; the entity stays where it is. No-op if not attached.
	entity_methods["detach"] = [this](sol::table self) {
		const core::NetId id = self_net_id(self);
		const auto it = entities.find(id);
		if (it == entities.end()) {
			throw sol::error("entity:detach(): entity is gone");
		}
		it->second.attach.reset();
		if (session != nullptr) {
			session->set_script_entity_attachment(id, std::nullopt);
		}
	};
	// The label's current value, or nil if the entity has none.
	entity_methods["get_text"] = [this](sol::table self, sol::this_state ts) -> sol::object {
		sol::state_view sv(ts);
		const auto it = entities.find(self_net_id(self));
		if (it == entities.end() || !it->second.text) {
			return sol::make_object(sv, sol::lua_nil);
		}
		return sol::make_object(sv, it->second.text->value);
	};
	entity_mt = lua.create_table();
	entity_mt["__index"] = entity_methods;

	world_tbl["spawn"] = [this](const std::string &kind, sol::table pos,
								 sol::optional<sol::table> opts,
								 sol::this_state ts) -> sol::object {
		sol::state_view sv(ts);
		auto kind_it = std::find_if(entity_kinds.begin(), entity_kinds.end(),
				[&](const EntityKindDef &e) { return e.name == kind; });
		if (kind_it == entity_kinds.end()) {
			throw sol::error("vb.world.spawn: unknown entity kind '" + kind + "'");
		}
		if (session == nullptr) {
			throw sol::error("vb.world.spawn: session not attached yet");
		}
		// Entity-management follow-up (spec architecture_spec/rendering.md
		// §11.3's "Per-instance override"): validated up front, before
		// spawn_script_entity() below, so a malformed override never leaves a
		// half-spawned entity behind.
		const sol::optional<sol::table> visual_override_table = [&]() -> sol::optional<sol::table> {
			if (!opts) {
				return sol::nullopt;
			}
			return (*opts)["visual_override"];
		}();
		std::optional<protocol::EntityVisualOverride> visual_override;
		if (visual_override_table) {
			visual_override = parse_entity_visual_override(*visual_override_table);
		}
		// Same "validate before spawning" rule for the label: the kind's style
		// is the base, a string sets just the value, a table overrides fields.
		std::optional<protocol::EntityText> text;
		if (!kind_it->text_style.value.empty()) {
			text = kind_it->text_style;
		}
		if (opts) {
			if (const sol::object text_obj = (*opts)["text"];
					text_obj.get_type() != sol::type::lua_nil) {
				text = parse_entity_text("vb.world.spawn", text_obj, kind_it->text_style);
			}
		}
		if (text && text->value.empty()) {
			text.reset();
		}
		const core::Vec3d p{ pos.get_or("x", 0.0), pos.get_or("y", 0.0),
			pos.get_or("z", 0.0) };
		const core::NetId id = session->spawn_script_entity(kind_it->id, p);
		if (visual_override) {
			session->set_script_entity_visual_override(id, visual_override);
		}
		if (text) {
			session->set_script_entity_text(id, text);
		}
		sol::table self = sv.create_table();
		self[sol::metatable_key] = entity_mt;
		self["__net_id"] = static_cast<double>(static_cast<std::uint32_t>(id));
		if (visual_override_table) {
			// Spec's reserved key, kept in sync purely for pack introspection
			// (e.g. a pack reading back what it passed in) -- ServerSession
			// already has the parsed, replicated copy above; nothing reads
			// this table back engine-side.
			self["visual_override"] = *visual_override_table;
		}
		const std::size_t kind_index =
				static_cast<std::size_t>(kind_it - entity_kinds.begin());
		ScriptEntity entity{ .kind_index = kind_index, .self = self, .pos = p, .health = std::nullopt, .text = text };
		if (kind_it->max_health) {
			entity.health = *kind_it->max_health;
		}
		entities[id] = std::move(entity);
		if (kind_it->on_spawn.valid()) {
			vm.begin_call_budget();
			sol::protected_function_result r = kind_it->on_spawn(self);
			if (!r.valid()) {
				const sol::error e = r;
				VB_WARN("script", "entity on_spawn handler error: ", e.what());
			}
		}
		return self;
	};

	static const std::set<std::string> kValidEvents = { "player_join",
		"player_leave", "block_break", "block_place", "player_interact",
		"chat", "tick", "ui_event", "player_death", "player_input",
		"block_break_begin", "block_break_tick", "block_health_tick",
		"region_enter", "region_exit", "player_landed", "login_changed" };
	vb["on"] = [this](const std::string &event, sol::protected_function fn) {
		if (kValidEvents.find(event) == kValidEvents.end()) {
			throw sol::error("vb.on: unknown event '" + event + "'");
		}
		handlers[event].push_back(std::move(fn));
	};

	vb["after"] = [this](double seconds, sol::protected_function fn) {
		timers.push_back({ seconds, seconds, false, std::move(fn), false });
	};
	vb["every"] = [this](double seconds, sol::protected_function fn) {
		timers.push_back({ seconds, seconds, true, std::move(fn), false });
	};

	sol::table storage_proxy = lua.create_table();
	sol::table storage_meta = lua.create_table();
	storage_meta[sol::meta_function::index] =
			[this](sol::table, const std::string &key,
					sol::this_state ts) -> sol::object {
		sol::state_view sv(ts);
		if (!storage.contains(key)) {
			return sol::make_object(sv, sol::lua_nil);
		}
		return json_to_lua(sv, storage.at(key));
	};
	storage_meta[sol::meta_function::new_index] =
			[this](sol::table, const std::string &key, sol::object value) {
				storage[key] = lua_to_json(value);
				storage_dirty_flag = true;
			};
	storage_proxy[sol::metatable_key] = storage_meta;
	vb["storage"] = storage_proxy;

	// Phase 6.4: vb.db -- a generic per-key store, distinct from the
	// pack-global vb.storage above. `key` is whatever the script chooses
	// ("user:" .. name, "session:" .. token, ...); values round-trip through
	// the same json_to_lua/lua_to_json used by vb.storage so tables/numbers/
	// strings/booleans all persist correctly, not just strings.
	sol::table db_tbl = lua.create_table();
	db_tbl["get"] = [this](const std::string &key,
							sol::this_state ts) -> sol::object {
		sol::state_view sv(ts);
		const std::optional<std::string> raw = db.get(key);
		if (!raw) {
			return sol::make_object(sv, sol::lua_nil);
		}
		const nlohmann::json parsed =
				nlohmann::json::parse(*raw, nullptr, false);
		if (parsed.is_discarded()) {
			return sol::make_object(sv, sol::lua_nil);
		}
		return json_to_lua(sv, parsed);
	};
	db_tbl["set"] = [this](const std::string &key, sol::object value) {
		db.set(key, lua_to_json(value).dump());
	};
	db_tbl["delete"] = [this](const std::string &key) { db.erase(key); };
	vb["db"] = db_tbl;

	// Phase 6.4: vb.crypto.hash -- a minimal primitive so a pack implementing
	// its own login (built on vb.db above) doesn't have to roll credential
	// hashing in pure Lua; the sandbox strips os/io deliberately (§10.2). The
	// engine itself still takes no position on auth as a concept.
	sol::table crypto_tbl = lua.create_table();
	crypto_tbl["hash"] = [](const std::string &data) -> std::string {
		return core::sha256_hex(data);
	};
	vb["crypto"] = crypto_tbl;
}

core::NetId PackRuntime::Impl::self_net_id(const sol::table &self) const {
	return static_cast<core::NetId>(
			static_cast<std::uint32_t>(self.get_or("__net_id", 0.0)));
}

// Phase 6.1's ScriptPreTickSystem-equivalent: fires on_tick(self, dt) for
// every currently-spawned entity, once per PackRuntime::dispatch_tick. Ids are
// snapshotted first since a handler may spawn/remove entities (including
// itself) mid-iteration.
void PackRuntime::Impl::dispatch_entity_tick(double dt) {
	std::vector<core::NetId> ids;
	ids.reserve(entities.size());
	for (const auto &[id, e] : entities) {
		(void)e;
		ids.push_back(id);
	}
	for (const core::NetId id : ids) {
		const auto it = entities.find(id);
		if (it == entities.end()) {
			continue; // removed by an earlier handler this tick
		}
		const EntityKindDef &kind = entity_kinds[it->second.kind_index];
		if (!kind.on_tick.valid()) {
			continue;
		}
		vm.begin_call_budget();
		sol::protected_function_result r = kind.on_tick(it->second.self, dt);
		if (!r.valid()) {
			const sol::error e = r;
			VB_WARN("script", "entity on_tick handler error: ", e.what());
		}
	}
}

void PackRuntime::Impl::dispatch_entity_hit(
		core::NetId id, double amount, std::string_view cause) {
	const auto it = entities.find(id);
	if (it == entities.end()) {
		return;
	}
	const std::size_t kind_index = it->second.kind_index;
	const EntityKindDef &kind = entity_kinds[kind_index];
	if (kind.on_hit.valid()) {
		vm.begin_call_budget();
		sol::protected_function_result r =
				kind.on_hit(it->second.self, amount, std::string(cause));
		if (!r.valid()) {
			const sol::error e = r;
			VB_WARN("script", "entity on_hit handler error: ", e.what());
		}
	}
	// on_hit is free to despawn (or respawn under a fresh id) the entity
	// itself, e.g. kitchen_sink's sentry.lua calling self:remove() once its
	// own hand-tracked hp runs out -- re-look-up rather than reuse `it`,
	// which the erase above would leave dangling.
	const auto it2 = entities.find(id);
	if (it2 == entities.end() || !it2->second.health) {
		// Health tracking is opt-in (vb.register_entity{health=...}) -- a kind
		// that never set it keeps the pre-existing notification-only behavior
		// (on_hit fires, nothing else) exactly as before this was added.
		return;
	}
	it2->second.health =
			std::max(0.0f, *it2->second.health - static_cast<float>(amount));
	if (*it2->second.health <= 0.0f) {
		despawn_entity(id, cause);
	}
}

// Phase 6.1's ScriptPostTickSystem-equivalent (the "entity despawn commit"
// half): fires on_death(self, cause), then removes the entity from both this
// map and the interest grid, so it stops replicating.
void PackRuntime::Impl::despawn_entity(core::NetId id, std::string_view cause) {
	const auto it = entities.find(id);
	if (it == entities.end()) {
		return;
	}
	const EntityKindDef &kind = entity_kinds[it->second.kind_index];
	if (kind.on_death.valid()) {
		vm.begin_call_budget();
		sol::protected_function_result r =
				kind.on_death(it->second.self, std::string(cause));
		if (!r.valid()) {
			const sol::error e = r;
			VB_WARN("script", "entity on_death handler error: ", e.what());
		}
	}
	if (session != nullptr) {
		session->remove_script_entity(id);
	}
	entities.erase(it);
}

void PackRuntime::Impl::dispatch_tick(double dt) {
	fire("tick", dt);
	dispatch_entity_tick(dt);

	for (auto &t : timers) {
		if (t.cancelled) {
			continue;
		}
		t.remaining -= dt;
		int fires = 0;
		while (t.remaining <= 0.0) {
			vm.begin_call_budget();
			sol::protected_function_result r = t.fn();
			if (!r.valid()) {
				const sol::error e = r;
				VB_WARN("script", "timer handler error: ", e.what());
			}
			++fires;
			if (!t.repeating) {
				t.cancelled = true;
				break;
			}
			t.remaining += t.period;
			if (fires >= kMaxTimerCatchUpFires) {
				break;
			}
		}
	}
	timers.erase(std::remove_if(timers.begin(), timers.end(),
						 [](const Timer &t) { return t.cancelled; }),
			timers.end());

	if (storage_dirty_flag) {
		flush_storage();
	}
}

void PackRuntime::Impl::flush_storage() {
	std::ofstream out(storage_path, std::ios::trunc);
	if (!out) {
		VB_WARN("script", "vb.storage: failed to open '", storage_path.string(),
				"' for writing");
		return;
	}
	out << storage.dump();
	storage_dirty_flag = false;
	++storage_revision_counter;
}

bool PackRuntime::Impl::on_block_edit_before(core::NetId editor,
		core::IVec3 pos, core::BlockId existing, core::BlockId new_block,
		bool is_break) {
	(void)existing;
	(void)new_block;
	sol::table pos_tbl = lua_state().create_table();
	pos_tbl["x"] = pos.x;
	pos_tbl["y"] = pos.y;
	pos_tbl["z"] = pos.z;
	PlayerHandle p{ editor, this };
	return run_veto(is_break ? "block_break" : "block_place", p, pos_tbl);
}

void PackRuntime::Impl::on_block_edit_after(core::NetId editor,
		core::IVec3 pos, core::BlockId removed, core::BlockId placed,
		bool is_break) {
	const core::BlockId affected = is_break ? removed : placed;
	for (auto &bd : blocks) {
		if (bd.id != affected) {
			continue;
		}
		sol::protected_function &cb = is_break ? bd.on_break : bd.on_place;
		if (!cb.valid()) {
			break;
		}
		sol::table pos_tbl = lua_state().create_table();
		pos_tbl["x"] = pos.x;
		pos_tbl["y"] = pos.y;
		pos_tbl["z"] = pos.z;
		sol::table ctx = lua_state().create_table();
		ctx["pos"] = pos_tbl;
		ctx["player"] = PlayerHandle{ editor, this };
		vm.begin_call_budget();
		sol::protected_function_result r = cb(ctx);
		if (!r.valid()) {
			const sol::error e = r;
			VB_WARN("script", "block on_break/on_place handler error: ", e.what());
		} else {
			const sol::object ret = r;
			if (ret.valid() && ret.get_type() != sol::type::lua_nil) {
				VB_DEBUG("script", "block callback returned a value (drop) "
								   "-- not materialized yet, Phase 5.1 items");
			}
		}
		break;
	}
}

void PackRuntime::Impl::sync_inventory(core::NetId id) {
	if (session == nullptr) {
		return;
	}
	const net::ConnId conn = session->conn_for_player(id);
	if (conn == net::ConnId::kInvalid) {
		return;
	}
	protocol::S2CInventory msg;
	for (const auto &stack : inventories[id].slots) {
		msg.slots.push_back({ stack.item, stack.count });
	}
	net::send_message(transport, conn, msg);
}

void PackRuntime::Impl::give_item(
		core::NetId id, core::BlockId item, std::uint16_t count) {
	if (count == 0) {
		return;
	}
	const std::uint16_t max_stack = registry.contains(item)
			? registry.get(item).max_stack
			: world::kDefaultMaxStackSize;
	std::vector<ecs::ItemStack> &slots = inventories[id].slots;
	std::uint32_t remaining = count;
	for (auto &s : slots) {
		if (remaining == 0) {
			break;
		}
		if (s.item != item || s.count >= max_stack) {
			continue;
		}
		const auto added = static_cast<std::uint16_t>(
				std::min<std::uint32_t>(max_stack - s.count, remaining));
		s.count = static_cast<std::uint16_t>(s.count + added);
		remaining -= added;
	}
	while (remaining > 0) {
		const auto added = static_cast<std::uint16_t>(
				std::min<std::uint32_t>(max_stack, remaining));
		slots.push_back({ item, added });
		remaining -= added;
	}
}

void PackRuntime::Impl::drop_all_items(core::NetId id, core::Vec3d pos) {
	if (session == nullptr) {
		return;
	}
	for (const auto &stack : inventories[id].slots) {
		session->spawn_item_drop(pos, stack.item, stack.count);
	}
	inventories[id].slots.clear();
	sync_inventory(id);
}

net::ServerSession::RespawnDecision PackRuntime::Impl::run_respawn_handler(
		core::NetId id, std::string_view cause, float health_before) {
	net::ServerSession::RespawnDecision decision{ 20.0f, session->spawn_point(id),
		"* you died and respawned" };

	auto it = handlers.find("player_death");
	if (it == handlers.end()) {
		return decision;
	}
	// Captured before the caller (check_respawns) overwrites the player's
	// position with the respawn point -- this is where they actually died,
	// used as the drop location so a dropped inventory doesn't just land at
	// the respawn point and get instantly re-picked-up there (pickup_radius
	// covers it).
	const core::Vec3d death_pos =
			session->player_move_state(id).value_or(physics::MoveState{}).position;
	PlayerHandle p{ id, this };
	for (auto &fn : it->second) {
		if (!fn.valid()) {
			continue;
		}
		vm.begin_call_budget();
		sol::protected_function_result r = fn(p, std::string(cause), health_before);
		if (!r.valid()) {
			const sol::error e = r;
			VB_WARN("script", "vb.on('player_death') handler error: ", e.what());
			continue;
		}
		const sol::object ret = r;
		if (!ret.valid() || ret.get_type() != sol::type::table) {
			continue;
		}
		sol::table t = ret.as<sol::table>();
		decision.heal_to = t.get_or("heal", decision.heal_to);
		if (sol::optional<sol::table> pos_tbl = t.get<sol::optional<sol::table>>("pos")) {
			decision.pos = { pos_tbl->get_or("x", decision.pos.x),
				pos_tbl->get_or("y", decision.pos.y),
				pos_tbl->get_or("z", decision.pos.z) };
		}
		decision.message = t.get_or("message", decision.message);
		if (t.get_or("drop_inventory", false)) {
			drop_all_items(id, death_pos);
		}
		break; // first handler that returns a decision table wins
	}
	return decision;
}

net::ServerSession::InputHookResult PackRuntime::Impl::run_player_input(
		core::NetId id, const protocol::InputCmd &cmd) {
	net::ServerSession::InputHookResult result;
	auto it = handlers.find("player_input");
	if (it == handlers.end()) {
		return result; // no handlers registered: pass through unchanged
	}

	core::Vec3f move = cmd.move;
	float yaw = cmd.yaw;
	float pitch = cmd.pitch;
	std::uint8_t buttons = cmd.buttons;
	std::uint32_t keybinds = cmd.keybinds;
	std::uint8_t selected_slot = cmd.selected_slot;
	bool changed = false;

	PlayerHandle p{ id, this };
	for (auto &fn : it->second) {
		if (!fn.valid()) {
			continue;
		}
		sol::table input_t = build_input_table(lua_state(), move, yaw, pitch,
				buttons, keybinds, keybind_names, static_cast<double>(cmd.dt),
				selected_slot);
		vm.begin_call_budget();
		sol::protected_function_result r = fn(p, input_t);
		if (!r.valid()) {
			const sol::error e = r;
			VB_WARN("script", "vb.on('player_input') handler error: ", e.what());
			continue;
		}
		const sol::object ret = r;
		if (ret.valid() && ret.get_type() == sol::type::boolean && !ret.as<bool>()) {
			result.veto = true;
			return result; // first veto wins, same as run_veto
		}
		if (!ret.valid() || ret.get_type() != sol::type::table) {
			continue; // true/nil/other: pass through unchanged
		}
		sol::table t = ret.as<sol::table>();
		if (sol::optional<sol::table> move_tbl =
						t.get<sol::optional<sol::table>>("move")) {
			move.x = move_tbl->get_or("x", move.x);
			move.y = move_tbl->get_or("y", move.y);
			move.z = move_tbl->get_or("z", move.z);
			changed = true;
		}
		if (sol::optional<float> y = t.get<sol::optional<float>>("yaw")) {
			yaw = *y;
			changed = true;
		}
		if (sol::optional<float> pi = t.get<sol::optional<float>>("pitch")) {
			pitch = *pi;
			changed = true;
		}
		const std::uint8_t new_buttons = buttons_from_table(t, buttons);
		changed = changed || new_buttons != buttons;
		buttons = new_buttons;
		const std::uint32_t new_keybinds =
				keybinds_from_table(t, keybinds, keybind_names);
		changed = changed || new_keybinds != keybinds;
		keybinds = new_keybinds;
		const std::uint8_t new_selected_slot =
				selected_slot_from_table(t, selected_slot);
		changed = changed || new_selected_slot != selected_slot;
		selected_slot = new_selected_slot;
	}

	if (changed) {
		result.replacement = net::ServerSession::PlayerInputOverride{ move, yaw,
			pitch, buttons, keybinds, selected_slot };
	}
	return result;
}

net::ServerSession::ChatHookResult PackRuntime::Impl::run_chat(
		core::NetId sender, std::string_view text) {
	net::ServerSession::ChatHookResult result;
	auto it = handlers.find("chat");
	if (it == handlers.end()) {
		return result; // no handlers registered: pass through unchanged
	}

	std::string current(text);
	bool changed = false;
	PlayerHandle p{ sender, this };
	for (auto &fn : it->second) {
		if (!fn.valid()) {
			continue;
		}
		vm.begin_call_budget();
		sol::protected_function_result r = fn(p, current);
		if (!r.valid()) {
			const sol::error e = r;
			VB_WARN("script", "vb.on('chat') handler error: ", e.what());
			continue;
		}
		const sol::object ret = r;
		if (ret.valid() && ret.get_type() == sol::type::boolean && !ret.as<bool>()) {
			result.veto = true;
			return result; // first veto wins, same as run_veto
		}
		if (ret.valid() && ret.get_type() == sol::type::string) {
			current = ret.as<std::string>();
			changed = true;
		}
	}

	if (changed) {
		result.replacement_text = current;
	}
	return result;
}

namespace {
sol::table make_pos_table(sol::state &lua, core::IVec3 pos) {
	sol::table t = lua.create_table();
	t["x"] = pos.x;
	t["y"] = pos.y;
	t["z"] = pos.z;
	return t;
}
} // namespace

bool PackRuntime::Impl::run_block_break_begin(core::NetId player, core::IVec3 pos) {
	PlayerHandle p{ player, this };
	return run_veto("block_break_begin", p, make_pos_table(lua_state(), pos));
}

// Phase 6.5: sums every registered handler's returned delta, matching §10.7's
// "the engine sums all concurrent contributors' deltas" -- concurrency there
// is across *players* (one call each, from ServerSession), while multiple
// handlers for the *same* call is an orthogonal, less-expected case; summing
// both the same way keeps this simple and never silently drops a delta.
float PackRuntime::Impl::run_block_break_tick(
		core::NetId player, core::IVec3 pos, std::uint16_t max_damage) {
	const auto it = handlers.find("block_break_tick");
	if (it == handlers.end()) {
		return 0.0f; // no policy registered -- damage never accrues (§10.7)
	}
	PlayerHandle p{ player, this };
	sol::table pos_tbl = make_pos_table(lua_state(), pos);
	float total = 0.0f;
	for (auto &fn : it->second) {
		if (!fn.valid()) {
			continue;
		}
		vm.begin_call_budget();
		sol::protected_function_result r = fn(p, pos_tbl, max_damage);
		if (!r.valid()) {
			const sol::error e = r;
			VB_WARN("script", "vb.on('block_break_tick') handler error: ", e.what());
			continue;
		}
		const sol::object ret = r;
		if (ret.valid() && ret.get_type() == sol::type::number) {
			total += ret.as<float>();
		}
	}
	return total;
}

// Phase 6.5: the last handler to return a number wins (chained, like
// run_player_input's replacement pipeline) -- nullopt (no handler, or every
// handler returned nothing) means "unchanged", i.e. permanent damage, no
// healing at all, matching §10.7's "no handler registered" default exactly.
std::optional<float> PackRuntime::Impl::run_block_health_tick(core::IVec3 pos,
		float damage, std::uint16_t max_damage, std::uint64_t ticks_since_last_hit) {
	const auto it = handlers.find("block_health_tick");
	if (it == handlers.end()) {
		return std::nullopt;
	}
	sol::table pos_tbl = make_pos_table(lua_state(), pos);
	std::optional<float> replacement;
	for (auto &fn : it->second) {
		if (!fn.valid()) {
			continue;
		}
		vm.begin_call_budget();
		sol::protected_function_result r = fn(pos_tbl, damage, max_damage,
				static_cast<double>(ticks_since_last_hit));
		if (!r.valid()) {
			const sol::error e = r;
			VB_WARN("script", "vb.on('block_health_tick') handler error: ", e.what());
			continue;
		}
		const sol::object ret = r;
		if (ret.valid() && ret.get_type() == sol::type::number) {
			damage = ret.as<float>();
			replacement = damage;
		}
	}
	return replacement;
}

void PackRuntime::Impl::run_region_event(const std::string &event,
		core::NetId player, core::IVec3 pos, core::BlockId block) {
	PlayerHandle p{ player, this };
	sol::table pos_tbl = make_pos_table(lua_state(), pos);
	// replicator is guaranteed non-null here: attach_world() runs before
	// attach_session() installs this hook (see PackRuntime::attach_session).
	const std::string block_name = replicator->world().registry().get(block).name;
	fire(event, p, pos_tbl, block_name);
}

void PackRuntime::Impl::run_landed_event(core::NetId player, double impact_speed) {
	PlayerHandle p{ player, this };
	fire("player_landed", p, impact_speed);
}

PackRuntime::PackRuntime(net::Transport &transport,
		world::BlockRegistry &registry, std::filesystem::path storage_path,
		VmLimits limits) : impl_(std::make_unique<Impl>(transport, registry,
								   std::move(storage_path), limits)) {}
PackRuntime::~PackRuntime() = default;
PackRuntime::PackRuntime(PackRuntime &&) noexcept = default;
PackRuntime &PackRuntime::operator=(PackRuntime &&) noexcept = default;

#if defined(VB_WITH_AUTOMATION)
bool PackRuntime::admin_give(core::NetId player, std::string_view item_name, std::uint16_t count) {
	const core::BlockId item = impl_->registry.find(item_name);
	if (count == 0 || item == core::BlockId::kAir) {
		return false;
	}
	impl_->give_item(player, item, count);
	impl_->sync_inventory(player);
	return true;
}
#endif

ScriptResult PackRuntime::load_pack_file(std::string_view code,
		std::string_view chunk_name) {
	return impl_->vm.do_string(code, chunk_name);
}

void PackRuntime::set_pack_modules(std::unordered_map<std::string, std::string> modules) {
	impl_->vm.install_require(std::move(modules));
}

void PackRuntime::freeze() {
	impl_->frozen = true;
	if (impl_->registry.size() > world::BlockRegistry::base().size()) {
		VB_INFO("script", "pack registered ", impl_->registry.size(),
				" blocks beyond the base set -- reaches clients only if the "
				"host wires HandshakeServerHost::block_registry from this "
				"registry (Phase 4.3; src/server/main.cpp does)");
	}
}

void PackRuntime::set_auth_required(bool required) { impl_->auth_required = required; }

void PackRuntime::install_join_veto(net::HandshakeServerHost &host) {
	Impl *self = impl_.get();
	// auth_mode none: no login exists, the handler gets (name, nil).
	auto user_auth = host.authenticate;
	host.authenticate = [self, user_auth](std::string_view name,
								std::string_view token) -> net::AuthOutcome {
		net::AuthOutcome outcome = user_auth(name, token);
		if (!outcome.ok) {
			return outcome;
		}
		if (!self->run_veto("player_join", std::string(name),
					sol::make_object(self->lua_state(), sol::lua_nil))) {
			return { false, "denied by pack", {}, {} };
		}
		return outcome;
	};
	// External auth: the FSM calls this after verification, with the final
	// in-game name and the frozen login (auth.md §6).
	host.join_veto = [self](std::string_view name, const net::LoginData *login) {
		sol::state_view lua(self->lua_state());
		const sol::object l = login != nullptr ? login_to_lua(lua, *login)
											   : sol::make_object(lua, sol::lua_nil);
		return self->run_veto("player_join", std::string(name), l);
	};
}

void PackRuntime::install_keybind_registry(net::HandshakeServerHost &host) {
	Impl *self = impl_.get();
	host.keybind_registry =
			[self]() -> std::optional<std::vector<std::string>> {
		if (self->keybind_names.empty()) {
			return std::nullopt;
		}
		return self->keybind_names;
	};
}

void PackRuntime::install_entity_kind_registry(net::HandshakeServerHost &host) {
	Impl *self = impl_.get();
	host.entity_kind_registry =
			[self]() -> std::optional<std::vector<protocol::EntityKindRegistryRecord>> {
		if (self->entity_kinds.empty()) {
			return std::nullopt;
		}
		std::vector<protocol::EntityKindRegistryRecord> out;
		out.reserve(self->entity_kinds.size());
		for (const auto &e : self->entity_kinds) {
			out.push_back({ e.name, e.width, e.height, e.visual, e.hidden });
		}
		return out;
	};
}

void PackRuntime::set_server_config(const core::ServerConfig &config) {
	impl_->server_config = config;
}

void PackRuntime::attach_world(net::WorldReplicator &replicator) {
	impl_->replicator = &replicator;
	Impl *self = impl_.get();
	net::BlockEditHooks hooks;
	hooks.before_edit = [self](core::NetId editor, core::IVec3 pos,
								core::BlockId existing,
								core::BlockId new_block, bool is_break) {
		return self->on_block_edit_before(editor, pos, existing, new_block,
				is_break);
	};
	hooks.after_edit = [self](core::NetId editor, core::IVec3 pos,
							   core::BlockId removed, core::BlockId placed,
							   bool is_break) {
		self->on_block_edit_after(editor, pos, removed, placed, is_break);
	};
	replicator.set_block_edit_hooks(std::move(hooks));
}

physics::MoveParams PackRuntime::effective_move_params(physics::MoveParams base) const {
	if (!impl_->move_params_table) {
		return base;
	}
	const sol::table &def = *impl_->move_params_table;
	physics::MoveParams out = base;
	out.half_width = def.get_or("half_width", out.half_width);
	out.height = def.get_or("height", out.height);
	out.eye_height = def.get_or("eye_height", out.eye_height);
	out.walk_speed = def.get_or("walk_speed", out.walk_speed);
	out.sprint_speed = def.get_or("sprint_speed", out.sprint_speed);
	out.accel = def.get_or("accel", out.accel);
	out.air_accel = def.get_or("air_accel", out.air_accel);
	out.friction = def.get_or("friction", out.friction);
	out.gravity = def.get_or("gravity", out.gravity);
	out.jump_speed = def.get_or("jump_speed", out.jump_speed);
	out.terminal_velocity = def.get_or("terminal_velocity", out.terminal_velocity);
	out.step_height = def.get_or("step_height", out.step_height);
	out.fly_speed = def.get_or("fly_speed", out.fly_speed);
	out.fly = def.get_or("fly", out.fly);
	return out;
}

net::ServerSession::PunchParams PackRuntime::effective_punch_params(
		net::ServerSession::PunchParams base) const {
	if (!impl_->combat_params_table) {
		return base;
	}
	const sol::table &def = *impl_->combat_params_table;
	net::ServerSession::PunchParams out = base;
	out.hit_radius = def.get_or("hit_radius", out.hit_radius);
	out.player_damage = def.get_or("player_damage", out.player_damage);
	out.heal_after_seconds =
			def.get_or("heal_after_seconds", out.heal_after_seconds);
	out.heal_interval_seconds =
			def.get_or("heal_interval_seconds", out.heal_interval_seconds);
	out.punch_cooldown_seconds =
			def.get_or("punch_cooldown_seconds", out.punch_cooldown_seconds);
	return out;
}

net::ActionParams PackRuntime::effective_action_params(net::ActionParams base) const {
	if (!impl_->action_params_table) {
		return base;
	}
	const sol::table &def = *impl_->action_params_table;
	net::ActionParams out = base;
	out.reach = def.get_or("reach", out.reach);
	return out;
}

net::ServerSession::HungerParams PackRuntime::effective_hunger_params(
		net::ServerSession::HungerParams base) const {
	if (!impl_->hunger_params_table) {
		return base;
	}
	const sol::table &def = *impl_->hunger_params_table;
	net::ServerSession::HungerParams out = base;
	out.decay_per_second = def.get_or("decay_per_second", out.decay_per_second);
	out.starvation_damage_per_second = def.get_or(
			"starvation_damage_per_second", out.starvation_damage_per_second);
	return out;
}

std::optional<protocol::S2CFogParams> PackRuntime::effective_fog_params() const {
	if (!impl_->fog_params_table) {
		return std::nullopt;
	}
	const sol::table &def = *impl_->fog_params_table;
	protocol::S2CFogParams out;
	out.fog_start = static_cast<float>(def.get<double>("start"));
	out.fog_end = static_cast<float>(def.get<double>("end"));
	const sol::optional<sol::table> tint = def["underwater_tint"];
	if (tint) {
		out.has_underwater_tint = true;
		out.underwater_tint_r = static_cast<std::uint8_t>(tint->get<double>("r"));
		out.underwater_tint_g = static_cast<std::uint8_t>(tint->get<double>("g"));
		out.underwater_tint_b = static_cast<std::uint8_t>(tint->get<double>("b"));
	}
	return out;
}

std::optional<world::DayNightCurve> PackRuntime::effective_day_night_curve() const {
	if (!impl_->day_night_curve_table) {
		return std::nullopt;
	}
	const sol::table &def = *impl_->day_night_curve_table;
	const sol::table keyframes = def["keyframes"];
	world::DayNightCurve curve;
	curve.keyframes.reserve(keyframes.size());
	for (std::size_t i = 1; i <= keyframes.size(); ++i) {
		const sol::table kf = keyframes[i];
		world::DayNightKeyframe out;
		out.tick = kf.get_or("tick", 0u);
		out.brightness = kf.get_or("brightness", 1.0);
		const sol::optional<sol::table> color = kf["color"];
		if (color) {
			out.color.r = color->get_or(1, std::uint8_t{ 0 });
			out.color.g = color->get_or(2, std::uint8_t{ 0 });
			out.color.b = color->get_or(3, std::uint8_t{ 0 });
		}
		curve.keyframes.push_back(out);
	}
	return curve;
}

double PackRuntime::effective_day_length_seconds(double base) const {
	return impl_->day_length_seconds_override.value_or(base);
}

worldgen::NoiseNodePtr PackRuntime::Impl::parse_noise_node(
		const sol::table &t, std::uint64_t &salt) const {
	auto node = std::make_shared<worldgen::NoiseNode>();
	node->salt = salt++;
	const std::string type = t.get_or("type", std::string("value"));
	if (type == "constant") {
		node->type = worldgen::NoiseNodeType::kConstant;
		node->constant_value = t.get_or("value", 0.0);
	} else if (type == "value") {
		node->type = worldgen::NoiseNodeType::kValue;
		node->frequency = t.get_or("frequency", 1.0);
	} else if (type == "cellular") {
		node->type = worldgen::NoiseNodeType::kCellular;
		node->frequency = t.get_or("frequency", 1.0);
	} else if (type == "fbm") {
		node->type = worldgen::NoiseNodeType::kFbm;
		node->frequency = t.get_or("frequency", 1.0);
		node->octaves = t.get_or("octaves", 4);
		node->lacunarity = t.get_or("lacunarity", 2.0);
		node->gain = t.get_or("gain", 0.5);
		const sol::optional<sol::table> src = t["source"];
		if (src) {
			node->source = parse_noise_node(*src, salt);
		} else {
			auto default_source = std::make_shared<worldgen::NoiseNode>();
			default_source->salt = salt++;
			default_source->type = worldgen::NoiseNodeType::kValue;
			node->source = default_source;
		}
	} else if (type == "remap") {
		node->type = worldgen::NoiseNodeType::kRemap;
		node->in_min = t.get_or("in_min", 0.0);
		node->in_max = t.get_or("in_max", 1.0);
		node->out_min = t.get_or("out_min", 0.0);
		node->out_max = t.get_or("out_max", 1.0);
		const sol::optional<sol::table> src = t["source"];
		if (!src) {
			throw sol::error("vb.noise.remap: 'source' is required");
		}
		node->source = parse_noise_node(*src, salt);
	} else if (type == "combine") {
		node->type = worldgen::NoiseNodeType::kCombine;
		const std::string op = t.get_or("op", std::string("add"));
		node->op = op == "multiply" ? worldgen::NoiseCombineOp::kMultiply
				: op == "min"		? worldgen::NoiseCombineOp::kMin
				: op == "max"		? worldgen::NoiseCombineOp::kMax
									: worldgen::NoiseCombineOp::kAdd;
		const sol::optional<sol::table> ta = t["a"];
		const sol::optional<sol::table> tb = t["b"];
		if (!ta || !tb) {
			throw sol::error("vb.noise.combine: 'a' and 'b' are required");
		}
		node->a = parse_noise_node(*ta, salt);
		node->b = parse_noise_node(*tb, salt);
	} else {
		throw sol::error("vb.noise: unknown node type '" + type + "'");
	}
	return node;
}

std::string PackRuntime::Impl::build_decoration(std::vector<worldgen::StructureDef> &structures,
		std::vector<std::vector<worldgen::PlacementRule>> &decoration) const {
	structures.clear();
	decoration.clear();
	std::string error;
	for (const auto &spec : structure_specs) {
		worldgen::StructureDef def;
		if (!worldgen::resolve_structure(spec, registry, def, error)) {
			return error;
		}
		structures.push_back(std::move(def));
	}
	const auto find_structure = [&](const std::string &name) -> int {
		for (std::size_t i = 0; i < structure_specs.size(); ++i) {
			if (structure_specs[i].name == name) {
				return static_cast<int>(i);
			}
		}
		return -1;
	};

	for (const auto &b : biomes) {
		std::vector<worldgen::PlacementRule> out;
		const sol::object deco_obj = b.raw.get<sol::object>("decoration");
		// A plain string (content/base's `decoration = "trees"`) records
		// intent only and is ignored, same as before structures existed.
		if (deco_obj.get_type() == sol::type::table) {
			const std::string who = "biome '" + b.name + "' decoration";
			std::size_t entry_index = 0;
			for (const auto &kv : deco_obj.as<sol::table>()) {
				++entry_index;
				if (kv.second.get_type() != sol::type::table) {
					continue;
				}
				const sol::table entry_tbl = kv.second.as<sol::table>();
				const std::string ctx = who + " entry " + std::to_string(entry_index);
				double spawn_rate = 0.0;
				const sol::object rate_obj = entry_tbl.get<sol::object>("spawn_rate");
				if (rate_obj.get_type() == sol::type::number) {
					spawn_rate = rate_obj.as<double>();
				}

				const sol::object structure_obj = entry_tbl.get<sol::object>("structure");
				worldgen::PlacementRule rule;
				if (structure_obj.get_type() == sol::type::string) {
					const std::string sname = structure_obj.as<std::string>();
					const int index = find_structure(sname);
					if (index < 0) {
						return ctx + ": unknown structure '" + sname + "'";
					}
					worldgen::PlacementSpec over;
					try {
						over = parse_placement(entry_tbl, ctx, { "structure", "spawn_rate" });
					} catch (const sol::error &e) {
						return e.what();
					}
					const auto &spec = structure_specs[static_cast<std::size_t>(index)];
					if (!worldgen::resolve_placement(static_cast<std::uint32_t>(index), spawn_rate,
								worldgen::merge_placement(spec.placement, over), registry, ctx,
								rule, error)) {
						return error;
					}
				} else {
					// Inline `{blocks = {{x=,y=,z=,block=}, ...}}` form: an
					// anonymous one-variant structure that overwrites
					// everything and never rotates.
					const sol::optional<sol::table> blocks_tbl = entry_tbl["blocks"];
					struct Offset {
						int x, y, z;
						core::BlockId block;
					};
					std::vector<Offset> offsets;
					if (blocks_tbl) {
						for (const auto &bkv : *blocks_tbl) {
							if (bkv.second.get_type() != sol::type::table) {
								continue;
							}
							const sol::table bo = bkv.second.as<sol::table>();
							const std::string bname = bo.get_or("block", std::string{});
							offsets.push_back({ bo.get_or("x", 0), bo.get_or("y", 0),
									bo.get_or("z", 0),
									bname.empty() ? core::BlockId::kAir : registry.find(bname) });
						}
					}
					if (offsets.empty()) {
						continue;
					}
					core::IVec3 lo{ offsets[0].x, offsets[0].y, offsets[0].z };
					core::IVec3 hi = lo;
					for (const Offset &o : offsets) {
						lo = { std::min(lo.x, o.x), std::min(lo.y, o.y), std::min(lo.z, o.z) };
						hi = { std::max(hi.x, o.x), std::max(hi.y, o.y), std::max(hi.z, o.z) };
					}
					const core::IVec3 size{ hi.x - lo.x + 1, hi.y - lo.y + 1, hi.z - lo.z + 1 };
					if (size.x > worldgen::kMaxStructureDim || size.y > worldgen::kMaxStructureDim ||
							size.z > worldgen::kMaxStructureDim) {
						return ctx + ": inline blocks span more than " +
								std::to_string(worldgen::kMaxStructureDim) + " blocks";
					}
					worldgen::StructureDef anon;
					anon.name = b.name + "#inline" + std::to_string(entry_index);
					anon.anchor = { -lo.x, -lo.y, -lo.z };
					anon.radius_xz = std::max({ anon.anchor.x, size.x - 1 - anon.anchor.x,
							anon.anchor.z, size.z - 1 - anon.anchor.z });
					worldgen::StructureVariant variant;
					variant.size = size;
					variant.cells.assign(
							static_cast<std::size_t>(size.x) * static_cast<std::size_t>(size.y) *
									static_cast<std::size_t>(size.z),
							worldgen::kKeepCell);
					for (const Offset &o : offsets) {
						const std::size_t idx =
								(static_cast<std::size_t>(o.y - lo.y) * static_cast<std::size_t>(size.z) +
										static_cast<std::size_t>(o.z - lo.z)) *
										static_cast<std::size_t>(size.x) +
								static_cast<std::size_t>(o.x - lo.x);
						variant.cells[idx] = o.block;
					}
					anon.variants.push_back(std::move(variant));
					rule.structure = static_cast<std::uint32_t>(structures.size());
					structures.push_back(std::move(anon));
					rule.spawn_rate = spawn_rate;
					rule.replace = worldgen::ReplacePolicy::kAll;
					rule.rotate = false;
					rule.mirror = false;
					rule.min_spacing = 1;
					rule.max_slope = 1 << 20;
					rule.y_min = -(1 << 20);
					rule.y_max = 1 << 20;
				}
				out.push_back(std::move(rule));
			}
		}
		decoration.push_back(std::move(out));
	}
	return {};
}

ScriptResult PackRuntime::validate_worldgen() const {
	if (impl_->worldgen_pipeline_table) {
		const std::string beach = impl_->worldgen_pipeline_table->get_or("beach", std::string{});
		if (!beach.empty() && impl_->registry.find(beach) == core::BlockId::kAir &&
				impl_->registry.get(core::BlockId::kAir).name != beach) {
			return { false, core::ScriptError::kRuntime,
				"vb.worldgen.set_pipeline: 'beach' names unknown block '" + beach + "'" };
		}
	}
	std::vector<worldgen::StructureDef> structures;
	std::vector<std::vector<worldgen::PlacementRule>> decoration;
	const std::string err = impl_->build_decoration(structures, decoration);
	if (err.empty()) {
		return ScriptResult::success();
	}
	return { false, core::ScriptError::kRuntime, err };
}

std::shared_ptr<const worldgen::PackWorldGenPipeline> PackRuntime::Impl::build_worldgen_pipeline(
		const worldgen::WorldGenParams &base) const {
	if (!worldgen_pipeline_table) {
		return nullptr;
	}
	const sol::table &def = *worldgen_pipeline_table;
	auto pipeline = std::make_shared<worldgen::PackWorldGenPipeline>();

	// Height field: bakes base_height/amplitude in so WorldGenerator's own
	// surface_height() doesn't need to know it's talking to a pack pipeline
	// vs. the fixed default -- same centred-[-1,1)-then-scaled remap the
	// fixed path already uses, for continuity of feel.
	const sol::table height_def = def["height"]; // validated non-null at set_pipeline() call time
	std::uint64_t salt = 0;
	const worldgen::NoiseNodePtr height_node = parse_noise_node(height_def, salt);
	const double base_height = def.get_or("base_height", base.base_height);
	const double amplitude = def.get_or("amplitude", base.amplitude);
	const std::uint64_t seed = base.seed;
#if VB_WITH_WORLDGEN
	// Real FastNoise2 backend (Phase 6.14): same IR, a different evaluator --
	// see vb/worldgen/fastnoise2_compile.hpp's header comment.
	std::function<double(double, double)> height_eval =
			worldgen::compile_fastnoise2_2d(height_node, seed);
#else
	std::function<double(double, double)> height_eval =
			[height_node, seed](double x, double z) { return height_node->eval2(seed, x, z); };
#endif
	pipeline->height_field = [height_eval, base_height, amplitude](double x, double z) {
		const double n = height_eval(x, z);
		return base_height + (n * 2.0 - 1.0) * amplitude;
	};
	pipeline->sea_level = def.get_or("sea_level", base.sea_level);
	pipeline->soil_depth = def.get_or("soil_depth", base.soil_depth);
	if (const std::string beach = def.get_or("beach", std::string{}); !beach.empty()) {
		pipeline->beach = registry.find(beach);
	}

	// Biomes: every vb.register_biome entry becomes a worldgen::BiomeEntry,
	// resolved surface/filler/stone block ids and a same-order adjacency
	// vector (missing pairs default to the neutral 1.0 multiplier).
	std::vector<worldgen::BiomeEntry> entries;
	entries.reserve(biomes.size());
	for (const auto &b : biomes) {
		worldgen::BiomeEntry e;
		e.name = b.name;
		e.probability = b.raw.get_or("probability", 1.0);
		const std::string surface_name = b.raw.get_or("surface", std::string{});
		const std::string filler_name = b.raw.get_or("filler", std::string{});
		const std::string stone_name = b.raw.get_or("stone", std::string{});
		e.surface = surface_name.empty() ? core::BlockId::kAir : registry.find(surface_name);
		e.filler = filler_name.empty() ? core::BlockId::kAir : registry.find(filler_name);
		e.stone = stone_name.empty() ? core::BlockId::kAir : registry.find(stone_name);
		entries.push_back(std::move(e));
	}
	for (std::size_t i = 0; i < biomes.size(); ++i) {
		entries[i].adjacency.assign(entries.size(), 1.0);
		const sol::optional<sol::table> adjacency = biomes[i].raw["adjacency"];
		if (!adjacency) {
			continue;
		}
		for (const auto &kv : *adjacency) {
			if (kv.first.get_type() != sol::type::string) {
				continue;
			}
			const std::string other_name = kv.first.as<std::string>();
			const double weight = kv.second.is<double>() ? kv.second.as<double>() : 1.0;
			for (std::size_t j = 0; j < biomes.size(); ++j) {
				if (biomes[j].name == other_name) {
					entries[i].adjacency[j] = weight;
					break;
				}
			}
		}
	}
	const double cell_size = def.get_or("cell_size", 256.0);
	pipeline->biomes = worldgen::BiomeSelector(base.seed, cell_size, entries);

	// Decoration: index-aligned with `entries`/`pipeline->biomes` -- always
	// one (possibly empty) rule list per biome so decoration_for()'s index
	// lookup stays valid. See build_decoration.
	{
		const std::string err = build_decoration(pipeline->structures, pipeline->decoration);
		if (!err.empty()) {
			// validate_worldgen() already reports this at startup; a caller
			// that skipped it gets terrain with no decoration, not a crash.
			VB_ERROR("script", "worldgen decoration disabled: ", err);
			pipeline->structures.clear();
			pipeline->decoration.assign(biomes.size(), {});
		}
		pipeline->replaceable.assign(registry.size(), 0);
		for (std::size_t i = 0; i < registry.size(); ++i) {
			pipeline->replaceable[i] =
					registry.get(static_cast<core::BlockId>(i)).replaceable ? 1 : 0;
		}
	}

	// Carvers (spec §6 stage 4) and veins (stage 5): top-level pipeline
	// tables, not per-biome.
	const sol::optional<sol::table> carvers_tbl = def["carvers"];
	if (carvers_tbl) {
		for (const auto &kv : *carvers_tbl) {
			if (kv.second.get_type() != sol::type::table) {
				continue;
			}
			const sol::table c = kv.second.as<sol::table>();
			const sol::optional<sol::table> noise_def = c["noise"];
			if (!noise_def) {
				continue; // malformed entry, skip rather than fail the whole pipeline
			}
			const worldgen::NoiseNodePtr node = parse_noise_node(*noise_def, salt);
			worldgen::CarverDef cd;
			cd.threshold = c.get_or("threshold", 0.5);
			cd.y_min = c.get_or("y_min", 0);
			cd.y_max = c.get_or("y_max", 255);
#if VB_WITH_WORLDGEN
			cd.density = worldgen::compile_fastnoise2_3d(node, seed);
#else
			cd.density = [node, seed](double x, double y, double z) {
				return node->eval3(seed, x, y, z);
			};
#endif
			pipeline->carvers.push_back(std::move(cd));
		}
	}
	const sol::optional<sol::table> veins_tbl = def["veins"];
	if (veins_tbl) {
		for (const auto &kv : *veins_tbl) {
			if (kv.second.get_type() != sol::type::table) {
				continue;
			}
			const sol::table v = kv.second.as<sol::table>();
			worldgen::VeinDef vd;
			const std::string block_name = v.get_or("block", std::string{});
			const std::string target_name =
					v.get_or("target_rock", std::string("base:stone"));
			vd.block = block_name.empty() ? core::BlockId::kAir : registry.find(block_name);
			vd.target_rock = registry.find(target_name);
			vd.height_min = v.get_or("height_min", 0);
			vd.height_max = v.get_or("height_max", 63);
			vd.vein_size = v.get_or("vein_size", 6);
			vd.spawn_rate = v.get_or("spawn_rate", 0.02);
			pipeline->veins.push_back(vd);
		}
	}

	return pipeline;
}

std::shared_ptr<const worldgen::PackWorldGenPipeline> PackRuntime::build_worldgen_pipeline(
		const worldgen::WorldGenParams &base) const {
	return impl_->build_worldgen_pipeline(base);
}

void PackRuntime::attach_session(net::ServerSession &session) {
	impl_->session = &session;
	session.set_on_script_entity_orphaned([this](core::NetId id) {
		impl_->despawn_entity(id, "parent_removed");
	});
	session.set_ui_event_handler(
			[this](core::NetId player, const protocol::C2SUiEvent &e) {
				dispatch_ui_event(player, e);
			});
	session.set_chat_handler([this](core::NetId sender, std::string_view text) {
		return dispatch_chat(sender, text);
	});
	// Phase 5.1: a player walking over a dropped item (ItemDropSystem,
	// ServerSession::update_item_drops) credits their inventory exactly like
	// player:give() does, including the same live S2C_Inventory push --
	// picking something up should look identical to a script handing it to
	// you directly.
	Impl *self = impl_.get();
	session.set_item_pickup_handler(
			[self](core::NetId player, core::BlockId item, std::uint16_t count) {
				self->give_item(player, item, count);
				self->sync_inventory(player);
			});
	// Phase 6.6: only installed when a pack actually registered
	// vb.on("player_death", ...) (all pack loading finished before
	// attach_session() runs, so `handlers` is already final) -- otherwise
	// ServerSession keeps its own built-in fallback, matching every
	// pre-6.6 caller/test exactly.
	if (self->handlers.count("player_death") != 0) {
		session.set_respawn_handler([self](core::NetId id, std::string_view cause,
											float health_before) {
			return self->run_respawn_handler(id, cause, health_before);
		});
	}
	// Phase 6.3: only installed when a pack actually registered
	// vb.on("player_input", ...) -- packs that never use this channel pay
	// zero extra cost in the hot per-tick handle_input_batch loop.
	if (self->handlers.count("player_input") != 0) {
		session.set_input_handler(
				[self](core::NetId id, const protocol::InputCmd &cmd) {
					return self->run_player_input(id, cmd);
				});
	}
	// Phase 6.5: only installed when a pack registered at least one of the
	// three block-damage events -- a pack that never opts in pays zero extra
	// cost in ServerSession's per-tick damage-map walk (it stays empty since
	// `begin` is never installed to admit a contributor in the first place).
	if (self->handlers.count("block_break_begin") != 0 ||
			self->handlers.count("block_break_tick") != 0 ||
			self->handlers.count("block_health_tick") != 0) {
		net::ServerSession::BlockBreakHooks hooks;
		hooks.begin = [self](core::NetId player, core::IVec3 pos, core::BlockId) {
			return self->run_block_break_begin(player, pos);
		};
		hooks.tick_damage = [self](core::NetId player, core::IVec3 pos,
									core::BlockId, std::uint16_t max_damage) {
			return self->run_block_break_tick(player, pos, max_damage);
		};
		hooks.health_tick = [self](core::IVec3 pos, core::BlockId, float damage,
									std::uint16_t max_damage,
									std::uint64_t idle) {
			return self->run_block_health_tick(pos, damage, max_damage, idle);
		};
		session.set_block_break_hooks(std::move(hooks));
	}
	// Phase 7.3: only installed when a pack registered at least one of the
	// two region events -- ServerSession's own per-tick walk stays a no-op
	// (both std::function fields unset) for every pack that never opts in,
	// same zero-extra-cost posture as BlockBreakHooks above.
	if (self->handlers.count("region_enter") != 0 ||
			self->handlers.count("region_exit") != 0) {
		net::ServerSession::RegionHooks hooks;
		hooks.enter = [self](core::NetId player, core::IVec3 pos,
							  core::BlockId block) {
			self->run_region_event("region_enter", player, pos, block);
		};
		hooks.exit = [self](core::NetId player, core::IVec3 pos,
							 core::BlockId block) {
			self->run_region_event("region_exit", player, pos, block);
		};
		session.set_region_hooks(std::move(hooks));
	}
	// Phase 6.22: only installed when a pack actually registered
	// vb.on("player_landed", ...) -- same zero-extra-cost-when-unused posture
	// as every other opt-in hook above (ServerSession's handle_input_batch
	// still computes was_on_ground/fall_speed_before unconditionally, since
	// that's cheap arithmetic already in the loop, but never calls into Lua
	// unless a pack asked for it).
	if (self->handlers.count("player_landed") != 0) {
		session.set_landed_hook([self](core::NetId player, double impact_speed) {
			self->run_landed_event(player, impact_speed);
		});
	}
	// Entity-management follow-up: forwards a pack's `represents = "player"`/
	// `"item_drop"` claim (vb.register_entity) onto ServerSession's own
	// replication-time kind tagging for players/dropped items -- unset means
	// neither setter is called, exact pre-existing behavior (players replicate
	// with EntityKindId::kInvalid, drops with the reserved world::kItemDropKind
	// sentinel).
	if (self->player_kind_id) {
		session.set_player_visual_kind(*self->player_kind_id);
	}
	if (self->item_drop_kind_id) {
		session.set_item_drop_visual_kind(*self->item_drop_kind_id);
	}
	// Formalizes dispatch_tick (global `tick` event + entity on_tick/on_hit/
	// on_death + timers) as a real ServerSession::build_systems() phase
	// instead of a separate step each embedder's own loop used to call after
	// session.tick() had already finished the whole SystemRunner for that
	// tick -- see ServerSession::set_script_tick_hook's own comment. Always
	// installed (unlike the conditional hooks above): dispatch_tick does
	// real work -- global timers, the `tick` event -- regardless of whether
	// this pack registered any entity-kind callbacks at all.
	session.set_script_tick_hook([this](double dt) { dispatch_tick(dt); });
}

void PackRuntime::dispatch_player_join_completed(
		const net::SessionPlayerJoined &j) {
	impl_->player_names[j.net_id] = j.name;
	if (j.login) {
		impl_->logins[j.net_id] = j.login;
	}
}

void PackRuntime::dispatch_login_changed(const net::SessionLoginChanged &c) {
	if (!c.login) {
		return;
	}
	impl_->logins[c.net_id] = c.login;
	impl_->login_tables.erase(c.net_id); // next get_login() sees the new claims
	PlayerHandle p{ c.net_id, impl_.get() };
	sol::state_view lua(impl_->lua_state());
	impl_->fire("login_changed", p, impl_->login_object(c.net_id, lua));
}

void PackRuntime::dispatch_player_leave(const net::SessionPlayerLeft &l) {
	if (!l.name.empty()) {
		impl_->player_names[l.net_id] = l.name;
	}
	if (l.last_state) {
		impl_->leaving_states[l.net_id] = *l.last_state;
	}
	PlayerHandle p{ l.net_id, impl_.get() };
	impl_->fire("player_leave", p);
	impl_->leaving_states.erase(l.net_id);
	impl_->player_names.erase(l.net_id);
	impl_->logins.erase(l.net_id);
	impl_->login_tables.erase(l.net_id);
	impl_->inventories.erase(l.net_id);
}

void PackRuntime::dispatch_tick(double dt_seconds) {
	impl_->dispatch_tick(dt_seconds);
}

net::ServerSession::ChatHookResult PackRuntime::dispatch_chat(
		core::NetId sender, std::string_view text) {
	return impl_->run_chat(sender, text);
}

void PackRuntime::dispatch_ui_event(core::NetId player,
		const protocol::C2SUiEvent &event) {
	PlayerHandle p{ player, impl_.get() };
	nlohmann::json parsed;
	try {
		parsed = nlohmann::json::parse(event.value_json);
	} catch (const nlohmann::json::parse_error &) {
		parsed = nullptr;
	}
	sol::object value = json_to_lua(impl_->lua_state(), parsed);
	impl_->fire("ui_event", p, event.ui_name, event.widget_id, event.event_kind,
			value);
}

bool PackRuntime::dispatch_player_interact(core::NetId player,
		core::IVec3 target) {
	PlayerHandle p{ player, impl_.get() };
	sol::table t = impl_->lua_state().create_table();
	t["x"] = target.x;
	t["y"] = target.y;
	t["z"] = target.z;
	return impl_->run_veto("player_interact", p, t);
}

bool PackRuntime::storage_dirty() const { return impl_->storage_dirty_flag; }

void PackRuntime::flush_storage() { impl_->flush_storage(); }

std::uint64_t PackRuntime::storage_revision() const {
	return impl_->storage_revision_counter;
}

std::vector<std::string> PackRuntime::global_names() {
	return list_lua_globals(impl_->vm);
}

std::vector<std::string> PackRuntime::describe_api() {
	std::vector<std::string> names = describe_lua_surface(impl_->vm, { "vb" });
	for (auto &n : names) {
		// The C++ class is PlayerHandle; scripts know it as `Player`.
		if (n.rfind("PlayerHandle:", 0) == 0) {
			n = "Player" + n.substr(sizeof("PlayerHandle") - 1);
		}
	}
	// `self` in a vb.register_entity callback: a per-instance table whose
	// metatable __index is this shared method table (Phase 6.1), unreachable
	// from the globals, so list it explicitly.
	for (const auto &kv : impl_->entity_methods) {
		if (kv.first.is<std::string>()) {
			names.push_back("Entity:" + kv.first.as<std::string>());
		}
	}
	std::sort(names.begin(), names.end());
	return names;
}

} // namespace vb::script

#endif // VB_WITH_LUA
