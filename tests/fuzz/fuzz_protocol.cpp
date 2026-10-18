// Fuzz target: every wire decoder in inc/vb/protocol/ (plus frame parsing,
// the chunk payload codec and LZ4). These parse bytes straight off the
// network from an untrusted peer, so they must never crash, and anything they
// accept must re-encode to bytes that decode to the same thing.
//
// Input layout: byte 0 picks the decoder, the rest is its payload.

#include "fuzz.hpp"

#include <span>
#include <tuple>
#include <type_traits>

#include "vb/protocol/assetsync.hpp"
#include "vb/protocol/chat.hpp"
#include "vb/protocol/compression.hpp"
#include "vb/protocol/handshake.hpp"
#include "vb/protocol/input.hpp"
#include "vb/protocol/inventory.hpp"
#include "vb/protocol/message.hpp"
#include "vb/protocol/snapshot.hpp"
#include "vb/protocol/world.hpp"
#include "vb/world/chunk_codec.hpp"

using namespace vb::protocol;

namespace {

using Messages = std::tuple<C2SHello, S2CServerInfo, C2SAuth, S2CAuthChallenge, S2CReauthRequest,
		C2SReauth, S2CAuthResult, C2SReady, S2CJoinAccept, S2CDisconnect,
		C2SAssetManifestRequest, S2CAssetManifest, C2SAssetRequest, S2CAssetData,
		S2CBlockRegistry, S2CChunkAdd, S2CChunkDelta, S2CChunkRemove, C2SBlockEdit,
		S2CBlockEditResult, S2CTimeOfDay, S2CKeybindRegistry, C2SBlockBreakBegin,
		C2SBlockBreakStop, S2CMoveParams, S2CDayNightCurve, S2CFogParams,
		S2CEntityKindRegistry, S2CBlockDamage, S2CEntitySnapshot, S2CEntityProps,
		C2SInputBatch, C2SChat, S2CChat, C2SUiEvent, S2COpenUi, S2CPlayerJoin,
		S2CPlayerLeave, S2CPlayerList, S2CInventory, S2CPlayerStatus>;

constexpr std::size_t kMessageCount = std::tuple_size_v<Messages>;
// Selectors past the message decoders.
constexpr std::size_t kFrameSelector = kMessageCount;
constexpr std::size_t kChunkSelector = kMessageCount + 1;
constexpr std::size_t kLz4Selector = kMessageCount + 2;
constexpr std::size_t kSelectorCount = kMessageCount + 3;

template <typename T, std::size_t I = 0>
constexpr std::size_t index_of() {
	if constexpr (std::is_same_v<T, std::tuple_element_t<I, Messages>>) {
		return I;
	} else {
		return index_of<T, I + 1>();
	}
}

template <typename T>
void round_trip(std::span<const std::byte> in) {
	auto first = T::decode(in);
	if (!first) {
		return;
	}
	std::vector<std::byte> once;
	first->encode(once);
	auto second = T::decode(once);
	VB_FUZZ_CHECK(second);
	std::vector<std::byte> twice;
	second->encode(twice);
	VB_FUZZ_CHECK(once == twice);
}

template <std::size_t I = 0>
void dispatch(std::size_t selector, std::span<const std::byte> in) {
	if constexpr (I < kMessageCount) {
		if (selector == I) {
			round_trip<std::tuple_element_t<I, Messages>>(in);
			return;
		}
		dispatch<I + 1>(selector, in);
	}
}

template <std::size_t I = 0>
void default_seeds(std::vector<std::vector<std::uint8_t>> &out) {
	if constexpr (I < kMessageCount) {
		std::vector<std::byte> payload;
		std::tuple_element_t<I, Messages>{}.encode(payload);
		std::vector<std::uint8_t> seed{ static_cast<std::uint8_t>(I) };
		for (std::byte b : payload) {
			seed.push_back(static_cast<std::uint8_t>(b));
		}
		out.push_back(std::move(seed));
		default_seeds<I + 1>(out);
	}
}

std::vector<std::uint8_t> with_selector(std::size_t selector, std::span<const std::byte> payload) {
	std::vector<std::uint8_t> seed{ static_cast<std::uint8_t>(selector) };
	for (std::byte b : payload) {
		seed.push_back(static_cast<std::uint8_t>(b));
	}
	return seed;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
	if (size == 0) {
		return 0;
	}
	const std::size_t selector = data[0] % kSelectorCount;
	const std::span<const std::byte> in(reinterpret_cast<const std::byte *>(data + 1), size - 1);
	if (selector < kMessageCount) {
		dispatch(selector, in);
	} else if (selector == kFrameSelector) {
		std::size_t offset = 0;
		while (offset < in.size()) {
			std::size_t consumed = 0;
			auto frame = read_frame(in.subspan(offset), consumed);
			if (!frame) {
				break;
			}
			VB_FUZZ_CHECK(consumed >= kEnvelopeBytes && consumed <= in.size() - offset);
			offset += consumed;
		}
	} else if (selector == kChunkSelector) {
		vb::world::Chunk chunk;
		if (vb::world::decode_chunk_payload(in, chunk)) {
			const auto again = vb::world::encode_chunk_payload(chunk);
			vb::world::Chunk copy;
			VB_FUZZ_CHECK(vb::world::decode_chunk_payload(again, copy));
		}
	} else if (selector == kLz4Selector) {
#if VB_WITH_COMPRESSION
		auto out = decompress_lz4(in);
		if (out) {
			auto back = decompress_lz4(compress_lz4(*out));
			VB_FUZZ_CHECK(back && *back == *out);
		}
#endif
	}
	return 0;
}

std::vector<std::vector<std::uint8_t>> vb_fuzz_seeds() {
	std::vector<std::vector<std::uint8_t>> seeds;
	default_seeds(seeds);

	// A few non-empty messages, so mutations start from lists and strings.
	{
		S2CChat chat;
		chat.text = "hello <b>world</b>";
		std::vector<std::byte> p;
		chat.encode(p);
		seeds.push_back(with_selector(index_of<S2CChat>(), p));
	}
	{
		std::vector<std::byte> frames;
		std::vector<std::byte> payload;
		C2SChat{ "hi" }.encode(payload);
		write_frame(frames, C2SChat::kType, payload);
		write_frame(frames, C2SReady::kType, {});
		seeds.push_back(with_selector(kFrameSelector, frames));
	}
	{
		vb::world::Chunk chunk;
		chunk.set(1, 2, 3, vb::core::BlockId{ 7 });
		chunk.set(15, 15, 15, vb::core::BlockId{ 300 });
		seeds.push_back(with_selector(kChunkSelector, vb::world::encode_chunk_payload(chunk)));
	}
#if VB_WITH_COMPRESSION
	{
		std::vector<std::byte> raw(512, std::byte{ 0x2a });
		raw[100] = std::byte{ 1 };
		seeds.push_back(with_selector(kLz4Selector, compress_lz4(raw)));
	}
#endif
	return seeds;
}
