#include <doctest/doctest.h>

#include "vb/render/entity_visual_layout.hpp"

using namespace vb::render;
using vb::protocol::EntityClipDef;
using vb::protocol::EntityVisualDef;
using vb::protocol::EntityVisualOverride;

namespace {

EntityVisualDef make_def() {
	EntityVisualDef def;
	def.texture = "textures/entities/slime.png";
	def.frame_width = 128;
	def.frame_height = 128;
	def.facings = 8;
	def.origin_x = 0.5f;
	def.origin_y = 1.0f;
	def.clips.push_back({ "idle", 4, 6.0f });
	def.clips.push_back({ "walk", 6, 10.0f });
	return def;
}

} // namespace

TEST_CASE("build_entity_visual_layout resolves running column offsets and row count") {
	const EntityVisualDef def = make_def();
	// facings=8 -> rows = 8/2+1 = 5; total frames = 4+6=10 columns.
	const auto layout = build_entity_visual_layout(def, 128 * 10, 128 * 5);
	REQUIRE(layout.has_value());
	CHECK(layout->frame_width == 128);
	CHECK(layout->frame_height == 128);
	CHECK(layout->rows == 5);
	REQUIRE(layout->clips.size() == 2);
	CHECK(layout->clips[0].name == "idle");
	CHECK(layout->clips[0].start_frame == 0);
	CHECK(layout->clips[1].name == "walk");
	CHECK(layout->clips[1].start_frame == 4); // running sum past "idle"'s 4 frames
}

TEST_CASE("build_entity_visual_layout resolves facings=4 to 3 rows") {
	EntityVisualDef def = make_def();
	def.facings = 4;
	const auto layout = build_entity_visual_layout(def, 128 * 10, 128 * 3);
	REQUIRE(layout.has_value());
	CHECK(layout->rows == 3);
	CHECK(layout->mirror == true);
}

TEST_CASE("build_entity_visual_layout with mirror=false requires a full row per facing") {
	EntityVisualDef def = make_def();
	def.facings = 4;
	def.mirror = false;
	// The old mirror=true sheet size (3 rows) is now the wrong height...
	CHECK_FALSE(build_entity_visual_layout(def, 128 * 10, 128 * 3).has_value());
	// ...4 rows (one per facing, none mirrored) is required instead.
	const auto layout = build_entity_visual_layout(def, 128 * 10, 128 * 4);
	REQUIRE(layout.has_value());
	CHECK(layout->rows == 4);
	CHECK(layout->mirror == false);
}

TEST_CASE("build_entity_visual_layout rejects a width mismatch") {
	const EntityVisualDef def = make_def();
	CHECK_FALSE(build_entity_visual_layout(def, 128 * 9, 128 * 5).has_value());
}

TEST_CASE("build_entity_visual_layout rejects a height mismatch") {
	const EntityVisualDef def = make_def();
	CHECK_FALSE(build_entity_visual_layout(def, 128 * 10, 128 * 4).has_value());
}

TEST_CASE("build_entity_visual_layout rejects an empty clip list") {
	EntityVisualDef def = make_def();
	def.clips.clear();
	CHECK_FALSE(build_entity_visual_layout(def, 128 * 10, 128 * 5).has_value());
}

TEST_CASE("resolve_clip finds an exact name match") {
	const EntityVisualDef def = make_def();
	const auto layout = build_entity_visual_layout(def, 128 * 10, 128 * 5);
	REQUIRE(layout.has_value());
	CHECK(resolve_clip(*layout, "walk").start_frame == 4);
}

TEST_CASE("resolve_clip falls back to the first declared clip when the name is absent") {
	const EntityVisualDef def = make_def();
	const auto layout = build_entity_visual_layout(def, 128 * 10, 128 * 5);
	REQUIRE(layout.has_value());
	// def has no "dead" clip -- falls back to "idle" (first declared).
	CHECK(resolve_clip(*layout, "dead").name == "idle");
}

TEST_CASE("merge_visual_override with every field unset returns the kind default unchanged") {
	const EntityVisualDef def = make_def();
	const EntityVisualOverride empty;
	CHECK(merge_visual_override(def, empty) == def);
}

TEST_CASE("merge_visual_override overrides only texture, e.g. a player skin") {
	const EntityVisualDef def = make_def();
	EntityVisualOverride ov;
	ov.texture = "textures/entities/skins/player_red.png";

	const EntityVisualDef merged = merge_visual_override(def, ov);
	CHECK(merged.texture == "textures/entities/skins/player_red.png");
	CHECK(merged.frame_width == def.frame_width);
	CHECK(merged.frame_height == def.frame_height);
	CHECK(merged.facings == def.facings);
	CHECK(merged.origin_x == def.origin_x);
	CHECK(merged.origin_y == def.origin_y);
	CHECK(merged.clips == def.clips);
}

TEST_CASE("merge_visual_override replaces frame size/origin/facings/clips wholesale when given") {
	const EntityVisualDef def = make_def();
	EntityVisualOverride ov;
	ov.frame_width = 256;
	ov.frame_height = 256;
	ov.facings = 4;
	ov.origin_x = 0.5f;
	ov.origin_y = 0.5f;
	ov.clips = std::vector<EntityClipDef>{ { "only", 1, 1.0f } };

	const EntityVisualDef merged = merge_visual_override(def, ov);
	CHECK(merged.texture == def.texture); // never overridden -- inherited
	CHECK(merged.frame_width == 256);
	CHECK(merged.frame_height == 256);
	CHECK(merged.facings == 4);
	CHECK(merged.origin_y == doctest::Approx(0.5f));
	CHECK(merged.mirror == def.mirror); // not given -- inherited
	REQUIRE(merged.clips.size() == 1);
	CHECK(merged.clips[0].clip == "only");
}

TEST_CASE("merge_visual_override overrides mirror independently of everything else") {
	const EntityVisualDef def = make_def(); // mirror defaults true
	EntityVisualOverride ov;
	ov.mirror = false;

	const EntityVisualDef merged = merge_visual_override(def, ov);
	CHECK(merged.mirror == false);
	CHECK(merged.facings == def.facings); // untouched
	CHECK(merged.texture == def.texture); // untouched
}

TEST_CASE("paper-doll layers: row masks and draw order") {
	using vb::protocol::EntityVisualLayer;
	CHECK(layer_applies_to_row(0, 0));
	CHECK(layer_applies_to_row(0, 4));
	CHECK(layer_applies_to_row(0b0000'0001, 0));
	CHECK_FALSE(layer_applies_to_row(0b0000'0001, 2));
	CHECK(layer_applies_to_row(0b0000'0100, 2));

	// One cape sheet listed twice: behind the body on the front row (0),
	// over it on the back row (2).
	EntityVisualLayer cape_front{ "cape.png", true, 0b0000'0001, { 255, 255, 255, 255 } };
	EntityVisualLayer shirt{ "shirt.png", false, 0, { 255, 255, 255, 255 } };
	EntityVisualLayer cape_back{ "cape.png", false, 0b0000'0100, { 255, 255, 255, 255 } };
	const std::vector<EntityVisualLayer> layers{ cape_front, shirt, cape_back };
	CHECK(layer_draw_order(layers, 0) == std::vector<int>{ 0, -1, 1 });
	CHECK(layer_draw_order(layers, 2) == std::vector<int>{ -1, 1, 2 });
	CHECK(layer_draw_order(layers, 1) == std::vector<int>{ -1, 1 });
	CHECK(layer_draw_order({}, 3) == std::vector<int>{ -1 });

	CHECK(layer_sheet_matches(512, 320, 512, 320));
	CHECK_FALSE(layer_sheet_matches(512, 320, 256, 320));
}

TEST_CASE("merge_visual_override replaces the kind's layer list only when given") {
	using vb::protocol::EntityVisualLayer;
	EntityVisualDef base = make_def();
	base.layers = { EntityVisualLayer{ "kind_hat.png" } };
	EntityVisualOverride none;
	CHECK(merge_visual_override(base, none).layers == base.layers);
	EntityVisualOverride over;
	over.layers = std::vector<EntityVisualLayer>{ EntityVisualLayer{ "shirt.png" }, EntityVisualLayer{ "cape.png", true } };
	CHECK(merge_visual_override(base, over).layers == *over.layers);
	over.layers = std::vector<EntityVisualLayer>{};
	CHECK(merge_visual_override(base, over).layers.empty()); // explicit "no layers"
}
