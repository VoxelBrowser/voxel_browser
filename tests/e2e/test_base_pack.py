"""Phase C4 (docs/content-base-testing.md): content/base journeys that genuinely need two
real processes and a cold cache -- asset sync, the full on_break -> ItemDropSystem -> pickup
chain, a real keybind -> server -> UI push round trip, and held-item placing through a real
client. Everything cheaper than this lives in tests/unit/content_base_*_test.cpp instead
(see CONTRIBUTING.md's "Testing content/base itself").
"""
import time
from pathlib import Path

from vbtest import expect
from vbtest.scene import block_under_feet, clear_corridor, step_aside

_CONTENT_BASE = Path(__file__).resolve().parent.parent.parent / "content" / "base"


def test_base_pack_textures_arrive_over_asset_sync(server, clients):
    (alice,) = clients(1, names=["Alice"])
    expect(alice).to_be_on_ground()
    # The asset cache is content-addressed (file *bytes* hashed, see
    # ClientAssetCache::shard_dir/file_path in src/assetsync/cache.cpp) -- a cached
    # texture's filename on disk is its hash, not "stone.png". Compare bytes instead,
    # proving each real texture file genuinely transferred, not just *some* file
    # (test_multiplayer_basics.py's own cache assertion only checks the cache
    # directory is non-empty at all).
    cached_blobs = [p.read_bytes() for p in alice.cache_dir.rglob("*") if p.is_file()]
    for name in ("stone.png", "water.png", "player.png", "dropped_item.png"):
        real_bytes = (_CONTENT_BASE / "textures" / name).read_bytes()
        assert real_bytes in cached_blobs, "%s's bytes never reached the client's asset cache" % name


def test_break_drop_pickup_round_trip(server, clients):
    (alice,) = clients(1, names=["Alice"])
    x, y, z = block_under_feet(alice)
    clear_corridor(server, x, y, z)
    # 1m away, not test_block_break_replicates_to_other_client's 2m -- on_break's
    # spawn_item_drop lands at the broken block's own center, and ItemDropSystem's
    # default pickup radius is 1.5m; 2m away (that test's own distance) is outside it,
    # so the drop would just sit there forever without a walk_to this test doesn't need.
    target = (x, y, z - 1)
    server.set_block(target, "base:stone")
    expect(alice).to_see_block(target, "base:stone")

    alice.break_block(target)
    expect(alice).to_see_block(target, "base:air")
    expect(alice).to_have_inventory("base:stone", 1)


def test_base_pause_screen_opens_from_the_real_escape_keybind(server, clients):
    (alice,) = clients(1, names=["Alice"])
    # The base:inventory half of this (E key) is already covered by
    # test_multiplayer_basics.py's test_inventory_screen_opens_and_closes; base:pause's
    # Escape binding (src/client/main.cpp's kCustomKeybinds) has no e2e coverage at all.
    alice.key_press("pause")
    expect(alice).to_have_ui_open("base:pause")
    expect(alice).to_have_widget("resume")
    expect(alice).not_.to_have_widget("signed_in")  # no auth.lua: state.login is nil
    alice.ui("resume").click()
    expect(alice).not_.to_have_ui_open("base:pause")


def test_placing_uses_the_actually_held_item_not_a_hardcoded_block(server, clients):
    """Pre-6.20 regression guard: RMB placing used to send a hardcoded base:stone block
    id regardless of what was in the selected slot (mechanics.lua's own header comment)."""
    (alice,) = clients(1, names=["Alice"])
    x, y, z = block_under_feet(alice)
    clear_corridor(server, x, y, z)
    support = (x, y, z - 2)
    server.set_block(support, "base:stone")
    server.give(alice, "base:planks", 3)
    expect(alice).to_have_inventory("base:planks", 3)

    alice.select_slot(1)
    placed_at = (x, y + 1, z - 2)
    alice.place_block(support, face=(0, 1, 0))
    expect(alice).to_see_block(placed_at, "base:planks")
    expect(alice).not_.to_have_inventory("base:planks", 3)  # one spent


def test_a_windowed_client_draws_the_hud(server, clients, artifact_dir):
    (alice,) = clients(1, names=["Alice"], windowed=True)
    expect(alice).to_be_on_ground()

    shot = alice.screenshot(artifact_dir / "hud.png")
    from vbtest.png import stats
    world = stats(shot["path"])
    # content/base/ui/hud.lua's hotbar/player-list/chat-log widgets draw on top of the
    # world every frame -- a blank/near-blank frame means either the HUD or the world
    # itself failed to render (same ceiling test_windowed.py's own world screenshot uses).
    assert world["distinct_colors"] >= 40, "the HUD/world screenshot looks blank: %r" % world


def test_a_plain_jump_takes_no_fall_damage(server, clients):
    """Regression guard: fall_damage.lua's SAFE_SPEED used to be a hardcoded 8.0 m/s, below
    the engine's own default jump_speed (8.9 m/s) -- since a jump lands at about the speed
    it launched at, every ordinary jump on flat ground took fall damage. Drives a real jump
    through the real input path (not tests/unit/content_base_behaviour_test.cpp's synthetic
    `land()`, which sets velocity directly) so the actual launch/landing speed round trip is
    exercised, not just the policy curve. See fall_damage.lua's own comment."""
    (alice,) = clients(1, names=["Alice"])
    expect(alice).to_be_on_ground()
    expect(alice).to_have_health(20)

    # key.press is raw input (docs/automation-protocol.md): unlike walk_to/break_block it
    # doesn't auto-capture the mouse, so movement/jump keys are silently ignored (same as a
    # real player who hasn't clicked to capture) until this is called.
    alice.capture_mouse()
    alice.key_press("jump")
    expect(alice).not_.to_be_on_ground(timeout=2.0)  # airborne
    expect(alice).to_be_on_ground(timeout=5.0)  # landed again

    # player_landed's damage (if any) lands a tick or two after on_ground flips true, not
    # on the same frame -- this is checking an *absence*, so unlike every other wait in this
    # suite there's no event to wait_for; a short settle is the only way to not race the
    # check against the hook (confirmed by hand: health dropped ~0.1-0.2s after landing with
    # the pre-fix SAFE_SPEED, which an immediate check here missed entirely).
    time.sleep(0.5)
    expect(alice).to_have_health(20)  # must be unscathed


def test_hud_health_bar_follows_the_players_health(server, clients):
    """The whole chain: server health -> S2C_PlayerStatus -> client.health() -> base:hud's bar."""
    (alice,) = clients(1, names=["Alice"])
    expect(alice).to_be_on_ground()

    # Full health: the bar's background, fill and "20 / 20" readout are all up.
    expect(alice).to_have_health(20)
    expect(alice).to_have_hud_widget("health_text", text="20 / 20")
    full = alice.hud_widget("health_fill")
    background = alice.hud_widget("health_bg")
    assert full is not None and background is not None
    assert abs(full["w"] - (background["w"] - 2)) < 0.01  # fill spans the inside of the border

    # With something in the inventory the hotbar exists, and the bar sits above it.
    server.give("Alice", "base:stone", 1)
    expect(alice).to_have_inventory("base:stone", 1)
    expect(alice).to_have_hud_widget("hotbar_container")
    hotbar = alice.hud_widget("hotbar_container")
    assert background["y"] + background["h"] <= hotbar["y"], (background, hotbar)

    # Half health: the client sees the new value and the fill is half as wide.
    server.set_health("Alice", 10)
    expect(alice).to_have_health(10)
    expect(alice).to_have_hud_widget("health_text", text="10 / 20")
    half = alice.hud_widget("health_fill")
    assert abs(half["w"] - full["w"] / 2) < 0.01, (half, full)

    # Healing back restores the full-width fill.
    server.set_health("Alice", 20)
    expect(alice).to_have_health(20)
    expect(alice).to_have_hud_widget("health_text", text="20 / 20")
    assert abs(alice.hud_widget("health_fill")["w"] - full["w"]) < 0.01
