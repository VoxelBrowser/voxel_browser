"""Typed wrappers over the automation protocol (docs/automation-protocol.md)."""
from .process import Process, timeout_scale


def _ms(seconds):
    return int(seconds * 1000 * timeout_scale())


class _Base(Process):
    def wait_for(self, pred, timeout=5.0):
        """Block until `pred` holds (evaluated every frame/tick in the process). Raises
        AutomationError(code='timeout') carrying what was last observed."""
        return self.call("wait_for", _timeout=timeout + 5.0, pred=pred, timeout_ms=_ms(timeout))

    role = "?"


class Server(_Base):
    role = "server"
    port = 0

    def block_at(self, pos):
        """Registry name of the block at `pos`, or None."""
        return self.call("block_at", pos=list(pos)).get("block")

    def set_block(self, pos, block):
        return self.call("set_block", pos=list(pos), block=block)

    def fill(self, lo, hi, block):
        return self.call("fill", **{"from": list(lo), "to": list(hi), "block": block})

    def teleport(self, player, pos):
        return self.call("teleport", player=_name(player), pos=list(pos))

    def give(self, player, item, count=1):
        return self.call("give", player=_name(player), item=item, count=count)

    def set_time(self, ticks):
        return self.call("set_time", ticks=ticks)

    def set_health(self, player, value):
        return self.call("set_health", player=_name(player), value=value)

    def kick(self, player, reason="kicked by test"):
        return self.call("kick", player=_name(player), reason=reason)

    def advance_reauth(self, player, seconds):
        """Fast-forward the player's re-auth timer (or, once asked, its grace countdown)."""
        return self.call("advance_reauth", player=_name(player), seconds=seconds)

    def run_lua(self, code):
        return self.call("run_lua", code=code)

    def player(self, name):
        for p in self.state()["players"]:
            if p["name"] == _name(name):
                return p
        return None


class Client(_Base):
    role = "client"
    player_name = ""

    # state helpers
    @property
    def feet(self):
        return self.state().get("feet")

    # raw input -------------------------------------------------------------
    def key_press(self, key):
        return self.call("key.press", key=key)

    def key_hold(self, key, frames):
        return self.call("key.hold", _timeout=frames / 20.0 + 15.0, key=key, frames=frames)

    def mouse_press(self, button="left"):
        return self.call("mouse.press", button=button)

    def mouse_hold(self, button, frames):
        return self.call("mouse.hold", _timeout=frames / 20.0 + 15.0, button=button, frames=frames)

    def capture_mouse(self, on=True):
        return self.call("mouse.capture", on=on)

    def look(self, yaw, pitch):
        return self.call("look", yaw=yaw, pitch=pitch)

    def look_at(self, pos):
        return self.call("look_at", pos=list(pos))

    # actions ---------------------------------------------------------------
    def select_slot(self, n):
        return self.call("select_slot", n=n)

    def walk_to(self, pos, tolerance=0.5, timeout=20.0):
        """pos = (x, y|None, z)."""
        return self.call("walk_to", _timeout=timeout + 10.0, pos=list(pos), tolerance=tolerance,
                         timeout_ms=_ms(timeout))

    def break_block(self, pos, timeout=10.0):
        return self.call("break_block", _timeout=timeout + 10.0, pos=list(pos), timeout_ms=_ms(timeout))

    def place_block(self, pos, face, timeout=10.0):
        """Place against face `face` (unit axis vector) of the existing block at `pos`."""
        return self.call("place_block", _timeout=timeout + 10.0, pos=list(pos), face=list(face),
                         timeout_ms=_ms(timeout))

    def chat(self, text):
        return self.call("chat.send", text=text)

    # windowed-only --------------------------------------------------------
    def menu_set_name(self, name):
        return self.call("menu.set_name", name=name)

    def menu_connect(self, host, port, name=None):
        """Click "Connect" on the main menu (the transition shows up in `app_state`)."""
        return self.call("menu.connect", host=host, port=port, **({"name": name} if name else {}))

    def menu_singleplayer(self, name=None):
        return self.call("menu.singleplayer", **({"name": name} if name else {}))

    def signin(self, choice):
        """Click a sign-in screen button on a windowed client: "trust" (Continue on the first-use
        prompt), "browser" or "cancel". The next frame acts on it."""
        return self.call("signin." + choice)

    def type(self, text):
        """Type into the chat box as keyboard characters would (then `key_press("enter")` sends it)."""
        return self.call("type", text=text)

    def screenshot(self, path):
        """Save the framebuffer as a PNG; windowed clients only. Returns {path, width, height}."""
        return self.call("screenshot", _timeout=30.0, path=str(path))

    # UI --------------------------------------------------------------------
    def ui(self, widget_id):
        return Locator(self, widget_id, hud=False)

    def hud(self, widget_id):
        return Locator(self, widget_id, hud=True)

    def hud_widget(self, widget_id):
        """The HUD widget's last-reported `{id, type, text, x, y, w, h}`, or None."""
        for w in self.state().get("hud", {}).get("widgets", []):
            if w["id"] == widget_id:
                return w
        return None


class Locator:
    """A widget found by its stable `id` on the open modal screen (or the HUD)."""

    def __init__(self, client, widget_id, hud):
        self.client, self.id, self._prefix = client, widget_id, "hud" if hud else "ui"

    def click(self):
        return self.client.call(self._prefix + ".click", id=self.id)

    def fill(self, text):
        return self.client.call(self._prefix + ".fill", id=self.id, text=text)

    def select(self, index):
        return self.client.call(self._prefix + ".select", id=self.id, index=index)


def _name(player):
    """Accepts a Client handle, a name, or a net id."""
    return getattr(player, "player_name", None) or player
