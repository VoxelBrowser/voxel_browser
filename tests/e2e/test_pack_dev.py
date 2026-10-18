"""`vb pack dev`: host a pack with restart-on-save, end to end.

Real `vb` (linking this build as a pseudo-version), real server started by
`vb host --watch`, real clients joining it. The client `vb pack dev` would open
itself is turned off (`--no-client`); the harness's own clients stand in for it.
"""
import os
import pathlib
import signal
import subprocess
import time

import pytest

from vbtest import expect
from vbtest.net import free_udp_port
from vbtest.process import timeout_scale
from vbtest.stack import EXE, ClientFactory, make_pack

pytestmark = pytest.mark.skipif(os.name == "nt", reason="stops vb with SIGINT to its process group")

# Written into the pack while it is being hosted: only a server that reloaded the
# pack sends this.
GREETER = """
local greeted = {}
vb.on("player_input", function(player)
	local name = player:get_name()
	if not greeted[name] then
		greeted[name] = true
		player:send_message("dev-reload: v2")
	end
end)
"""


def _wait_for_line(log, text, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if log.exists() and text in log.read_text(errors="replace"):
            return
        time.sleep(0.2)
    raise AssertionError("vb never printed %r within %.0fs; output:\n%s"
                         % (text, timeout, log.read_text(errors="replace") if log.exists() else "(none)"))


@pytest.fixture
def vb_dev(binaries, artifact_dir, tmp_path):
    """Starts `vb pack dev <copy of base> --no-client` and yields (pack dir, port, log path)."""
    build = binaries["server"].parent
    vb = build / ("vb" + EXE)
    if not vb.exists():
        pytest.skip("no vb binary in %s (VB_BUILD_CLI off)" % build)
    home = tmp_path / "vbhome"
    env = dict(os.environ, VB_HOME=str(home), HOME=str(tmp_path / "home"), XDG_CONFIG_HOME=str(tmp_path / "config"),
               XDG_DATA_HOME=str(tmp_path / "data"), XDG_CACHE_HOME=str(tmp_path / "cache"))
    subprocess.run([str(vb), "link", "dev", str(build)], env=env, check=True, capture_output=True, timeout=60)
    pack = make_pack(pathlib.Path(__file__).resolve().parents[2], tmp_path)
    port = free_udp_port()
    log = pathlib.Path(artifact_dir) / "vb-pack-dev.log"
    out = open(log, "w")
    proc = subprocess.Popen([str(vb), "pack", "dev", str(pack), "--version", "dev", "--port", str(port), "--no-client"],
                            env=env, stdout=out, stderr=subprocess.STDOUT, start_new_session=True)
    try:
        yield pack, port, log
    finally:
        # What a Ctrl+C in the terminal does: the whole group (vb and its server) gets SIGINT.
        try:
            os.killpg(proc.pid, signal.SIGINT)
            proc.wait(timeout=30 * timeout_scale())
        except (ProcessLookupError, subprocess.TimeoutExpired):
            try:
                os.killpg(proc.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            proc.wait()
        out.close()


def test_pack_dev_hosts_the_pack_and_restarts_it_on_save(binaries, artifact_dir, tmp_path, _procs, vb_dev):
    pack, port, log = vb_dev
    _wait_for_line(log, "watching", 60 * timeout_scale())
    clients = ClientFactory(binaries["client"], None, artifact_dir, tmp_path, _procs)

    # The server `vb pack dev` started serves the pack (it may still be starting: retry the join).
    deadline = time.monotonic() + 60 * timeout_scale()
    while True:
        try:
            (alice,) = clients(1, names=["Alice"], port=port)
            break
        except Exception:
            if time.monotonic() > deadline:
                raise
            time.sleep(1)
    expect(alice).to_be_joined()

    # Saving a file in the pack restarts the server on the new contents.
    (pack / "zz_dev_greeter.lua").write_text(GREETER)
    _wait_for_line(log, "-- restarting", 30 * timeout_scale())
    alice.close()
    _procs.remove(alice)

    deadline = time.monotonic() + 60 * timeout_scale()
    while True:
        try:
            (bob,) = clients(1, names=["Bob"], port=port)
            break
        except Exception:
            if time.monotonic() > deadline:
                raise
            time.sleep(1)
    expect(bob).to_be_joined()
    expect(bob).to_have_chat("dev-reload: v2", timeout=30)
    assert "zz_dev_greeter.lua" in log.read_text(errors="replace")  # the change vb reported
