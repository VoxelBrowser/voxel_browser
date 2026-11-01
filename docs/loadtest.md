# Server capacity benchmark (`vb_loadtest`)

How many players can one `voxel_browser_server` carry? This benchmark answers
that over real UDP. It connects N bot players to a real server, steps N up,
and reports where the server stops keeping its tick rate and how much
bandwidth it needs.

The deterministic in-process checks live in `tests/unit/capacity_test.cpp`
(the `max_players` cap and a per-tick budget at 16 players). This page covers
the real-network side.

## Pieces

- **`vb_loadtest`** (`src/loadtest/main.cpp`, built next to the server into the
  build root). One process runs N headless bots. Each bot has its own
  `GnsTransport` connection and a real `ClientSession`. A bot joins, then
  sends one movement input per tick: walking forward and turning to a random
  direction every 2–6 s, or standing still with `--idle`. For each bot it
  reports join time, round-trip time and bytes in and out. It also reports how
  long its own tick took; if that nears the tick period, the bot process is
  the bottleneck, and the summary says so.
- **Server `--status-file`**: the file now also includes `tick_ms_avg` and
  `tick_ms_max`, the wall time each tick spent working (not sleeping) over
  the 5 s status window, next to the achieved `tick_rate`.
- **`scripts/loadtest.py`** runs a fresh server for each step (temporary world,
  fixed seed, `--max-players N`), runs `vb_loadtest` against it, and reads the
  status file while all N bots are online. It prints one row per step.

Bots skip asset download (no `ClientAssetCache`), so the bandwidth numbers
cover gameplay traffic only: terrain streaming, entity snapshots and acks.
They don't include a first join's pack download.

## Running it

Build with networking (needs protobuf and OpenSSL; see README's
build-from-source section):

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DVB_WITH_NET=ON -DVB_WITH_LUA=ON -DVB_WITH_COMPRESSION=ON
cmake --build build --target voxel_browser_server vb_loadtest

python3 scripts/loadtest.py --build build --steps 8,16,32,64,128 --duration 30
python3 scripts/loadtest.py --build build --steps 32,64,128 --idle   # players only, little terrain
```

Useful flags: `--json rows.json`, `--keep` (keep each step's `server.log` and
world), `--verbose` (print `vb_loadtest`'s own summary), `--ramp` (bots per
second while connecting). The script exits non-zero if any step fails to get
every bot in or falls behind its tick rate.

To load a server that's already running (on another machine, say), run the
bots directly, in several processes if one gets bot-bound:

```bash
./build/vb_loadtest --host 10.0.0.5 --port 27015 --bots 50 --duration 60
```

The verdict column reads **OK** when every bot joined, the achieved tick rate
was at least 95% of the target, and the average tick work stayed under 80% of
the tick period. **BEHIND** means the server couldn't keep up. **JOIN FAIL**
means some bots never got in (the reasons are listed below the row). A
**(bot-bound)** suffix means the bot process's p95 tick went over 80% of the
period, so that row understates what the server could do.

`ctest -L loadtest` (Linux, `VB_WITH_NET`) runs a 4-bot smoke version in CI.
It checks only that bots join and the server reports stats, never tick speed.

## Results (2026-11-01)

These were measured on a 4-vCPU cloud container (Xeon @ 2.8 GHz), with the
server and all bots sharing those 4 cores over localhost, in a RelWithDebInfo
build with the base pack and default `view_distance = 4`. Treat them as a
rough floor, not a spec: on a dedicated machine with the bots elsewhere, the
server gets all the cores.

Walking bots (they spread out, so the server generates and streams new
terrain):

| bots | joined | srv tick avg | srv tick max | tick rate | rx/bot KiB/s | total KiB/s | verdict |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| 8 | 8 | 4.9 ms | 154 ms | 20.0 | 17 | 138 | OK |
| 16 | 16 | 10.2 ms | 229 ms | 19.8 | 25 | 402 | OK |
| 32 | 32 | 16.7 ms | 543 ms | 19.8 | 38 | 1 227 | OK |
| 64 | 64 | 28.2 ms | 538 ms | 19.8 | 71 | 4 522 | OK (bot-bound) |
| 128 | 122 | – | – | – | 112 | 13 622 | JOIN FAIL (bot-bound): 6 × handshake timeout |

Idle bots (everyone stands at spawn, so there's little terrain work, but
every bot sees every other one):

| bots | joined | srv tick avg | srv tick max | tick rate | rx/bot KiB/s | total KiB/s | verdict |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| 32 | 32 | 5.5 ms | 430 ms | 20.0 | 36 | 1 156 | OK |
| 64 | 64 | 11.6 ms | 874 ms | 20.0 | 67 | 4 263 | OK |
| 128 | 128 | 24.5 ms | 2 509 ms | 20.0 | 183 | 23 451 | OK |

What this shows:

- **CPU:** average tick work grows roughly linearly with player count, at
  about 0.2 ms per idle player and about 0.45 ms per walking player (the extra
  is terrain generation). The 50 ms budget at 20 Hz is reached somewhere past
  64 walking players on this hardware, with the bots competing for the same
  cores.
- **Bandwidth is the first real limit.** Each player receives every nearby
  player's updates, so total server upload grows with N². At 128 players
  crowded together that's about 23 MiB/s (about 190 Mbit/s), which most
  hosting links can't sustain long before CPU runs out. Spread-out players
  cost less per player.
- **Spikes:** `srv tick max` includes the join burst and first-time chunk
  generation. Hundreds of milliseconds there show up as a hitch for everyone
  online when many players join or explore new ground at once. The
  steady-state average is the number to compare with the budget.
- **Joins:** a 128-bot walking run lost 6 bots to `handshake timeout` while
  the shared CPU was saturated. A real server with its own cores, or a slower
  `--ramp`, avoids that, but it marks where joins start failing under load.

Rule of thumb for the default config: **up to about 32 players is
comfortable, and 64 works with headroom for CPU but needs around 35 Mbit/s of
upload.** Beyond that, measure on your own hardware.
