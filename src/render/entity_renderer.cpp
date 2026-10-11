#include "vb/render/entity_renderer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <raylib.h>
#include <raymath.h>
#include <rlgl.h>

#include "vb/core/ids.hpp"
#include "vb/core/log.hpp"
#include "vb/net/session.hpp"
#include "vb/render/chunk_renderer.hpp"
#include "vb/render/entity_visual.hpp"
#include "vb/render/entity_visual_layout.hpp"

namespace vb::render {

namespace {

constexpr int kDefaultFacings = 8;
constexpr float kPlaceholderWidth = 0.8f; // metres
constexpr float kPlaceholderHeight = 1.8f; // matches physics::MoveParams::height

Camera3D to_raylib_camera(const CameraView &v) {
	Camera3D cam{};
	cam.position = { static_cast<float>(v.position.x),
		static_cast<float>(v.position.y), static_cast<float>(v.position.z) };
	cam.target = { static_cast<float>(v.target.x),
		static_cast<float>(v.target.y), static_cast<float>(v.target.z) };
	cam.up = { 0.0f, 1.0f, 0.0f };
	cam.fovy = 60.0f; // unused by DrawBillboardPro's view-matrix math
	cam.projection = CAMERA_PERSPECTIVE;
	return cam;
}

// A cheap, deterministic hash -> hue so distinct entities are visually
// distinguishable even as flat placeholder quads (real art replaces this).
Color tint_for_entity(core::NetId id) {
	std::uint32_t h = static_cast<std::uint32_t>(id) * 2654435761u;
	h ^= h >> 16;
	const float hue = static_cast<float>(h % 360u);
	return ColorFromHSV(hue, 0.55f, 0.85f);
}

} // namespace

// Entity-management follow-up to Phase 6.1: a script entity's registered
// vb.register_entity{width=, height=} (protocol::EntityKindRegistryRecord,
// looked up via ClientSession::entity_kind()) replaces the flat
// kPlaceholderWidth/kPlaceholderHeight for its billboard -- players
// (EntityRecord::kind == kInvalid) and any kind with no registry entry (host
// never opted in) keep the placeholder defaults, same "missing = default"
// posture as every other opt-in registry in this codebase.
struct TrackedEntity {
	EntityPresentationState state;
	core::EntityKindId kind = core::EntityKindId::kInvalid;
	float width = kPlaceholderWidth;
	float height = kPlaceholderHeight;
	// The facings/mirror `state` was actually constructed with -- sync() below
	// re-checks these against whichever KindVisual now applies (arriving
	// mid-flight, since the kind registry and the first snapshot for an
	// entity aren't ordering-guaranteed) and rebuilds `state` on a change, so
	// pose selection never runs against a stale/default facings count.
	int facings = kDefaultFacings;
	bool mirror = true;
	// Set for a dropped-item entity: draw_item_cube() instead of a billboard.
	std::optional<core::BlockId> item;
	// `vb.register_entity{visual = false}`: no sprite at all (text only).
	bool hidden = false;
	// The entity's current label, copied from ClientSession::entity_text()
	// every sync(); nullopt = none.
	std::optional<protocol::EntityText> text;
	// Where to draw it this frame: the interpolated position, or for an
	// attached entity its parent's position + offset (resolved in sync()).
	core::Vec3d render_pos{};
	// Depth-order step (protocol::EntityVisualDef::layer, overridden per
	// instance and by an attachment) and the "draw over terrain" flag.
	int layer = 0;
	bool through_walls = false;
	// entity:set_clip(name), and how long it has been playing.
	std::optional<std::string> forced_clip;
	double forced_clip_time = 0.0;

	explicit TrackedEntity(int initial_facings) : state(initial_facings) {}
};

// A resolved, drawable visual: the base sheet plus its paper-doll layers
// (only those whose sheet loaded and matches the base size). Textures are
// owned by Impl::textures, so swapping outfits never reloads a file.
struct KindVisual {
	Texture2D texture{};
	EntityVisualLayout layout;
	std::vector<protocol::EntityVisualLayer> layers;
	std::vector<Texture2D> layer_textures; // parallel to `layers`
};

// A decoded sheet, cached by pack path for the whole session.
struct LoadedTexture {
	Texture2D texture{};
	int width = 0;
	int height = 0;
};

// A per-instance visual (override merged over the kind default) and the
// merged def it was built from: rebuilt whenever that def changes.
struct InstanceVisual {
	protocol::EntityVisualDef def;
	std::optional<KindVisual> visual;
};

struct EntityRenderer::Impl {
	Texture2D placeholder{};
	std::unordered_map<core::NetId, TrackedEntity> states;
	std::unordered_map<core::EntityKindId, KindVisual> kind_visuals;
	// Per-NetId visuals for entities (and players) with a visual override,
	// rebuilt in sync() whenever the override -- which the server can change
	// at any time (S2C_EntityProps) -- or the kind default changes. Takes
	// priority over kind_visuals in draw() when it resolved.
	std::unordered_map<core::NetId, InstanceVisual> instance_visuals;
	// Every sheet decoded so far, by pack path; nullopt = failed to load
	// (already warned about, never retried).
	std::unordered_map<std::string, std::optional<LoadedTexture>> textures;
	VirtualFs vfs; // set once, right after join, by set_virtual_fs()
	std::filesystem::path disk_root; // set_disk_fallback()
	bool draw_local_player = false; // set_draw_local_player()
	const ChunkRenderer *chunks = nullptr; // set_block_colors()
	// Alpha-cutout variant of raylib's default batch shader, so a sprite's
	// clear margin never writes depth and hides an entity behind it.
	Shader cutout{};
	int cutout_loc_alpha = -1;
	// "<texture>/<clip>" pairs already warned about falling back to the
	// first clip -- once each, so a missing clip is easy to spot in the log.
	mutable std::unordered_set<std::string> warned_clip_fallbacks;
};

namespace {

// raylib's default batch shader (rlgl.h RL_DEFAULT_SHADER_*), plus a discard
// below `alphaCutoff` -- the same cutout block textures get (see
// chunk_renderer.cpp's kFogFs), here for billboards and dropped items.
constexpr const char *kCutoutVs = R"(#version 330
in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec4 vertexColor;
out vec2 fragTexCoord;
out vec4 fragColor;
uniform mat4 mvp;
void main()
{
    fragTexCoord = vertexTexCoord;
    fragColor = vertexColor;
    gl_Position = mvp*vec4(vertexPosition, 1.0);
}
)";

constexpr const char *kCutoutFs = R"(#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
out vec4 finalColor;
uniform sampler2D texture0;
uniform vec4 colDiffuse;
uniform float alphaCutoff;
void main()
{
    vec4 c = texture(texture0, fragTexCoord)*colDiffuse*fragColor;
    if (c.a < alphaCutoff) {
        discard;
    }
    finalColor = c;
}
)";

// Low enough that soft edges and glows still blend (sprites are drawn back
// to front for that), high enough that a clear margin writes no depth.
constexpr float kAlphaCutoff = 0.02f;
// How far one layer step moves a billboard toward the camera.
constexpr double kLayerStep = 0.02;

} // namespace

EntityRenderer::EntityRenderer() : impl_(std::make_unique<Impl>()) {
	Image img = GenImageColor(1, 1, WHITE);
	impl_->placeholder = LoadTextureFromImage(img);
	UnloadImage(img);
	impl_->cutout = LoadShaderFromMemory(kCutoutVs, kCutoutFs);
	impl_->cutout_loc_alpha = GetShaderLocation(impl_->cutout, "alphaCutoff");
	SetShaderValue(impl_->cutout, impl_->cutout_loc_alpha, &kAlphaCutoff,
			SHADER_UNIFORM_FLOAT);
}

EntityRenderer::~EntityRenderer() {
	if (impl_->placeholder.id != 0) {
		UnloadTexture(impl_->placeholder);
	}
	if (impl_->cutout.id != 0) {
		UnloadShader(impl_->cutout);
	}
	for (auto &[path, tex] : impl_->textures) {
		(void)path;
		if (tex && tex->texture.id != 0) {
			UnloadTexture(tex->texture);
		}
	}
}

// Decodes (once) and caches the sheet at pack path `path`: from the synced
// pack, or in singleplayer from the content directory on disk. nullptr if it
// can't be found or decoded.
const LoadedTexture *EntityRenderer::load_texture(std::string_view context,
		const std::string &path, const VirtualFs &vfs) {
	if (const auto it = impl_->textures.find(path); it != impl_->textures.end()) {
		return it->second ? &*it->second : nullptr;
	}
	std::optional<LoadedTexture> loaded;
	std::vector<std::byte> bytes;
	if (const auto it = vfs.find(path); it != vfs.end()) {
		bytes = it->second;
	} else if (const auto it2 = impl_->vfs.find(path); it2 != impl_->vfs.end()) {
		bytes = it2->second;
	} else if (!impl_->disk_root.empty() && path.find("..") == std::string::npos) {
		std::ifstream f(impl_->disk_root / path, std::ios::binary);
		if (f) {
			const std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
			bytes.resize(data.size());
			std::memcpy(bytes.data(), data.data(), data.size());
		}
	}
	if (bytes.empty()) {
		VB_WARN("render", context, ": texture '", path, "' not found in the pack");
	} else {
		Image decoded = LoadImageFromMemory(".png",
				reinterpret_cast<const unsigned char *>(bytes.data()),
				static_cast<int>(bytes.size()));
		if (decoded.data == nullptr) {
			VB_WARN("render", context, ": failed to decode '", path, "'");
		} else {
			loaded = LoadedTexture{ LoadTextureFromImage(decoded), decoded.width, decoded.height };
			UnloadImage(decoded);
		}
	}
	auto [it, inserted] = impl_->textures.emplace(path, std::move(loaded));
	(void)inserted;
	return it->second ? &*it->second : nullptr;
}

// Resolves a full visual def into something draw() can use: the base sheet
// validated against the declared layout, plus every layer whose sheet loads
// and has the base sheet's exact size (others are skipped with a warning).
// nullopt if the base sheet itself is unusable -- the caller keeps its
// fallback (the kind's visual, or the flat placeholder).
std::optional<KindVisual> EntityRenderer::build_visual(std::string_view context,
		const protocol::EntityVisualDef &def, const VirtualFs &vfs) {
	if (def.texture.empty()) {
		return std::nullopt;
	}
	const LoadedTexture *base = load_texture(context, def.texture, vfs);
	if (base == nullptr) {
		return std::nullopt;
	}
	const auto layout = build_entity_visual_layout(def, base->width, base->height);
	if (!layout) {
		VB_WARN("render", context, ": '", def.texture, "' (", base->width, "x",
				base->height, ") doesn't match its declared frame/facings/clip layout");
		return std::nullopt;
	}
	KindVisual visual;
	visual.texture = base->texture;
	visual.layout = *layout;
	for (const protocol::EntityVisualLayer &l : def.layers) {
		const LoadedTexture *tex = load_texture(context, l.texture, vfs);
		if (tex == nullptr) {
			continue;
		}
		if (!layer_sheet_matches(base->width, base->height, tex->width, tex->height)) {
			VB_WARN("render", context, ": layer '", l.texture, "' is ", tex->width, "x",
					tex->height, " but its base sheet '", def.texture, "' is ", base->width,
					"x", base->height, "; skipping that layer");
			continue;
		}
		visual.layers.push_back(l);
		visual.layer_textures.push_back(tex->texture);
	}
	return visual;
}

void EntityRenderer::set_kind_visual(core::EntityKindId id,
		const protocol::EntityVisualDef &def, const VirtualFs &vfs) {
	if (auto visual = build_visual("entity kind visual", def, vfs)) {
		impl_->kind_visuals.insert_or_assign(id, std::move(*visual));
	}
}

void EntityRenderer::set_disk_fallback(std::filesystem::path content_root) {
	impl_->disk_root = std::move(content_root);
}

void EntityRenderer::set_draw_local_player(bool draw) {
	impl_->draw_local_player = draw;
}

void EntityRenderer::set_virtual_fs(VirtualFs vfs) {
	impl_->vfs = std::move(vfs);
}

namespace {

// Feet position and yaw of `id` as this client sees it: its own player from
// local prediction (yaw from the camera), anyone else interpolated.
std::optional<std::pair<core::Vec3d, float>> entity_pose(
		const net::ClientSession &client, core::NetId id, const CameraView &camera) {
	if (client.join_accept() && client.join_accept()->your_net_id == id) {
		const double fx = camera.target.x - camera.position.x;
		const double fz = camera.target.z - camera.position.z;
		// Yaw 0 faces -Z (physics::wish_dir_from_local's convention).
		const float yaw = static_cast<float>(std::atan2(fx, -fz) * 180.0 / 3.14159265358979323846);
		return std::make_pair(client.predicted_feet(), yaw);
	}
	const auto it = client.remote_entities().find(id);
	if (it == client.remote_entities().end()) {
		return std::nullopt;
	}
	return std::make_pair(client.interpolated_pos(id), it->second.rot.x);
}

} // namespace

// An attached entity is drawn at its parent's (recursively resolved) pose +
// offset each frame, so it never trails the parent; if the parent isn't
// visible here, its own replicated position (which the server keeps glued
// to the parent) is used instead.
core::Vec3d EntityRenderer::resolve_render_pos(const net::ClientSession &client,
		core::NetId id, const CameraView &camera, int depth) const {
	const protocol::EntityAttachment *a = client.entity_attachment(id);
	if (a != nullptr && depth < 8) {
		if (const auto parent = entity_pose(client, a->parent, camera)) {
			const core::Vec3d base = client.entity_attachment(a->parent) != nullptr
					? resolve_render_pos(client, a->parent, camera, depth + 1)
					: parent->first;
			const core::Vec3d off = protocol::attachment_world_offset(*a, parent->second);
			return { base.x + off.x, base.y + off.y, base.z + off.z };
		}
	}
	return client.interpolated_pos(id);
}

void EntityRenderer::sync(const net::ClientSession &client,
		const CameraView &camera, double dt_seconds) {
	const auto &remote = client.remote_entities();

	// The local player is never in remote_entities(); in third person it is
	// tracked like any other entity, from local prediction.
	std::optional<protocol::EntityRecord> local;
	if (impl_->draw_local_player && client.join_accept()) {
		protocol::EntityRecord r;
		r.net_id = client.join_accept()->your_net_id;
		r.kind = client.local_entity_kind();
		const physics::MoveState &m = client.predicted_state();
		r.pos = m.position;
		r.vel = core::Vec3f{ static_cast<float>(m.velocity.x),
			static_cast<float>(m.velocity.y), static_cast<float>(m.velocity.z) };
		r.flags = m.on_ground ? kAnimOnGround : std::uint8_t{ 0 };
		const auto pose = entity_pose(client, r.net_id, camera);
		r.rot.x = pose ? pose->second : 0.0f;
		local = r;
	}

	for (auto it = impl_->states.begin(); it != impl_->states.end();) {
		const bool keep = remote.find(it->first) != remote.end() ||
				(local && local->net_id == it->first);
		if (!keep) {
			impl_->instance_visuals.erase(it->first);
			it = impl_->states.erase(it);
		} else {
			++it;
		}
	}

	for (const auto &[id, rec] : remote) {
		track(client, rec, resolve_render_pos(client, id, camera, 0), camera, dt_seconds);
	}
	if (local) {
		track(client, *local, local->pos, camera, dt_seconds);
	}
}

void EntityRenderer::track(const net::ClientSession &client,
		const protocol::EntityRecord &rec, core::Vec3d pos, const CameraView &camera,
		double dt_seconds) {
	const core::NetId id = rec.net_id;
	auto [it, inserted] = impl_->states.try_emplace(id, kDefaultFacings);
	(void)inserted;
	// Re-checked every sync (cheap: one map lookup) rather than only on
	// insert, so a registry that arrives just after this entity's first
	// snapshot still takes effect -- frame arrival order across the
	// S2C_EntityKindRegistry/S2C_EntitySnapshot messages isn't guaranteed.
	it->second.kind = rec.kind;
	it->second.item = client.entity_item(id);
	const protocol::EntityKindRegistryRecord *kind_record = client.entity_kind(rec.kind);
	int layer = 0;
	bool through_walls = false;
	if (kind_record != nullptr) {
		it->second.width = kind_record->width;
		it->second.height = kind_record->height;
		it->second.hidden = kind_record->hidden;
		if (kind_record->visual) {
			layer = kind_record->visual->layer;
			through_walls = kind_record->visual->through_walls;
		}
	}
	const protocol::EntityVisualOverride *override_def = client.entity_visual_override(id);
	if (override_def != nullptr) {
		layer = override_def->layer.value_or(layer);
		through_walls = override_def->through_walls.value_or(through_walls);
	}
	const protocol::EntityAttachment *attachment = client.entity_attachment(id);
	if (attachment != nullptr && attachment->layer) {
		layer = *attachment->layer;
	}
	it->second.layer = layer;
	it->second.through_walls = through_walls;
	if (const std::string *clip = client.entity_clip(id)) {
		if (it->second.forced_clip != *clip) {
			it->second.forced_clip = *clip;
			it->second.forced_clip_time = 0.0;
		} else {
			it->second.forced_clip_time += dt_seconds;
		}
	} else {
		it->second.forced_clip.reset();
	}
	if (const protocol::EntityText *text = client.entity_text(id)) {
		if (!it->second.text || *it->second.text != *text) {
			it->second.text = *text;
		}
	} else {
		it->second.text.reset();
	}
	// A per-instance visual override is merged over the kind's own default
	// and rebuilt whenever the merged result changes (a new outfit, or the
	// kind registry arriving late). Decoded sheets are cached by path, so
	// this only re-validates layouts; nothing is reloaded.
	if (override_def != nullptr) {
		protocol::EntityVisualDef base;
		if (kind_record != nullptr && kind_record->visual) {
			base = *kind_record->visual;
		}
		protocol::EntityVisualDef merged = merge_visual_override(base, *override_def);
		auto inst = impl_->instance_visuals.find(id);
		if (inst == impl_->instance_visuals.end() || inst->second.def != merged) {
			std::optional<KindVisual> visual =
					build_visual("entity instance visual override", merged, impl_->vfs);
			impl_->instance_visuals.insert_or_assign(id, InstanceVisual{ std::move(merged), std::move(visual) });
		}
	} else {
		impl_->instance_visuals.erase(id);
	}
	// Real bug fix: `state` used to be permanently constructed with
	// kDefaultFacings (8) and never updated once the entity's real kind
	// (or instance override) resolved a different facings/mirror -- pose
	// selection silently ran against the wrong sector count/row-mirroring
	// for any kind whose visual declared facings=4 (e.g. base:player),
	// picking rows that could fall outside its own spritesheet.
	// Re-resolve every sync() from whichever KindVisual draw() will
	// actually use for this id, and only rebuild `state` (which would
	// otherwise reset clip_time/the bucket tracker every frame) when it
	// actually changed.
	int resolved_facings = kDefaultFacings;
	bool resolved_mirror = true;
	if (const KindVisual *v = visual_for(id, rec.kind)) {
		resolved_facings = v->layout.facings;
		resolved_mirror = v->layout.mirror;
	}
	if (it->second.facings != resolved_facings || it->second.mirror != resolved_mirror) {
		it->second.facings = resolved_facings;
		it->second.mirror = resolved_mirror;
		it->second.state = EntityPresentationState(resolved_facings, resolved_mirror);
	}
	it->second.render_pos = pos;
	it->second.state.update(pos, static_cast<double>(rec.rot.x), rec.vel,
			rec.flags, camera.position, dt_seconds);
}

// The visual draw() uses for `id`: its resolved instance visual, else its
// kind's, else nullptr (flat placeholder).
const KindVisual *EntityRenderer::visual_for(core::NetId id, core::EntityKindId kind) const {
	if (const auto inst = impl_->instance_visuals.find(id);
			inst != impl_->instance_visuals.end() && inst->second.visual) {
		return &*inst->second.visual;
	}
	if (const auto k = impl_->kind_visuals.find(kind); k != impl_->kind_visuals.end()) {
		return &k->second;
	}
	return nullptr;
}

void EntityRenderer::set_block_colors(const ChunkRenderer *chunks) {
	impl_->chunks = chunks;
}

namespace {

// A dropped item: a small cube spinning and bobbing above its resting point.
// `phase` (the drop's id) desynchronises neighbouring drops.
void draw_item_cube(Vector3 feet, Color color, float phase) {
	constexpr float kSize = 0.3f;
	const float t = static_cast<float>(GetTime());
	const float bob = 0.12f + 0.05f * std::sin(t * 2.5f + phase);
	rlPushMatrix();
	rlTranslatef(feet.x, feet.y + bob + kSize * 0.5f, feet.z);
	rlRotatef(std::fmod(t * 60.0f + phase * 37.0f, 360.0f), 0.0f, 1.0f, 0.0f);
	DrawCube(Vector3{ 0.0f, 0.0f, 0.0f }, kSize, kSize, kSize, color);
	DrawCubeWires(Vector3{ 0.0f, 0.0f, 0.0f }, kSize, kSize, kSize,
			Color{ 0, 0, 0, 120 });
	rlPopMatrix();
}

} // namespace

void EntityRenderer::draw(const CameraView &camera_view) const {
	const Camera3D camera = to_raylib_camera(camera_view);
	const core::Vec3d eye = camera_view.position;

	// Back to front (so soft edges blend over what's behind them), with
	// through-walls sprites last since they ignore the depth buffer. Each
	// layer step moves the billboard kLayerStep toward the camera, so a
	// higher layer at the same spot always wins the depth test and is drawn
	// after the lower one.
	struct Pending {
		core::NetId id;
		const TrackedEntity *tracked;
		Vector3 pos;
		double dist_sq;
	};
	std::vector<Pending> pending;
	pending.reserve(impl_->states.size());
	for (const auto &[id, tracked] : impl_->states) {
		if (tracked.hidden) {
			continue;
		}
		core::Vec3d p = tracked.render_pos;
		if (tracked.layer != 0) {
			const core::Vec3d to_eye{ eye.x - p.x, eye.y - p.y, eye.z - p.z };
			const double len = std::sqrt(to_eye.x * to_eye.x + to_eye.y * to_eye.y + to_eye.z * to_eye.z);
			if (len > 1e-6) {
				const double k = kLayerStep * tracked.layer / len;
				p = { p.x + to_eye.x * k, p.y + to_eye.y * k, p.z + to_eye.z * k };
			}
		}
		const double dx = p.x - eye.x;
		const double dy = p.y - eye.y;
		const double dz = p.z - eye.z;
		pending.push_back({ id, &tracked,
				Vector3{ static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z) },
				dx * dx + dy * dy + dz * dz });
	}
	std::sort(pending.begin(), pending.end(), [](const Pending &x, const Pending &y) {
		if (x.tracked->through_walls != y.tracked->through_walls) {
			return !x.tracked->through_walls;
		}
		if (x.dist_sq != y.dist_sq) {
			return x.dist_sq > y.dist_sq;
		}
		return x.id < y.id; // stable across frames and sessions
	});

	BeginShaderMode(impl_->cutout);
	bool depth_test = true;
	for (const Pending &p : pending) {
		const core::NetId id = p.id;
		const TrackedEntity &tracked = *p.tracked;
		const Vector3 feet = p.pos;
		if (tracked.through_walls && depth_test) {
			rlDrawRenderBatchActive();
			rlDisableDepthTest();
			depth_test = false;
		}
		const EntityPresentationState::Frame &frame = tracked.state.frame();

		// A per-instance override (if it resolved) always wins over the
		// kind's own default visual -- see track().
		const KindVisual *visual = visual_for(id, tracked.kind);
		const bool has_visual = visual != nullptr;

		if (tracked.item && impl_->chunks != nullptr) {
			draw_item_cube(feet, impl_->chunks->underwater_tint(*tracked.item),
					static_cast<float>(id));
			continue;
		}

		Rectangle source;
		Texture2D texture;
		if (has_visual) {
			const EntityVisualLayout &layout = visual->layout;
			texture = visual->texture;
			// entity:set_clip() wins over the clip picked from velocity/flags.
			const std::string_view wanted = tracked.forced_clip
					? std::string_view(*tracked.forced_clip)
					: anim_clip_name(frame.clip);
			const double clip_time = tracked.forced_clip ? tracked.forced_clip_time : frame.clip_time;
			const EntityClipLayout &clip = resolve_clip(layout, wanted);
			if (clip.name != wanted) {
				std::string key = std::to_string(texture.id) + "/" + std::string(wanted);
				if (impl_->warned_clip_fallbacks.insert(std::move(key)).second) {
					VB_WARN("render", "entity kind ", static_cast<unsigned>(tracked.kind),
							": sheet has no '", wanted, "' clip, playing its first clip '",
							clip.name, "' instead");
				}
			}
			const int frame_in_clip = clip.frames > 0
					? static_cast<int>(clip_time * static_cast<double>(clip.fps)) % clip.frames
					: 0;
			const int column = clip.start_frame + frame_in_clip;
			const int row = frame.pose.pose_index;
			source = Rectangle{
				static_cast<float>(column * layout.frame_width),
				static_cast<float>(row * layout.frame_height),
				static_cast<float>(layout.frame_width),
				static_cast<float>(layout.frame_height),
			};
		} else {
			texture = impl_->placeholder;
			source = Rectangle{ 0.0f, 0.0f,
				static_cast<float>(impl_->placeholder.width),
				static_cast<float>(impl_->placeholder.height) };
		}

		Vector2 size{ tracked.width, tracked.height };
		if (frame.pose.mirrored) {
			size.x = -size.x; // DrawBillboardPro flips the source horizontally
		}
		// Anchor conversion: the spec's origin_x/origin_y are normalized,
		// image-space (0,0 = top-left, y grows downward, default {0.5, 1.0}
		// = bottom-centre feet point). raylib's `origin` for
		// DrawBillboardPro is a world-size offset measured from `position`
		// along the quad's own right/up axes (up = +Y), so origin.y=0 means
		// `position` sits at the quad's bottom edge -- the inverse sense of
		// image-space y. origin.x uses the same left-right sense in both
		// (the mirrored `size.x` flip above keeps a mirrored pose centred
		// symmetrically either way).
		const float origin_x = has_visual ? visual->layout.origin_x : 0.5f;
		const float origin_y = has_visual ? visual->layout.origin_y : 1.0f;
		const Vector2 origin{ size.x * origin_x, size.y * (1.0f - origin_y) };

		if (!has_visual || visual->layers.empty()) {
			DrawBillboardPro(camera, texture, source, feet,
					Vector3{ 0.0f, 1.0f, 0.0f }, size, origin, 0.0f,
					has_visual ? WHITE : tint_for_entity(id));
			continue;
		}
		// Paper-doll layers: every sheet is drawn with the base frame's exact
		// source rectangle, size and origin, so clothes can't drift from the
		// body. Same depth, so GL_LEQUAL lets each later sheet win in order.
		for (const int idx : layer_draw_order(visual->layers, frame.pose.pose_index)) {
			const bool base = idx < 0;
			const auto i = static_cast<std::size_t>(base ? 0 : idx);
			DrawBillboardPro(camera, base ? texture : visual->layer_textures[i], source, feet,
					Vector3{ 0.0f, 1.0f, 0.0f }, size, origin, 0.0f,
					base ? WHITE : Color{ visual->layers[i].tint[0], visual->layers[i].tint[1], visual->layers[i].tint[2], visual->layers[i].tint[3] });
		}
	}
	rlDrawRenderBatchActive();
	if (!depth_test) {
		rlEnableDepthTest();
	}
	EndShaderMode();
}

namespace {

// Labels use the engine's UI font (raylib's built-in bitmap font, the one
// raygui draws with) at its native pixel size: point-sampled, so it stays
// crisp at any scale. One text line is kLabelFontPx "pixels" tall and is
// mapped to EntityText::size metres.
constexpr float kLabelFontPx = 10.0f;
constexpr float kLabelSpacingPx = 1.0f;
constexpr float kLabelLineGapPx = 2.0f;
constexpr float kLabelPaddingPx = 2.0f;

Color to_color(const std::array<std::uint8_t, 4> &c) {
	return Color{ c[0], c[1], c[2], c[3] };
}

std::vector<std::string> split_lines(const std::string &value) {
	std::vector<std::string> lines;
	std::size_t start = 0;
	while (true) {
		const std::size_t nl = value.find('\n', start);
		lines.push_back(value.substr(start, nl == std::string::npos ? nl : nl - start));
		if (nl == std::string::npos) {
			break;
		}
		start = nl + 1;
	}
	return lines;
}

// Draws one label as a camera-facing quad whose bottom centre sits at
// `anchor`. The text is laid out in 2D pixel space (y down) and mapped onto
// the camera's right/up axes by a model matrix, so raylib's ordinary 2D text
// and rectangle calls draw it in world space.
void draw_label(const protocol::EntityText &text, Vector3 anchor,
		Vector3 right, Vector3 up) {
	const Font font = GetFontDefault();
	const std::vector<std::string> lines = split_lines(text.value);
	std::vector<float> widths;
	widths.reserve(lines.size());
	float max_width = 0.0f;
	for (const std::string &line : lines) {
		const float w = MeasureTextEx(font, line.c_str(), kLabelFontPx, kLabelSpacingPx).x;
		widths.push_back(w);
		max_width = std::max(max_width, w);
	}
	const float n = static_cast<float>(lines.size());
	const float panel_w = max_width + 2.0f * kLabelPaddingPx;
	const float panel_h = n * kLabelFontPx + (n - 1.0f) * kLabelLineGapPx +
			2.0f * kLabelPaddingPx;
	const float scale = text.size / kLabelFontPx; // metres per pixel

	// Pixel (x, y) -> anchor + right*(x - panel_w/2)*scale + up*(panel_h - y)*scale.
	const Vector3 origin = Vector3Add(anchor,
			Vector3Add(Vector3Scale(right, -0.5f * panel_w * scale),
					Vector3Scale(up, panel_h * scale)));
	const Vector3 forward = Vector3CrossProduct(right, up);
	const Matrix model{
		right.x * scale, -up.x * scale, forward.x, origin.x,
		right.y * scale, -up.y * scale, forward.y, origin.y,
		right.z * scale, -up.z * scale, forward.z, origin.z,
		0.0f, 0.0f, 0.0f, 1.0f
	};

	rlPushMatrix();
	rlMultMatrixf(MatrixToFloat(model));
	if (text.background) {
		DrawRectangleRounded(Rectangle{ 0.0f, 0.0f, panel_w, panel_h }, 0.35f, 4,
				to_color(*text.background));
	}
	const Color color = to_color(text.color);
	const Color outline{ 0, 0, 0, static_cast<unsigned char>(color.a * 3 / 4) };
	for (std::size_t i = 0; i < lines.size(); ++i) {
		const Vector2 at{ kLabelPaddingPx + 0.5f * (max_width - widths[i]),
			kLabelPaddingPx + static_cast<float>(i) * (kLabelFontPx + kLabelLineGapPx) };
		if (!text.background) {
			// No panel: a one-pixel dark outline keeps it legible on any terrain.
			static constexpr Vector2 kOffsets[] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } };
			for (const Vector2 o : kOffsets) {
				DrawTextEx(font, lines[i].c_str(), Vector2{ at.x + o.x, at.y + o.y },
						kLabelFontPx, kLabelSpacingPx, outline);
			}
		}
		DrawTextEx(font, lines[i].c_str(), at, kLabelFontPx, kLabelSpacingPx, color);
	}
	rlPopMatrix();
}

} // namespace

void EntityRenderer::draw_labels(const CameraView &camera_view) const {
	const Vector3 eye{ static_cast<float>(camera_view.position.x),
		static_cast<float>(camera_view.position.y),
		static_cast<float>(camera_view.position.z) };
	struct Pending {
		const protocol::EntityText *text;
		Vector3 anchor;
		float dist_sq;
	};
	std::vector<Pending> pending;
	for (const auto &[id, tracked] : impl_->states) {
		(void)id;
		if (!tracked.text || tracked.text->value.empty()) {
			continue;
		}
		const core::Vec3d pos = tracked.render_pos;
		const Vector3 anchor{ static_cast<float>(pos.x),
			static_cast<float>(pos.y) + tracked.text->offset_y,
			static_cast<float>(pos.z) };
		const float dist_sq = Vector3DistanceSqr(anchor, eye);
		const float max = tracked.text->max_distance;
		if (max > 0.0f && dist_sq > max * max) {
			continue;
		}
		pending.push_back({ &*tracked.text, anchor, dist_sq });
	}
	if (pending.empty()) {
		return;
	}
	// Depth-tested labels first, then through-walls ones; far to near within
	// each group so overlapping translucent panels blend correctly.
	std::sort(pending.begin(), pending.end(), [](const Pending &a, const Pending &b) {
		if (a.text->through_walls != b.text->through_walls) {
			return !a.text->through_walls;
		}
		return a.dist_sq > b.dist_sq;
	});

	// Billboard axes from the view matrix, as DrawBillboardPro does.
	const Matrix view = GetCameraMatrix(to_raylib_camera(camera_view));
	const Vector3 right{ view.m0, view.m4, view.m8 };
	const Vector3 up{ view.m1, view.m5, view.m9 };

	rlDrawRenderBatchActive();
	rlDisableDepthMask();
	rlDisableBackfaceCulling(); // the y-flipped text quads wind backwards
	bool depth_test = true;
	for (const Pending &p : pending) {
		if (p.text->through_walls && depth_test) {
			rlDrawRenderBatchActive();
			rlDisableDepthTest();
			depth_test = false;
		}
		draw_label(*p.text, p.anchor, right, up);
	}
	rlDrawRenderBatchActive();
	rlEnableDepthTest();
	rlEnableBackfaceCulling();
	rlEnableDepthMask();
}

std::size_t EntityRenderer::tracked_count() const {
	return impl_->states.size();
}

} // namespace vb::render
