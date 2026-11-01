#!/usr/bin/env python3
"""Real-UDP server capacity benchmark: step the bot count up and see where the
server stops keeping up.

For each N in --steps it starts a fresh voxel_browser_server (temporary world,
fixed seed), connects N bots with vb_loadtest, and reads the server's
--status-file while they play. Prints one table row per step:

    bots  joined  srv tick avg/max (ms)  tick rate  rx/bot KiB/s  rtt p50  verdict

"verdict" is OK while the server still hits its target tick rate and its
average tick work stays under 80% of the tick period. See docs/loadtest.md.

Needs a build with VB_WITH_NET=ON (vb_loadtest and voxel_browser_server are
both written to the build directory's root).
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import tempfile
import time
import tomllib
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent


def exe(build: Path, name: str) -> Path:
    for candidate in (build / name, build / f"{name}.exe"):
        if candidate.exists():
            return candidate
    sys.exit(f"loadtest: {name} not found in {build} (build it with -DVB_WITH_NET=ON)")


def read_status(path: Path) -> dict | None:
    try:
        return tomllib.loads(path.read_text())
    except (OSError, tomllib.TOMLDecodeError):
        return None  # not written yet, or caught mid-rename


def run_step(args: argparse.Namespace, n: int, server_bin: Path, bot_bin: Path) -> dict:
    work = Path(tempfile.mkdtemp(prefix=f"vb_loadtest_{n}_"))
    status_file = work / "status.toml"
    stop_file = work / "stop"
    server_log = (work / "server.log").open("w")
    server_cmd = [
        str(server_bin),
        "--port", str(args.port),
        "--max-players", str(n),
        "--seed", str(args.seed),
        "--world-dir", str(work / "world"),
        "--content-pack", str(Path(args.content_pack).resolve()),
        "--status-file", str(status_file),
        "--stop-file", str(stop_file),
    ]
    # cwd = the scratch dir, so no stray server.toml in the caller's cwd is picked up.
    server = subprocess.Popen(server_cmd, cwd=work, stdout=server_log, stderr=subprocess.STDOUT)
    try:
        deadline = time.monotonic() + args.server_start_timeout
        while read_status(status_file) is None:
            if server.poll() is not None:
                sys.exit(f"loadtest: server exited early, see {work / 'server.log'}")
            if time.monotonic() > deadline:
                sys.exit(f"loadtest: server did not start, see {work / 'server.log'}")
            time.sleep(0.2)

        summary_path = work / "bots.json"
        bot_cmd = [
            str(bot_bin),
            "--host", "127.0.0.1",
            "--port", str(args.port),
            "--bots", str(n),
            "--ramp", str(args.ramp),
            "--duration", str(args.duration),
            "--seed", str(args.seed),
            "--json", str(summary_path),
            "--quiet",
        ]
        if args.idle:
            bot_cmd.append("--idle")
        bots = subprocess.Popen(bot_cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)

        # Keep every status window written while all joined bots were online.
        windows: list[dict] = []
        last_updated = None
        while bots.poll() is None:
            st = read_status(status_file)
            if st and st.get("updated") != last_updated:
                last_updated = st.get("updated")
                if len(st.get("players", [])) >= n and st.get("tick_ms_avg") is not None:
                    windows.append(st)
            time.sleep(0.5)
        bot_output = bots.stdout.read() if bots.stdout else ""
        if args.verbose:
            print(bot_output)

        summary = {}
        if summary_path.exists():
            summary = json.loads(summary_path.read_text())
        else:
            print(bot_output, file=sys.stderr)

        target_rate = windows[-1]["target_tick_rate"] if windows else 20
        period_ms = 1000.0 / target_rate
        # The first full window may still include joins/terrain bursts; it
        # counts toward max but not toward the steady-state average.
        steady = windows[1:] if len(windows) > 1 else windows
        row = {
            "bots": n,
            "joined": summary.get("bots_joined", 0),
            "failures": summary.get("failures", {}),
            "server_tick_ms_avg": (sum(w["tick_ms_avg"] for w in steady) / len(steady)) if steady else None,
            "server_tick_ms_max": max((w["tick_ms_max"] for w in windows), default=None),
            "tick_rate": min((w["tick_rate"] for w in steady), default=None),
            "target_tick_rate": target_rate,
            "rx_kib_s_per_bot": summary.get("rx_kib_s_per_bot"),
            "rx_kib_s_total": summary.get("rx_kib_s_total"),
            "rtt_ms_p50": summary.get("rtt_ms_p50"),
            "join_s_max": summary.get("join_s_max"),
            "bot_tick_ms_p95": summary.get("bot_tick_ms_p95"),
            "status_windows": len(windows),
        }
        ok = (
            row["joined"] == n
            and row["server_tick_ms_avg"] is not None
            and row["server_tick_ms_avg"] < 0.8 * period_ms
            and row["tick_rate"] is not None
            and row["tick_rate"] >= 0.95 * target_rate
        )
        if row["joined"] != n:
            row["verdict"] = "JOIN FAIL"
        elif not windows:
            row["verdict"] = "NO DATA"  # run too short for a full status window
        else:
            row["verdict"] = "OK" if ok else "BEHIND"
        if (row["bot_tick_ms_p95"] or 0.0) > 0.8 * period_ms:
            row["verdict"] += " (bot-bound)"
        return row
    finally:
        stop_file.touch()
        try:
            server.wait(timeout=30)
        except subprocess.TimeoutExpired:
            server.kill()
            server.wait()
        server_log.close()
        if args.keep:
            print(f"  kept {work}")
        else:
            shutil.rmtree(work, ignore_errors=True)


def fmt(v, spec=".2f") -> str:
    return "-" if v is None else format(v, spec)


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--build", default=str(REPO / "build"), help="build directory (default: build/)")
    p.add_argument("--steps", default="8,16,32,64", help="comma-separated bot counts (default 8,16,32,64)")
    p.add_argument("--duration", type=int, default=30, help="seconds of play per step after joining (default 30)")
    p.add_argument("--ramp", type=int, default=20, help="bots connected per second (default 20)")
    p.add_argument("--port", type=int, default=27115, help="UDP port for the test server (default 27115)")
    p.add_argument("--seed", type=int, default=12345, help="world + walk seed (default 12345)")
    p.add_argument("--content-pack", default=str(REPO / "content" / "base"))
    p.add_argument("--idle", action="store_true", help="bots stand still instead of walking")
    p.add_argument("--server-start-timeout", type=float, default=60.0)
    p.add_argument("--json", help="write all rows here as JSON")
    p.add_argument("--keep", action="store_true", help="keep each step's scratch dir (server.log, world)")
    p.add_argument("--verbose", action="store_true", help="print vb_loadtest's own summary per step")
    p.add_argument("--smoke", action="store_true",
                   help="CI mode: pass when every bot joins and the server reported data, "
                        "however slow its ticks (sanitizer builds are)")
    args = p.parse_args()

    build = Path(args.build).resolve()
    server_bin = exe(build, "voxel_browser_server")
    bot_bin = exe(build, "vb_loadtest")
    steps = [int(s) for s in args.steps.split(",") if s.strip()]
    if args.duration < 12:
        print("loadtest: note: the server's status window is 5 s; --duration under ~12 s may give no data",
              file=sys.stderr)

    header = (f"{'bots':>5} {'joined':>6} {'srv tick avg':>12} {'srv tick max':>12} {'tick rate':>9} "
              f"{'rx/bot KiB/s':>12} {'rx total':>9} {'rtt p50':>8}  verdict")
    print(header)
    print("-" * len(header))
    rows = []
    for n in steps:
        row = run_step(args, n, server_bin, bot_bin)
        rows.append(row)
        print(f"{n:>5} {row['joined']:>6} {fmt(row['server_tick_ms_avg']):>9} ms {fmt(row['server_tick_ms_max']):>9} ms "
              f"{fmt(row['tick_rate'], '.1f'):>5}/{row['target_tick_rate']:<3} {fmt(row['rx_kib_s_per_bot']):>12} "
              f"{fmt(row['rx_kib_s_total'], '.0f'):>9} {fmt(row['rtt_ms_p50'], '.1f'):>5} ms  {row['verdict']}",
              flush=True)
        for reason, count in row["failures"].items():
            print(f"        {count} x {reason}")

    if args.json:
        Path(args.json).write_text(json.dumps(rows, indent=2) + "\n")
    if args.smoke:
        return 0 if all(r["joined"] == r["bots"] and r["status_windows"] > 0 for r in rows) else 1
    return 0 if all(r["verdict"].startswith("OK") for r in rows) else 1


if __name__ == "__main__":
    sys.exit(main())
