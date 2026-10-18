#include "vb/world/chunk_codec.hpp"

#include "vb/protocol/byte_buffer.hpp"
#include "vb/world/paletted_chunk_store.hpp"

namespace vb::world {

using core::Err;
using core::ProtocolError;
using protocol::ByteReader;
using protocol::ByteWriter;

std::vector<std::byte> rle_encode(std::span<const std::byte> in) {
	std::vector<std::byte> out;
	ByteWriter w(out);
	std::size_t i = 0;
	while (i < in.size()) {
		const std::byte value = in[i];
		std::size_t run = 1;
		while (i + run < in.size() && in[i + run] == value) {
			++run;
		}
		w.varint(run);
		w.u8(static_cast<std::uint8_t>(value));
		i += run;
	}
	return out;
}

core::Result<std::vector<std::byte>, ProtocolError> rle_decode(
		std::span<const std::byte> in, std::size_t expected_size) {
	std::vector<std::byte> out;
	out.reserve(expected_size);
	ByteReader r(in);
	while (!r.at_end() && !r.failed()) {
		const std::uint64_t run = r.varint();
		const std::uint8_t value = r.u8();
		if (r.failed()) {
			break;
		}
		// Not `out.size() + run > expected_size`: a run near 2^64 wraps that sum (found by fuzz_protocol).
		if (run > expected_size - out.size()) {
			return Err{ ProtocolError::kLengthExceeded };
		}
		out.insert(out.end(), static_cast<std::size_t>(run),
				static_cast<std::byte>(value));
	}
	if (r.failed()) {
		return Err{ r.error() };
	}
	if (out.size() != expected_size) {
		return Err{ ProtocolError::kMalformed };
	}
	return out;
}

namespace {

// palette index -> lookup helper
std::size_t index_in(const std::vector<core::BlockId> &palette,
		core::BlockId block) {
	for (std::size_t i = 0; i < palette.size(); ++i) {
		if (palette[i] == block) {
			return i;
		}
	}
	return 0;
}

} // namespace

std::vector<std::byte> encode_chunk_payload(const Chunk &chunk) {
	const PalettedChunkStore &store = chunk.blocks();
	const std::vector<core::BlockId> &palette = store.palette();

	std::vector<std::byte> out;
	ByteWriter w(out);

	// palette
	w.varint(palette.size());
	for (core::BlockId id : palette) {
		w.u16(static_cast<std::uint16_t>(id));
	}

	// blocks: RLE of (run_length, palette_index) over the linear voxel order.
	std::size_t i = 0;
	while (i < kChunkVolume) {
		const std::size_t idx = index_in(palette, store.get(i));
		std::size_t run = 1;
		while (i + run < kChunkVolume &&
				index_in(palette, store.get(i + run)) == idx) {
			++run;
		}
		w.varint(run);
		w.varint(idx);
		i += run;
	}

	// light: RLE of the packed light bytes.
	std::vector<std::byte> light_bytes(kChunkVolume);
	for (std::size_t j = 0; j < kChunkVolume; ++j) {
		light_bytes[j] = static_cast<std::byte>(chunk.light_volume()[j].packed);
	}
	const std::vector<std::byte> light_rle = rle_encode(light_bytes);
	w.varint(light_rle.size());
	w.bytes({ light_rle.data(), light_rle.size() });

	return out;
}

core::Result<void, ProtocolError> decode_chunk_payload(
		std::span<const std::byte> payload, Chunk &out) {
	ByteReader r(payload);

	const std::uint64_t palette_len = r.varint();
	if (r.failed() || palette_len == 0 || palette_len > 65536) {
		return Err{ ProtocolError::kMalformed };
	}
	std::vector<core::BlockId> palette;
	palette.reserve(static_cast<std::size_t>(palette_len));
	for (std::uint64_t i = 0; i < palette_len; ++i) {
		palette.push_back(static_cast<core::BlockId>(r.u16()));
	}
	if (r.failed()) {
		return Err{ r.error() };
	}

	PalettedChunkStore &store = out.blocks();
	store.fill(palette[0]);

	std::size_t written = 0;
	while (written < kChunkVolume && !r.failed()) {
		const std::uint64_t run = r.varint();
		const std::uint64_t idx = r.varint();
		if (r.failed()) {
			break;
		}
		if (idx >= palette.size() || run > kChunkVolume - written) { // no wrapping sum
			return Err{ ProtocolError::kMalformed };
		}
		if (idx != 0) {
			for (std::uint64_t k = 0; k < run; ++k) {
				store.set(written + k, palette[static_cast<std::size_t>(idx)]);
			}
		}
		written += static_cast<std::size_t>(run);
	}
	if (r.failed()) {
		return Err{ r.error() };
	}
	if (written != kChunkVolume) {
		return Err{ ProtocolError::kMalformed };
	}

	const std::uint64_t light_len = r.varint();
	if (r.failed()) {
		return Err{ r.error() };
	}
	const auto light_rle = r.bytes(static_cast<std::size_t>(light_len));
	if (r.failed()) {
		return Err{ r.error() };
	}
	auto light = rle_decode(light_rle, kChunkVolume);
	if (!light) {
		return Err{ light.error() };
	}
	for (std::size_t j = 0; j < kChunkVolume; ++j) {
		out.light_volume()[j].packed =
				static_cast<std::uint8_t>((*light)[j]);
	}

	r.expect_consumed();
	if (r.failed()) {
		return Err{ r.error() };
	}
	return {};
}

} // namespace vb::world
