#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string_view>

#include "vb/core/math.hpp"

// Client-side entity presentation logic (spec §11.3, ARCHITECTURE_SPEC.md):
// which directional pose and which animation clip to show for a billboarded
// entity, and the hysteresis that keeps the pose from flickering at a sector
// boundary. Pure math, no raylib — vb/render/entity_renderer.cpp draws with
// it. Header-only so it is unit-tested without linking the render library
// (same reasoning as vb/render/camera.hpp).

namespace vb::render {

// Client-resolved animation state. Priority order, highest first: kDead >
// kHurt > kActing > kJump/kFall > kRun > kWalk > kIdle (resolve_anim_clip()
// implements exactly this order).
enum class AnimClip {
	kIdle,
	kWalk,
	kRun,
	kJump,
	kFall,
	kActing,
	kHurt,
	kDead,
};

// Matches the S2C_EntitySnapshot EntityRecord.flags bit layout (spec
// §11.3). Bit 0 (on_ground) is sent for every entity since protocol 32:
// players copy their collider, and entities without physics (script
// entities, item drops) are always grounded, so a stationary one resolves to
// idle. The other bits are not sent by the server yet, so resolve_anim_clip()
// degrades gracefully to jump/fall/run/walk/idle; a pack forces any clip with
// entity:set_clip(name) instead.
enum EntityAnimFlag : std::uint8_t {
	kAnimOnGround = 1u << 0,
	kAnimDead = 1u << 1,
	kAnimHurtPulse = 1u << 2,
	kAnimActing = 1u << 3,
};

struct AnimThresholds {
	double walk_speed = 0.3; // m/s horizontal, above this counts as "moving"
	double run_speed = 5.5; // m/s horizontal, above this counts as sprinting
	double fall_speed = 0.5; // m/s downward, above this counts as "falling"
};

// Pick the animation clip from replicated state alone -- no extra round trip.
inline AnimClip resolve_anim_clip(core::Vec3f vel, std::uint8_t flags,
		AnimThresholds t = {}) {
	if ((flags & kAnimDead) != 0) {
		return AnimClip::kDead;
	}
	if ((flags & kAnimHurtPulse) != 0) {
		return AnimClip::kHurt;
	}
	if ((flags & kAnimActing) != 0) {
		return AnimClip::kActing;
	}
	const bool on_ground = (flags & kAnimOnGround) != 0;
	if (!on_ground) {
		return static_cast<double>(vel.y) < -t.fall_speed ? AnimClip::kFall
														  : AnimClip::kJump;
	}
	const double vx = static_cast<double>(vel.x);
	const double vz = static_cast<double>(vel.z);
	const double horiz = std::sqrt(vx * vx + vz * vz);
	if (horiz > t.run_speed) {
		return AnimClip::kRun;
	}
	if (horiz > t.walk_speed) {
		return AnimClip::kWalk;
	}
	return AnimClip::kIdle;
}

// Canonical clip name per AnimClip, matching architecture_spec/rendering.md
// §11.3's base clip set exactly -- bridges resolve_anim_clip()'s enum output
// to a pack's `visual.clips` list (protocol::EntityClipDef::clip), looked up
// by render::resolve_clip() (vb/render/entity_visual_layout.hpp).
inline std::string_view anim_clip_name(AnimClip clip) {
	switch (clip) {
		case AnimClip::kIdle:
			return "idle";
		case AnimClip::kWalk:
			return "walk";
		case AnimClip::kRun:
			return "run";
		case AnimClip::kJump:
			return "jump";
		case AnimClip::kFall:
			return "fall";
		case AnimClip::kActing:
			return "acting";
		case AnimClip::kHurt:
			return "hurt";
		case AnimClip::kDead:
			return "dead";
	}
	return "idle";
}

inline double normalize_angle_deg(double deg) {
	double d = std::fmod(deg, 360.0);
	if (d < 0.0) {
		d += 360.0;
	}
	return d;
}

// Bearing from `from` to `to` in the world XZ plane, degrees, matching the yaw
// convention used throughout vb_core (0 deg = -Z, 90 deg = +X — see
// render::FirstPersonController::forward() / physics::wish_dir_from_local()).
inline double bearing_degrees(core::Vec3d from, core::Vec3d to) {
	const double dx = to.x - from.x;
	const double dz = to.z - from.z;
	constexpr double kPi = 3.14159265358979323846;
	const double rad = std::atan2(dx, -dz); // 0 at -Z, +90 at +X
	return normalize_angle_deg(rad * 180.0 / kPi);
}

// Which of `facings` equal sectors the camera sits in, relative to the
// entity's own facing yaw. Sector 0 is centred on 0 deg (the entity facing
// directly at the viewer, i.e. the viewer sees the entity's front).
inline int direction_bucket(double bearing_to_camera_deg, double entity_yaw_deg,
		int facings) {
	facings = std::max(facings, 1);
	const double view_angle =
			normalize_angle_deg(bearing_to_camera_deg - entity_yaw_deg);
	const double sector = 360.0 / static_cast<double>(facings);
	int bucket =
			static_cast<int>(std::floor((view_angle + sector * 0.5) / sector));
	bucket %= facings;
	if (bucket < 0) {
		bucket += facings;
	}
	return bucket;
}

// Which unique authored pose to use for a direction bucket, and whether to
// mirror it. With `mirror = true` (default), packs author floor(facings/2)+1
// unique poses (front..back) and the engine mirrors the rest (e.g. facings=8
// -> 5 unique poses; facings=4 -> 3). With `mirror = false`, a pack authors
// one distinct row per facing (rows == facings, e.g. a real "left" pose
// instead of a horizontally-flipped "right" one) and nothing is ever
// mirrored -- `bucket` maps straight to `pose_index`.
struct PoseSelection {
	int pose_index = 0;
	bool mirrored = false;

	bool operator==(const PoseSelection &) const = default;
};

inline PoseSelection select_pose(int bucket, int facings, bool mirror = true) {
	facings = std::max(facings, 1);
	if (!mirror) {
		return { bucket, false };
	}
	const int half = facings / 2;
	if (bucket <= half) {
		return { bucket, false };
	}
	return { facings - bucket, true };
}

// Debounces direction_bucket() against flicker when the viewer sits near a
// sector boundary: an observed bucket must persist for `stable_frames`
// consecutive updates before it replaces the current one.
class DirectionBucketTracker {
public:
	explicit DirectionBucketTracker(int stable_frames = 3) : stable_frames_(std::max(stable_frames, 1)) {}

	int update(int observed_bucket) {
		if (observed_bucket == current_) {
			pending_ = current_;
			pending_count_ = 0;
			return current_;
		}
		if (observed_bucket == pending_) {
			++pending_count_;
		} else {
			pending_ = observed_bucket;
			pending_count_ = 1;
		}
		if (pending_count_ >= stable_frames_) {
			current_ = pending_;
			pending_count_ = 0;
		}
		return current_;
	}

	int current() const { return current_; }

private:
	int stable_frames_;
	int current_ = 0;
	int pending_ = 0;
	int pending_count_ = 0;
};

// Ties the above together into one per-entity render state: which clip, which
// pose, and how long we've been in the current clip (for future per-frame
// animation timing -- Phase 3's placeholder is a single frame, so this is
// plumbed through but not yet used to index into anything).
class EntityPresentationState {
public:
	struct Frame {
		AnimClip clip = AnimClip::kIdle;
		PoseSelection pose{};
		double clip_time = 0.0; // seconds into the current clip
	};

	explicit EntityPresentationState(int facings = 8, bool mirror = true, int stable_frames = 3) : facings_(facings), mirror_(mirror), bucket_tracker_(stable_frames) {}

	// `entity_yaw_deg` is the entity's own facing (Rotation.yaw); `camera_pos`
	// is the viewer's eye position. Advances clip_time by dt_seconds unless the
	// clip just changed.
	void update(core::Vec3d entity_pos, double entity_yaw_deg, core::Vec3f vel,
			std::uint8_t flags, core::Vec3d camera_pos, double dt_seconds,
			AnimThresholds thresholds = {}) {
		position_ = entity_pos;

		const AnimClip clip = resolve_anim_clip(vel, flags, thresholds);
		if (clip != frame_.clip) {
			frame_.clip_time = 0.0;
		} else {
			frame_.clip_time += dt_seconds;
		}
		frame_.clip = clip;

		const double bearing = bearing_degrees(entity_pos, camera_pos);
		const int raw_bucket = direction_bucket(bearing, entity_yaw_deg, facings_);
		const int stable_bucket = bucket_tracker_.update(raw_bucket);
		frame_.pose = select_pose(stable_bucket, facings_, mirror_);
	}

	const Frame &frame() const { return frame_; }
	core::Vec3d position() const { return position_; }

private:
	int facings_;
	bool mirror_;
	DirectionBucketTracker bucket_tracker_;
	Frame frame_{};
	core::Vec3d position_{};
};

} // namespace vb::render
