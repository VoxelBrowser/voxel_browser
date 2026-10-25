## Phase 12 — Main menu redesign (search-engine home page)

> Full plan for this phase; linked from `REMAINING_TASKS.md`. Design (what
> the screens look like and why): `architecture_spec/rendering.md` §11.6.

> User-requested (2026-10-25): make the main menu look like a search engine's
> home page — a "Voxel Browser" title, an address bar with Continue beside
> it, a small recent-servers box below, Settings bottom-left — on a dirt
> texture backdrop that also covers the connecting screen. Player name moves
> into Settings as the default player name; full server history lives in
> Settings › History; Singleplayer and Quit buttons go.
>
> Approved mockup (design canvas, private to the owner):
> https://claude.ai/artifact/CNjU94trHxZDstHCtNQNRy — artboards: main menu
> 1280×720, connecting, Settings › General, Settings › History, main menu at
> the 720×360 minimum.
>
> Decisions taken with the user (2026-10-25):
> - Clicking a recent server opens **Settings › History** with that entry
>   highlighted; it neither connects nor fills the address field.
> - The backdrop goes on **every** non-gameplay screen, not just connecting.
> - Embed Pixelify Sans (title) + Inter (UI), both OFL, ~300 KB, rather than
>   scaling raylib's default font.
> - Singleplayer button removal follows Phase 11.2 option (a): only the
>   button goes; `menu.singleplayer` automation stays.

- [ ] 12.1 **Backdrop and embedded assets.** Copy `content/base/textures/dirt.png`
      to an engine resource dir (e.g. `src/render/resources/`) with the two
      font files and their OFL texts; bake them in with `vb_embed_files`
      (`cmake/EmbedFiles.cmake` — today it emits `vb::cli::kEmbeddedFiles`,
      so parameterise the namespace/symbol or add a render variant). Add
      `MainMenu::draw_backdrop()` (tiled 4×, `TEXTURE_FILTER_POINT`, 55% black
      scrim) and a panel style helper (`#140E0A` at ~85%, 1 px 14%-white
      border, rounded with `DrawRectangleRounded`); call it from every
      `draw_*`. Load fonts once with `LoadFontFromMemory`, `GuiSetFont`; keep
      the title font separate. Ship the licence texts with release archives.
- [ ] 12.2 **Main screen.** Rewrite `draw_main`: title, single address field
      + Continue (Enter submits), recent panel (first 3 of `recent_servers`,
      hidden when empty) + "All history in Settings" link, Settings
      bottom-left, signed-in chip top-right (bottom-right when the title
      would overlap, i.e. at the 720×360 minimum). Vertically centre the
      stack against `GetScreenHeight()` so it fits 720×360.
      `MainResult`: drop `singleplayer`, `quit`; add `open_history` +
      `history_index` (-1 for the link). Address parsing (`host`,
      `host:port`, `[v6]:port`, default 27015, reject port 0/>65535 inline
      under the field) goes in a pure function with a unit test. Drops the
      `GuiListView`-with-`nullptr`-items code that left "Recent servers"
      rendering empty.
- [ ] 12.3 **Settings sections.** Section list (General / Video / Controls /
      History) + Back/Save footer. General: **Default player name**
      (validated non-empty, same limits as the handshake) + asset cache.
      Video and Controls: the existing fields; Controls links to the
      keybindings screen. History: all entries with Connect / Remove and
      Clear history; `open_settings(config, section, highlight)` opens it on
      a given section/entry. Raise the history cap from 8 to 50
      (`kMaxServerHistory` in `vb/core/config.hpp`, used by
      `ClientApp`'s join path).
- [ ] 12.4 **Player name ownership.** Settings › General is the only writer of
      `client.toml` `player_name`; remove `config.player_name =
      menu.player_name()` from the join path (`client_app.cpp`).
      `--name`, `menu.set_name` and `menu.connect{name}` become a session
      override (in memory, never saved). `MainMenu::player_name()` /
      `prefill(…, name)` move to `ClientApp`.
- [ ] 12.5 **Remove Singleplayer and Quit.** Delete both buttons and the
      `result.singleplayer` / `result.quit` branches in `ClientApp::frame`
      (and the `client: exited from menu` line — nothing greps it). Keep
      `menu.singleplayer` → `ClientApp::menu_singleplayer` for automation
      (Phase 11.2 (a)). Closing the window still exits cleanly via
      `WindowShouldClose`.
- [ ] 12.6 **Tests.** `tests/e2e/test_windowed.py`:
      `test_connecting_panel_fits_on_screen` compares the last pixel row to
      the flat clear colour `(18,18,22)` — with the backdrop, compare it to
      the same row of a menu screenshot taken just before connecting
      instead (the backdrop is identical on both screens). Menu screenshot
      still 720×360, `distinct_colors` check still holds. Add a windowed test
      that a long history shows only 3 recent entries (count via a new
      read-only `ui` field or the automation `state` — pick whichever is
      smaller), plus unit tests for the address parser and the history cap.
- [ ] 12.7 **Docs.** `README.md` (menu description, screenshots if any),
      `docs/automation-protocol.md` (`menu.set_name` now session-only),
      `docs/e2e-automation.md` §11 checklist, `STATE.md`, and the stale
      "spec §5.3" reference in `inc/vb/render/main_menu.hpp` → §11.6.

Out of scope: a server browser / master list (Deferred), favourites, server
icons or MOTD in the recent list (would need a ping protocol), animated
backdrop.
