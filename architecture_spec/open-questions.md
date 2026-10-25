# Open Questions — Full Resolution History

> Full detail for this topic; linked from `ARCHITECTURE_SPEC.md`. Ground truth — do not duplicate here.

## 19. Open Questions

1. **Binding layer**: raw Lua C API vs. `sol2`. **Resolved (2026-09-10): sol2**
   (v3.3.0), for ergonomics; compile-time cost accepted.
2. **Cellulose API fit**: **Resolved (2026-09-11), then reversed
   (2026-09-16), and the dependency fully removed (2026-09-17) — hand-rolled
   meshing is the permanent backend.** Cellulose (a header-only greedy
   mesher) was spiked behind a build flag: it emitted vertex data, not GPU
   buffers, via a reusable `greedy_mesh(vector<
   MeshSample>, size, block_scale, ao, weld) -> ChunkMesh` entry point, fed
   from a flat grid filled from `ClientChunkStore`. It was wired in and
   passed the full test suite, but crashed in real play: greedy-merged
   output has far more volatile per-edit vertex/index counts than the
   hand-rolled per-face mesher (merging means one edit near a merge boundary
   can swing a whole face's quad count a lot), which defeated
   `chunk_renderer.cpp`'s GPU-buffer-headroom mitigation for the NVIDIA
   VAO/VBO-churn heap-corruption bug (see `STATE.md` §1/§8) and reproduced
   it far more often. Reverted, and Cellulose's build-flag/FetchContent
   scaffolding was later removed outright rather than kept dormant —
   `vb/world/
   chunk_mesher` + `chunk_mesh_snapshot`'s hand-rolled per-face mesher
   (fixed 4 vertices/6 indices per visible face, so an edit's effect on GPU
   buffer size stays small and local) is the permanent choice, with no
   planned swap-in. Full story: `STATE.md` §8's 15th entry (2026-09-16) and
   the 2026-09-17 removal entry.
3. **librg version / API**: **Resolved (2026-09-10): librg v7.4.0** (single
   self-contained header, zpl bundled). Interest is chunk-radius based (cells
   independent of voxel chunks). **Use librg for interest culling + its
   create/update/remove framing; keep our own per-entity payload codec and
   envelope/lane routing.** Not yet wired — Phase 1 ships a hand-rolled
   `InterestGrid` with the same diff semantics behind a narrow interface. Full
   write-up in `docs/replication.md`.
4. **Chunk compression**: LZ4 vs. zstd vs. palette-only. **Resolved
   (2026-09-28): LZ4, applied generically at the message-frame level (not
   chunk-specific plumbing), above a size threshold, only when it actually
   shrinks the payload.** `VB_WITH_COMPRESSION` already linked LZ4 (for
   asset-sync manifest hashing's xxHash dependency) but nothing had ever
   called into it for network payloads — `MessageFlag::kCompressed`
   (`inc/vb/protocol/message.hpp`) existed as pure unused scaffolding since
   Phase 2. New `vb::protocol::compress_lz4`/`decompress_lz4`
   (`inc/vb/protocol/compression.hpp`, `src/protocol/compression.cpp`, only
   compiled when `VB_WITH_COMPRESSION`): a u32 LE original-size prefix
   followed by one `LZ4_compress_default`/`LZ4_decompress_safe` block (LZ4's
   block API carries no length of its own, so the decompressor needs to be
   told the output size up front). Wired into the one shared
   `vb::net::frame_message()` helper (`inc/vb/net/handshake.hpp`) every
   message type already goes through to get framed — not a chunk-specific
   code path — behind `kCompressionThresholdBytes` (128 bytes) and an
   explicit "only if `compressed.size() < payload.size()`" check, so a
   payload that wouldn't actually shrink is sent uncompressed with the flag
   unset rather than paying LZ4's own fixed overhead for nothing. The
   receiving side (`decompress_frame_payload()`, an anonymous-namespace
   helper shared by both `ServerSession::tick()`'s and `ClientSession::
   tick()`'s `kMessage` handling in `src/net/session.cpp`) reverses this
   transparently right after `read_frame()`, before any message-specific
   `decode()` call — every existing decode call site needed zero changes.
   A frame arriving with the flag set on a binary built without
   `VB_WITH_COMPRESSION` is treated as a real protocol violation (dropped/
   disconnected with a clear reason), not silently misinterpreted, since the
   sender only ever sets the flag once it actually compressed something.
   **Measured, not guessed** (the item's own "start LZ4, measure"
   instruction), via a throwaway scratch test against real
   `WorldGenerator`-produced chunks (removed after measuring, per this
   project's convention of not committing debug scaffolding): a real surface
   chunk (smooth fBm heightmap, no caves) already RLE's its full 32768-byte
   raw volume down to ~12-14 bytes on its own — LZ4 on top of that actually
   comes out *larger* (17-19 bytes, all fixed per-call overhead: the 4-byte
   size prefix plus LZ4's own minimum block framing) since there is nothing
   left for it to find, which is exactly why `kCompressionThresholdBytes`
   exists rather than compressing unconditionally. The case LZ4 actually
   matters for is a heavily-edited, low-run-length chunk — a synthetic
   per-voxel-random "swiss cheese" chunk (4 block types, no long runs at
   all, the realistic shape of a well-played, heavily-mined area) measured
   at 50832 bytes RLE'd, LZ4'd down to 32393 bytes, a real ~36% further
   reduction. zstd and palette-only-with-no-generic-compression-at-all were
   not implemented or measured — LZ4 was already a zero-new-dependency,
   already-linked option, and the measured numbers gave no reason to reach
   for zstd's extra complexity/dependency cost. Verified: full `vb_tests`
   408/408 green (4 new cases: `protocol_test.cpp` covers `compress_lz4`/
   `decompress_lz4` directly — compressible/incompressible/empty round-trips
   plus rejecting a truncated buffer; `net_test.cpp` covers `frame_message`'s
   own threshold + "only if it helps" policy against a real `S2CChat`
   message, both the compressed-large-repetitive-payload case and the
   left-uncompressed-small-payload case), clean `-Werror` build of
   `vb_tests`/`voxel_browser`/`voxel_browser_server` (temporarily
   reconfigured `build-net-lua` with `-DVB_WARNINGS_AS_ERRORS=ON`, confirmed
   clean, reconfigured back to this dir's OFF default afterward).
5. **Persistence**: region file format for world save. **Resolved
   (2026-09-25, Phase 7.6, `REMAINING_TASKS.md`):** the 2026-09-17 "lean
   toward LMDB" direction note above was **reversed** after an explicit
   user decision (flat files over LMDB, to avoid a new `FetchContent`
   dependency with no upstream CMake) — landed as `vb::world::RegionStore`,
   an Anvil-style flat file per 16x16-chunk X/Z region (Y ungrouped), reusing
   the existing `vb/world/chunk_codec` palette+RLE serialization (the same
   format `S2C_ChunkAdd` already uses) as the on-disk chunk payload verbatim.
   **Region file LZ4 framing landed 2026-09-28**, closing
   `remaining_tasks/deferred.md`'s "region file LZ4/zstd framing" follow-up:
   `RegionStore::flush()` now runs each entry's chunk-codec payload through
   the same `compress_for_disk()` policy `net::frame_message()` uses for wire
   messages (a local copy of the threshold + "only if it helps" gate, not a
   shared function — `world/` has no business depending on `net/` for one
   size constant) before writing it, and `region_for()`'s read path reverses
   it transparently. The on-disk format version bumped 1 -> 2 to add a
   per-entry `u8 flags` byte (bit 0 = "this entry's payload is LZ4-framed");
   version-1 files (no flags byte) are still read correctly (`flags` defaults
   to 0 = uncompressed for them) — an existing on-disk world upgrades in
   place, one edited chunk at a time, not via any migration pass. Wired into
   `voxel_browser_server` only; the `--singleplayer` integrated server still
   has no `RegionStore` at all, a separate, still-open gap (see
   `remaining_tasks/deferred.md`).
6. **Account/auth**: `auth_mode = none | token` — token verification service is
   out of scope for v0 but the handshake reserves the field. **Direction set
   (2026-09-17, not yet implemented):** the engine will not own an auth
   concept at all — `vb.db` (§10.6) gives packs a generic per-key store, and
   any login flow (recognizing a returning player, credential checks) is
   entirely pack-implemented on top of it plus the UI/input APIs. This
   `auth_mode` field stays reserved for a future *transport-level* token
   check, which is a different, lower-level concern than pack-level identity.
   **Future direction noted (2026-09-17, not yet planned into a phase):** a
   baked-in OpenID Connect client using the loopback-redirect flow (RFC 8252)
   -- the client opens the system browser to the identity provider and spins
   up a local HTTP listener to catch the redirect back, so no embedded
   browser or client secret is needed. This would land as a new `auth_mode =
   oidc` value at the transport/handshake level, alongside (not replacing)
   the pack-level `vb.db`-based identity approach above.
   **Direction revised (2026-10-05, planned as Phase 9):** mandatory,
   engine-verified auth declared by a pack-level `auth.lua` (provider,
   client id, scopes, name/claim mapping); presence = mandatory, verified
   during the handshake before asset sync, exposed to Lua as
   `player:get_login()` (`nil` = no auth). This is the anticipated
   `auth_mode = oidc`, chosen by the pack rather than `server.toml`. Full
   design: `architecture_spec/auth.md`.
7. **Entity visual presentation**: 3D blocky models vs. 2D sprites.
   **Resolved (2026-09-11): Don't Starve-style Y-axis-billboarded sprites**, not
   blocky models — full design in §11.3. Key parameters locked in: raylib
   `DrawBillboardPro` with a fixed world-up (verified against `rmodels.c`, no
   custom quad math); default 8 discrete facings authored as 5 unique poses +
   engine-side mirroring; animation state resolved client-side from
   `EntityRecord.vel` + `flags` bits (`on_ground` existing, `dead`/`hurt_pulse`/
   `acting` new — not yet wired on the wire, see `REMAINING_TASKS.md` 3.5).
   Deferred to implementation time: exact clip-name base set beyond the priority
   list itself, and whether a blob-shadow decal ships alongside v1 or later.
8. **In-process server: product mode vs. diagnostic.** `--singleplayer` (the
   `Singleplayer` struct in `src/client/session_host.hpp`, an in-process
   `ServerSession` over `LoopbackNetwork`) was exposed as "Play Singleplayer".
   **Direction (2026-10-25): diagnostic, not a play mode.** The product is
   multiplayer-first; new users come in through `vb`, and `vb pack dev` hosts a
   real dedicated server joined over GNS, so nobody needs the in-process path to
   play or to author packs. Its real value is isolating netcode — rendering,
   worldgen, pack Lua, physics, persistence and automation against the same
   server code with no GNS, asset sync or compression negotiation — which is
   also exactly what it can't vouch for. Plan (`REMAINING_TASKS.md` Phase 11):
   drop the menu button, rename the flag (proposed `--loopback`, keeping
   `--singleplayer` as a deprecated alias), document what it doesn't cover,
   and later share the server bootstrap with `src/server/main.cpp`. Name still
   pending sign-off.
