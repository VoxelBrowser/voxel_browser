"""Windowed clients: a real window, the raygui main menu, screenshots (docs/e2e-automation.md).

These need a display; run the suite under `xvfb-run -a` (CI does). Without one they skip.
raygui reads raylib's real mouse state, which a script can't produce, so the menu commands
inject the *result* of a click (and fill the same fields a player would type into).
"""
import pathlib
import time

import pytest

from vbtest import expect
from vbtest.png import read_png, stats
from vbtest.scene import block_under_feet
from vbtest.stack import make_pack

REPO = pathlib.Path(__file__).resolve().parents[2]


def test_main_menu_connect_and_world_screenshots(server, clients, artifact_dir):
    (alice,) = clients(1, names=["Alice"], via_menu=True)
    expect(alice).to_be_in_state("menu")

    menu_shot = alice.screenshot(artifact_dir / "menu.png")
    assert (menu_shot["width"], menu_shot["height"]) == (720, 360)
    menu = stats(menu_shot["path"])
    assert menu["distinct_colors"] >= 4, "the menu screenshot looks blank: %r" % menu

    alice.menu_connect("127.0.0.1", server.port, name="Alice")
    expect(alice).to_be_in_state("playing", timeout=60)  # connecting -> asset sync -> loading screen -> playing
    expect(server).to_have_player_count(1)
    assert server.player("Alice") is not None
    expect(alice).to_be_on_ground()

    world_shot = alice.screenshot(artifact_dir / "world.png")
    world = stats(world_shot["path"])
    assert world["distinct_colors"] >= 40, "the in-game screenshot looks blank: %r" % world
    assert world["mean_rgb"] != menu["mean_rgb"], "the screenshot did not change after joining"


def test_connecting_panel_fits_on_screen(server, clients, artifact_dir):
    """Regression guard: MainMenu::draw_connecting/draw_error used to put the panel at a
    hardcoded y=220 (src/render/main_menu.cpp) regardless of actual screen height. At the
    engine's own enforced minimum window height (360px, kMinWindowHeight), the asset-sync
    variant of this panel is 178px tall -- 220+178=398 -- so its bottom (the Cancel button)
    rendered clipped off the bottom edge of the window. Now vertically centered instead."""
    (alice,) = clients(1, names=["Alice"], via_menu=True)
    expect(alice).to_be_in_state("menu")

    alice.menu_connect("127.0.0.1", server.port, name="Alice")
    # Race for a screenshot while still connecting (asset sync is fast on loopback with
    # this tiny pack, so there's no wait_for predicate for "mid-handshake" to use instead).
    shot = None
    for _ in range(20):
        if alice.state().get("app_state") == "connecting":
            shot = alice.screenshot(artifact_dir / "connecting.png")
            break
        if alice.state().get("app_state") not in ("connecting", "menu"):
            break
        time.sleep(0.02)
    if shot is None:
        pytest.skip("never caught the client mid-connect (handshake finished too fast)")

    width, height, channels, rows = read_png(shot["path"])
    bg = (18, 18, 22)  # Color{18,18,22,255}, src/render/window.cpp's ClearBackground
    last_row = rows[-1]
    non_bg = sum(1 for i in range(0, width * channels, channels)
                 if tuple(last_row[i:i + 3]) != bg)
    assert non_bg == 0, (
        "the connecting panel's bottom row bled into the window's last pixel row "
        "(clipped off-screen): %d non-background pixels" % non_bg)


@pytest.mark.parametrize("windowed", [False, True], ids=["headless", "windowed"])
def test_typing_into_the_chat_box(server, clients, windowed):
    alice, bob = clients(2, names=["Alice", "Bob"], windowed=windowed)
    alice.type("hello ")
    alice.type("from the chat box")
    assert alice.state()["chat_open"] is True
    expect(bob).not_.to_have_chat("from the chat box", timeout=1)  # nothing is sent until Enter
    alice.key_press("enter")
    expect(bob).to_have_chat("Alice: hello from the chat box")
    assert alice.state()["chat_open"] is False


def test_screenshot_needs_a_window(server, clients, artifact_dir):
    (alice,) = clients(1, names=["Alice"])  # headless
    from vbtest import AutomationError
    with pytest.raises(AutomationError) as e:
        alice.screenshot(artifact_dir / "nope.png")
    assert e.value.code == "unsupported"


def test_singleplayer_says_on_the_menu_when_the_pack_needs_another_engine(make_clients, tmp_path, artifact_dir):
    # pack.toml's engine_version_req admits no current build: singleplayer must not start (it used to
    # run the base block set and only say why on stderr); the error screen names the requirement.
    pack = make_pack(REPO, tmp_path, {"pack.toml": 'name = "too_new"\nversion = "1.0.0"\nengine_version_req = ">=99.0.0"\n'})
    (solo,) = make_clients(None)(1, names=["Solo"], via_menu=True,
                                 extra_args=["--content-pack", str(pack), "--world-dir", str(tmp_path / "world")])
    expect(solo).to_be_in_state("menu")
    solo.menu_singleplayer()
    expect(solo).to_be_in_state("error", timeout=30)
    message = solo.state()["error_message"]
    assert "singleplayer" in message and ">=99.0.0" in message, message
    shot = solo.screenshot(artifact_dir / "singleplayer_engine_req.png")
    assert stats(shot["path"])["distinct_colors"] >= 4
