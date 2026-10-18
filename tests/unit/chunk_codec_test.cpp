#include <doctest/doctest.h>

#include <ostream>

#include <random>
#include <span>
#include <vector>

#include "vb/protocol/message.hpp"
#include "vb/protocol/world.hpp"
#include "vb/world/block.hpp"
#include "vb/world/chunk.hpp"
#include "vb/world/chunk_codec.hpp"
#include "vb/world/lighting.hpp"
#include "vb/world/paletted_chunk_store.hpp"

using namespace vb::world;
using vb::core::BlockId;
namespace proto = vb::protocol;

namespace {
std::span<const std::byte> sp(const std::vector<std::byte> &v) {
	return { v.data(), v.size() };
}
} // namespace

TEST_CASE("rle round-trips arbitrary bytes") {
	std::vector<std::byte> in;
	std::mt19937 rng(7);
	// Run-structured data, like a light volume or terrain slab.
	while (in.size() < 5000) {
		const auto value = static_cast<std::byte>(rng() % 6);
		const std::size_t run = 1 + (rng() % 200);
		for (std::size_t k = 0; k < run && in.size() < 5000; ++k) {
			in.push_back(value);
		}
	}
	const auto enc = rle_encode(sp(in));
	auto dec = rle_decode(sp(enc), in.size());
	REQUIRE(dec);
	CHECK(*dec == in);
	CHECK(enc.size() < in.size()); // compresses run-structured input

	// Random data round-trips too (may inflate, that's fine).
	std::vector<std::byte> noise;
	for (int i = 0; i < 300; ++i) {
		noise.push_back(static_cast<std::byte>(rng() % 256));
	}
	auto n2 = rle_decode(sp(rle_encode(sp(noise))), noise.size());
	REQUIRE(n2);
	CHECK(*n2 == noise);
}

TEST_CASE("rle_decode rejects a size mismatch") {
	std::vector<std::byte> in(100, std::byte{ 5 });
	const auto enc = rle_encode(sp(in));
	CHECK_FALSE(rle_decode(sp(enc), 99));
	CHECK_FALSE(rle_decode(sp(enc), 101));
}

TEST_CASE("rle_decode rejects a run length that would wrap the size check") {
	// varint 2^64 - 1 (ten bytes), then the value byte. `out.size() + run` used to wrap to a
	// small number and then ask std::vector for 2^64 bytes (found by fuzz_protocol).
	std::vector<std::byte> in = { std::byte{ 0x02 }, std::byte{ 0x07 } };
	for (int i = 0; i < 9; ++i) {
		in.push_back(std::byte{ 0xff });
	}
	in.push_back(std::byte{ 0x01 });
	in.push_back(std::byte{ 0x07 });
	CHECK_FALSE(rle_decode(sp(in), 16));
}

TEST_CASE("chunk payload round-trips blocks and light") {
	auto reg = BlockRegistry::base();
	Chunk src({ 2, -1, 3 });
	// Some structure: a stone floor, a dirt pillar, a lamp.
	for (int z = 0; z < kChunkDim; ++z) {
		for (int x = 0; x < kChunkDim; ++x) {
			src.blocks().set(x, 0, z, base_block::stone);
		}
	}
	for (int y = 1; y < 8; ++y) {
		src.blocks().set(4, y, 4, base_block::dirt);
	}
	src.blocks().set(10, 5, 10, base_block::grass);
	LightEngine(reg).relight_chunk(src);

	const auto payload = encode_chunk_payload(src);
	CHECK(payload.size() < kChunkVolume); // far smaller than raw

	Chunk dst({ 2, -1, 3 });
	REQUIRE(decode_chunk_payload(sp(payload), dst));

	for (std::size_t i = 0; i < kChunkVolume; ++i) {
		REQUIRE(dst.blocks().get(i) == src.blocks().get(i));
		REQUIRE(dst.light_volume()[i].packed == src.light_volume()[i].packed);
	}
}

TEST_CASE("homogeneous chunk payload is tiny and round-trips") {
	Chunk src({ 0, 0, 0 });
	src.blocks().fill(base_block::stone);
	const auto payload = encode_chunk_payload(src);
	CHECK(payload.size() < 40);

	Chunk dst;
	REQUIRE(decode_chunk_payload(sp(payload), dst));
	CHECK(dst.blocks().get(12345) == base_block::stone);
}

TEST_CASE("S2C_ChunkAdd / Delta / Remove round-trip") {
	proto::S2CChunkAdd add;
	add.coord = { 1, 2, 3 };
	add.revision = 99;
	add.payload = { std::byte{ 1 }, std::byte{ 2 }, std::byte{ 3 } };
	std::vector<std::byte> buf;
	add.encode(buf);
	auto add2 = proto::S2CChunkAdd::decode(sp(buf));
	REQUIRE(add2);
	CHECK(add2->coord == vb::core::ChunkCoord{ 1, 2, 3 });
	CHECK(add2->revision == 99);
	CHECK(add2->payload == add.payload);

	proto::S2CChunkDelta delta;
	delta.coord = { -1, 0, 5 };
	delta.base_revision = 3;
	delta.new_revision = 4;
	delta.blocks.push_back({ 42, base_block::wood });
	delta.blocks.push_back({ 9001, BlockId::kAir });
	delta.light.push_back({ 42, 0xF3 });
	buf.clear();
	delta.encode(buf);
	auto d2 = proto::S2CChunkDelta::decode(sp(buf));
	REQUIRE(d2);
	CHECK(d2->new_revision == 4);
	REQUIRE(d2->blocks.size() == 2);
	CHECK(d2->blocks[0] == proto::BlockChange{ 42, base_block::wood });
	REQUIRE(d2->light.size() == 1);
	CHECK(d2->light[0].packed == 0xF3);

	proto::S2CChunkRemove rem;
	rem.coord = { 7, 7, 7 };
	buf.clear();
	rem.encode(buf);
	auto r2 = proto::S2CChunkRemove::decode(sp(buf));
	REQUIRE(r2);
	CHECK(r2->coord == vb::core::ChunkCoord{ 7, 7, 7 });
}

TEST_CASE("chunk messages travel on the world lane") {
	CHECK(proto::lane_for(proto::MessageType::kS2CChunkAdd) ==
			proto::Lane::kWorld);
	CHECK(proto::lane_for(proto::MessageType::kS2CChunkRemove) ==
			proto::Lane::kWorld);
}
