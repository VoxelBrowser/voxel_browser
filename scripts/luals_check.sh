#!/usr/bin/env bash
# LuaLS (lua-language-server) --check over the SDK stubs, the shipped packs and the pack templates;
# fails on any warning. Used by CI's lint job (lint.yml `luals`); locally:
#   LUALS=/path/to/bin/lua-language-server scripts/luals_check.sh
# Without $LUALS it downloads the pinned release below (Linux x64) into build/luals/.
set -euo pipefail
LUALS_VERSION=3.18.2
root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

if [ -z "${LUALS:-}" ]; then
  dir="$root/build/luals/$LUALS_VERSION"
  if [ ! -x "$dir/bin/lua-language-server" ]; then
    mkdir -p "$dir"
    curl -fsSL "https://github.com/LuaLS/lua-language-server/releases/download/$LUALS_VERSION/lua-language-server-$LUALS_VERSION-linux-x64.tar.gz" \
      | tar -xz -C "$dir"
  fi
  LUALS="$dir/bin/lua-language-server"
fi
logs="$(mktemp -d)"
tmp="$(mktemp -d)"
trap 'rm -rf "$logs" "$tmp"' EXIT

failed=0
check() { # check <dir> [config]
  local args=(--check "$1" --checklevel=Information --logpath="$logs")
  [ -n "${2:-}" ] && args+=(--configpath="$2")
  echo "== $1"
  local out rc=0
  out="$("$LUALS" "${args[@]}" 2>&1)" || rc=$?
  printf '%s\n' "$out" | tr '\r' '\n' | grep -v -e '^>' -e '^ *$' -e '^Initializing' || true
  [ "$rc" -eq 0 ] || failed=1 # non-zero = problems found (listed above)
}

check sdk/lua
for pack in content/base content/examples/kitchen_sink; do
  check "$pack" "$root/$pack/.luarc.json"
  [ -f "$pack/ui/.luarc.json" ] && check "$pack/ui" "$root/$pack/ui/.luarc.json"
done
# Pack templates (templates/pack/<name> + _common), checked the way `vb pack init` lays them out:
# the stubs in .vb/lua and the configs from sdk/lua/luarc*.template.json.
for t in templates/pack/*/; do
  name="$(basename "$t")"
  [ "$name" = _common ] && continue
  out="$tmp/$name"
  mkdir -p "$out/.vb/lua"
  cp -r templates/pack/_common/. "$t". "$out/"
  cp sdk/lua/library/*.lua "$out/.vb/lua/"
  cp sdk/lua/luarc.template.json "$out/.luarc.json"
  check "$out" "$out/.luarc.json"
  if [ -d "$out/ui" ]; then
    cp sdk/lua/luarc.ui.template.json "$out/ui/.luarc.json"
    check "$out/ui" "$out/ui/.luarc.json"
  fi
done
exit $failed
