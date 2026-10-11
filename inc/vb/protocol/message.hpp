#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "vb/core/error.hpp"
#include "vb/core/result.hpp"
#include "vb/protocol/byte_buffer.hpp"

// Message envelope (spec §14): { uint16 type, uint16 flags, uint32 payload_len }
// followed by payload_len bytes, all little-endian. One envelope per logical
// message; GNS lane selection is decided by the message type (see lane_for()).

namespace vb::protocol {

// Wire message identifiers. Values are frozen once shipped; append only.
enum class MessageType : std::uint16_t {
	kInvalid = 0,

	// --- handshake / control (lane 0) ---
	kC2SHello = 1,
	kS2CServerInfo = 2,
	kC2SAuth = 3,
	kS2CAuthResult = 4,
	kC2SReady = 5,
	kS2CJoinAccept = 6,
	kS2CDisconnect = 7,
	kS2CAuthChallenge = 8, // protocol v29, only when auth_mode == kExternal
	kS2CReauthRequest = 9, // protocol v29 wire format; used from Phase 9.6
	kC2SReauth = 10, // protocol v29 wire format; used from Phase 9.6

	// --- asset sync (lane 3) ---
	kC2SAssetManifestRequest = 20,
	kS2CAssetManifest = 21,
	kC2SAssetRequest = 22,
	kS2CAssetData = 23,

	// --- world (lane 1) ---
	kS2CBlockRegistry = 40,
	kS2CChunkAdd = 41,
	kS2CChunkDelta = 42,
	kS2CChunkRemove = 43,
	kC2SBlockEdit = 44,
	kS2CBlockEditResult = 45,
	kS2CTimeOfDay = 46,
	kS2CKeybindRegistry = 47,
	kC2SBlockBreakBegin = 48,
	kC2SBlockBreakStop = 49,
	kS2CMoveParams = 50,
	kS2CDayNightCurve = 51,
	kS2CFogParams = 52,
	kS2CEntityKindRegistry = 53,
	kS2CBlockDamage = 54,

	// --- snapshot (lane 2) ---
	kS2CEntitySnapshot = 60,
	kS2CEntityProps = 61, // protocol v32, lane kFeedback (reliable)

	// --- input (lane 4) ---
	kC2SInputBatch = 80,

	// --- chat / rpc (lane 0) ---
	kC2SChat = 100,
	kS2CChat = 101,
	kC2SUiEvent = 102,
	kS2COpenUi = 103,
	kS2CPlayerJoin = 104,
	kS2CPlayerLeave = 105,
	kS2CPlayerList = 106,
	kS2CInventory = 107,
	kS2CPlayerStatus = 108,
};

enum class Lane : std::uint8_t {
	kControl = 0,
	kWorld = 1,
	kSnapshot = 2,
	kAssets = 3,
	kInput = 4,
	// Small, latency-sensitive world updates (block damage) that must not
	// queue behind bulk `kWorld` chunk data. Reliable ordered within itself,
	// but *not* ordered against kWorld: receivers reconcile by revision.
	kFeedback = 5,
};

// Lane a message type travels on (spec §8.2). Unknown -> control.
constexpr Lane lane_for(MessageType type) {
	switch (type) {
		case MessageType::kS2CChunkAdd:
		case MessageType::kS2CChunkDelta:
		case MessageType::kS2CChunkRemove:
		case MessageType::kC2SBlockEdit:
		case MessageType::kS2CBlockEditResult:
		case MessageType::kS2CBlockRegistry:
		case MessageType::kS2CTimeOfDay:
		case MessageType::kS2CKeybindRegistry:
		case MessageType::kC2SBlockBreakBegin:
		case MessageType::kC2SBlockBreakStop:
		case MessageType::kS2CMoveParams:
		case MessageType::kS2CDayNightCurve:
		case MessageType::kS2CFogParams:
		case MessageType::kS2CEntityKindRegistry:
			return Lane::kWorld;
		case MessageType::kS2CBlockDamage:
		case MessageType::kS2CEntityProps:
			return Lane::kFeedback;
		case MessageType::kS2CEntitySnapshot:
			return Lane::kSnapshot;
		case MessageType::kC2SAssetManifestRequest:
		case MessageType::kS2CAssetManifest:
		case MessageType::kC2SAssetRequest:
		case MessageType::kS2CAssetData:
			return Lane::kAssets;
		case MessageType::kC2SInputBatch:
			return Lane::kInput;
		default:
			return Lane::kControl;
	}
}

enum class MessageFlag : std::uint16_t {
	kNone = 0,
	kCompressed = 1 << 0, // payload is LZ4-compressed
};

inline constexpr std::size_t kEnvelopeBytes = 8;
// Hard cap on a single message payload; larger declared lengths are rejected.
inline constexpr std::uint32_t kMaxPayloadBytes = 16u * 1024u * 1024u;

struct MessageHeader {
	MessageType type = MessageType::kInvalid;
	std::uint16_t flags = 0;
	std::uint32_t payload_len = 0;
};

struct Frame {
	MessageHeader header;
	std::span<const std::byte> payload;
};

// Append a complete envelope + payload to `out`.
inline void write_frame(std::vector<std::byte> &out, MessageType type,
		std::span<const std::byte> payload, std::uint16_t flags = 0) {
	ByteWriter w(out);
	w.u16(static_cast<std::uint16_t>(type));
	w.u16(flags);
	w.u32(static_cast<std::uint32_t>(payload.size()));
	w.bytes(payload);
}

// Parse one frame from the front of `buffer`. On success, `consumed` is set to
// the total envelope+payload size so the caller can advance its stream cursor.
inline core::Result<Frame, core::ProtocolError> read_frame(
		std::span<const std::byte> buffer, std::size_t &consumed) {
	consumed = 0;
	if (buffer.size() < kEnvelopeBytes) {
		return core::Err{ core::ProtocolError::kShortBuffer };
	}
	ByteReader r(buffer);
	const auto type = static_cast<MessageType>(r.u16());
	const std::uint16_t flags = r.u16();
	const std::uint32_t len = r.u32();
	if (len > kMaxPayloadBytes) {
		return core::Err{ core::ProtocolError::kLengthExceeded };
	}
	if (buffer.size() < kEnvelopeBytes + len) {
		return core::Err{ core::ProtocolError::kShortBuffer };
	}
	consumed = kEnvelopeBytes + len;
	return Frame{ MessageHeader{ type, flags, len },
		buffer.subspan(kEnvelopeBytes, len) };
}

} // namespace vb::protocol
