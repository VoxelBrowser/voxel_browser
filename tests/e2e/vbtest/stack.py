"""Starting a server and clients, independent of pytest fixtures.

The function-scoped `server`/`clients` fixtures in conftest.py are thin wrappers over these, and
tests that need a *second* world (record a session, then replay it on a fresh server) use them
directly. Everything started is appended to `procs`, which the caller closes.
"""
import json
import os
import pathlib
import shutil

from .expect import expect
from .handles import Client, Server
from .net import check_host, free_udp_port
from .process import ProcessDied, timeout_scale

EXE = ".exe" if os.name == "nt" else ""
DEFAULT_SERVER_CONFIG = {"world_seed": 7, "view_distance": 3, "persist_world": False, "tick_rate": 20}


def start_server(server_bin, repo, artifact_dir, workdir, procs, config=None, extra_args=(), name="server",
                 pack_files=None):
    """A fresh dedicated server on a free loopback port, with a private copy of the base pack
    (nothing a test does, e.g. vb.storage, can dirty the repo). `pack_files` ({relative path: text})
    are written into that copy, e.g. an `auth.lua` that makes the pack require sign-in.
    Returns a connected `Server`."""
    # The server drops a join stuck in one handshake step for this long; stretch it
    # like every other wait, or a slow (sanitizer) server kicks a client the harness
    # is still happily waiting for ("join failed: handshake timeout").
    cfg = {**DEFAULT_SERVER_CONFIG, "handshake_timeout_seconds": 10.0 * timeout_scale(), **(config or {})}
    workdir = pathlib.Path(workdir)
    pack = make_pack(repo, workdir, pack_files)
    last = None
    for _ in range(3):  # a free port can be taken between picking it and binding it
        port = free_udp_port()
        lines = ["port = %d" % port, 'content_pack = "%s"' % pack.as_posix(),
                 'world_dir = "%s"' % (workdir / "world").as_posix()]
        for k, v in cfg.items():
            lines.append("%s = %s" % (k, json.dumps(v)))
        toml = workdir / "server.toml"
        toml.write_text("\n".join(lines) + "\n")
        proc = Server(name, [str(server_bin), "--config", str(toml), "--automation", "stdio"] + list(extra_args),
                      artifact_dir, cwd=str(workdir))
        procs.append(proc)
        try:
            info = proc.hello()
            proc.port = port
            assert info["role"] == "server"
            return proc
        except (ProcessDied, TimeoutError) as e:
            last = e
            proc.close()
            procs.remove(proc)
    raise last


def make_pack(repo, workdir, pack_files=None):
    """A private copy of content/base under `workdir` with `pack_files` ({path: text}) written into it."""
    pack = pathlib.Path(workdir) / "content" / "base"
    if not pack.exists():
        shutil.copytree(pathlib.Path(repo) / "content" / "base", pack)
    for rel, text in (pack_files or {}).items():
        (pack / rel).write_text(text)
    return pack


class ClientFactory:
    """`clients(n, names=...)`: real client processes, each with its own empty asset cache."""

    def __init__(self, client_bin, server, artifact_dir, workdir, procs, extra_args=(), allow_remote=False,
                 skip=None):
        self.client_bin, self.server, self.artifact_dir = client_bin, server, artifact_dir
        self.workdir, self.procs, self.extra_args = pathlib.Path(workdir), procs, list(extra_args)
        self.allow_remote, self.skip, self._count = allow_remote, skip, 0

    def __call__(self, n=1, names=None, host="127.0.0.1", port=None, join=True, render_distance=2, windowed=False,
                 via_menu=False, tcp=False, extra_args=(), home=None, singleplayer=None):
        """windowed=True opens a real window (needs a display, e.g. `xvfb-run -a pytest ...`);
        via_menu=True starts on the main menu instead of connecting (drive it with `menu_connect`).
        tcp=True drives the client over `--automation tcp` (loopback, token) instead of its stdio.
        extra_args are appended to the client's command line (e.g. `--automation-record f.py`).
        home="name" reuses one client data directory (config, cache, saved sign-ins) across several
        spawns: a second client started with the same `home` is "the same install", e.g. one that
        finds yesterday's refresh token.
        singleplayer="<pack dir>" runs the integrated server on that content pack instead of
        connecting to `server` (which may then be None); join/expect work the same."""
        check_host(host, self.allow_remote)  # §7.4: test bots never go near a real server
        if via_menu:
            windowed, join = True, False  # the menu only exists in a window
        if windowed and not os.environ.get("DISPLAY"):
            msg = "windowed tests need a display: run under xvfb-run (e.g. `xvfb-run -a ctest -L e2e`)"
            if self.skip:
                self.skip(msg)
            raise RuntimeError(msg)
        made = []
        for i in range(n):
            self._count += 1
            idx = self._count
            name = names[i] if names else "Player%d" % idx
            home_dir = self.workdir / (("home-" + home) if home else ("client%d" % idx))
            cache = home_dir / "cache"
            cache.mkdir(parents=True, exist_ok=bool(home))
            conf = home_dir / "client.toml"
            # asset_cache_dir pinned explicitly (not left to vb::core::user_cache_dir()'s
            # own default) so this fixture's own `cache_dir` attribute is actually where
            # the client writes its cache on every platform -- the HOME/XDG_CACHE_HOME/
            # LOCALAPPDATA overrides below only redirect that default on Linux/Windows;
            # macOS's user_cache_dir() hardcodes "~/Library/Caches" regardless of
            # XDG_CACHE_HOME, which silently pointed `cache_dir` at a directory the
            # client never actually wrote to.
            conf.write_text(
                    'player_name = "%s"\nrender_distance = %d\nasset_cache_dir = "%s"\n'
                    % (name, render_distance, str(cache)) +
                    # 720x360: the engine's own enforced minimum window size
                    # (kMinWindowWidth/Height, inc/vb/core/config.hpp) -- a
                    # smaller request gets silently clamped up to this by
                    # apply_cli_overrides(ClientConfig&, ...), so asking for
                    # anything smaller here would just be a lie about what
                    # the client actually opens at.
                    ('window_width = 720\nwindow_height = 360\nvsync = false\n' if windowed else ""))
            env = dict(os.environ, HOME=str(home_dir), XDG_CACHE_HOME=str(cache), LOCALAPPDATA=str(cache),
                       # saved sign-ins live under the config dir: never the real one, whatever the runner sets
                       XDG_CONFIG_HOME=str(home_dir / "config"), APPDATA=str(home_dir / "config"),
                       # ...and never the developer's OS keychain either: refresh tokens stay in files
                       VB_KEYCHAIN="off",
                       # the client's own 10 s connect deadline, stretched like every other wait
                       VB_CONNECT_TIMEOUT_SECONDS=str(int(10 * timeout_scale())))
            argv = [str(self.client_bin)] + ([] if windowed else ["--headless"])
            if singleplayer:
                argv += ["--loopback", "--content-pack", str(singleplayer), "--world-dir",
                         str(home_dir / "world")]
            elif not via_menu:  # --server connects straight away; without it a windowed client opens on the menu
                argv += ["--server", host, "--port", str(port or self.server.port)]
            argv += ["--name", name, "--config", str(conf), "--render-distance", str(render_distance),
                     "--automation", "tcp" if tcp else "stdio"] + self.extra_args + list(extra_args)
            c = Client("client-" + name, argv, self.artifact_dir, env=env, tcp=tcp)
            c.player_name = name
            c.cache_dir = cache
            c.config_path = conf
            self.procs.append(c)
            made.append(c)
        for c in made:
            c.hello()  # headless: answered once connected (or it exits with its error); windowed: at once
        if join:
            for c in made:
                expect(c).to_be_joined()
                expect(c).to_have_loaded_chunks(8)
                # Windowed clients sit on a loading screen until their first chunks are meshed.
                expect(c).to_be_in_state("playing", timeout=60)
        return made
