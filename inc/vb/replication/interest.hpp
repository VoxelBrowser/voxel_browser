#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <unordered_map>
#include <vector>

#include "vb/core/ids.hpp"
#include "vb/core/math.hpp"

// Interest management (spec §8.4, docs/replication.md).
//
// Without VB_WITH_REPLICATION: a hand-rolled linear scan over a coarse grid.
// With VB_WITH_REPLICATION: `visible_from` is backed by librg's chunk-radius
// query (§18 Q3) -- librg owns entity tracking + the culling query, we keep
// our own EntityState payload and diff/snapshot code untouched either way.
// Same public interface, same diff semantics, in both configurations.

namespace vb::replication {

struct EntityState {
	core::NetId net_id = core::NetId::kInvalid;
	core::EntityKindId kind = core::EntityKindId::kInvalid;
	core::Vec3d pos{};
	core::Vec2f rot{}; // yaw, pitch
	core::Vec3f vel{};
	// Replicated as EntityRecord::flags (bit 0 = on_ground) so the client can
	// pick idle/walk/run instead of jump/fall. Players copy their collider;
	// anything without physics (script entities, item drops) counts as
	// grounded.
	std::uint8_t flags = 0;

	bool operator==(const EntityState &) const = default;
};

class InterestGrid {
public:
	explicit InterestGrid(double cell_size = 32.0);
	~InterestGrid();

	InterestGrid(const InterestGrid &) = delete;
	InterestGrid &operator=(const InterestGrid &) = delete;
	InterestGrid(InterestGrid &&) = delete;
	InterestGrid &operator=(InterestGrid &&) = delete;

	void upsert(const EntityState &state);
	void remove(core::NetId id);
	void clear();
	std::size_t size() const { return entities_.size(); }

	const EntityState *get(core::NetId id) const {
		const auto it = entities_.find(id);
		return it == entities_.end() ? nullptr : &it->second;
	}

	// Net ids visible to `self` within `radius_cells` of its interest cell,
	// excluding `self`. Sorted ascending for stable diffs.
	//
	// `eye` is only consulted by the non-librg backend (a plain distance
	// check against every tracked entity); the librg backend instead uses
	// the cell `self` was placed at by its most recent upsert(), so callers
	// must upsert(self, ...) before querying its own visibility -- true of
	// every call site today (ServerSession always upserts a player on join
	// before the first broadcast tick).
	std::vector<core::NetId> visible_from(
			core::Vec3d eye, int radius_cells, core::NetId self) const;

private:
	std::int64_t cell_of(double v) const {
		return static_cast<std::int64_t>(std::floor(v / cell_size_));
	}

	double cell_size_;
	std::unordered_map<core::NetId, EntityState> entities_;
#if defined(VB_WITH_REPLICATION)
	// Opaque `librg_world*`; kept as void* so this header never has to
	// include librg.h (a vendored C99 single-header) -- only interest.cpp
	// does. Mutable: librg_world_query() only mutates transient scratch
	// state internal to librg, not anything entities_ already reflects.
	mutable void *librg_world_ = nullptr;
#endif
};

struct InterestDiff {
	std::vector<core::NetId> entered; // in cur, not in prev
	std::vector<core::NetId> stayed; // in both
	std::vector<core::NetId> left; // in prev, not in cur
};

// Both inputs must be sorted ascending (visible_from() returns them that way).
inline InterestDiff diff_interest(const std::vector<core::NetId> &prev,
		const std::vector<core::NetId> &cur) {
	InterestDiff d;
	std::set_difference(cur.begin(), cur.end(), prev.begin(), prev.end(),
			std::back_inserter(d.entered));
	std::set_intersection(cur.begin(), cur.end(), prev.begin(), prev.end(),
			std::back_inserter(d.stayed));
	std::set_difference(prev.begin(), prev.end(), cur.begin(), cur.end(),
			std::back_inserter(d.left));
	return d;
}

} // namespace vb::replication
