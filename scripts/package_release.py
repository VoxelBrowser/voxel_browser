#!/usr/bin/env python3
"""Package one platform x build-type build into installable release zips.

Produces (see architecture_spec/dev-cli.md section 4.1):
  <project>-<version>-<platform>[-debug].zip   the game (client, server, vb, content, ...)
  vb-<version>-<platform>.zip                  just the CLI, release builds only (bootstrap)

Standard library only, so it runs unchanged on every CI runner.
"""
import argparse
import datetime
import os
import re
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BINARIES = ("voxel_browser", "voxel_browser_server", "vb")
# Built only when the renderer and Lua are (VB_BUILD_EDITOR); shipped when present.
OPTIONAL_BINARIES = ("vb_structure_editor",)
EXTRA_FILES = ("server.toml.example", "client.toml.example")
EXEC_ATTR = (0o100755 << 16)
FILE_ATTR = (0o100644 << 16)
# Same set CI stripped before: runtime state a pack wrote while being tested.
RUNTIME_STATE = ("storage.json",)


def git(*args):
    try:
        return subprocess.check_output(["git", *args], cwd=ROOT, text=True,
                                       stderr=subprocess.DEVNULL).strip()
    except (OSError, subprocess.CalledProcessError):
        return ""


def exe_name(name, platform):
    return name + ".exe" if platform.startswith("windows") else name


def content_files(content_dir):
    for path in sorted(content_dir.rglob("*")):
        rel = path.relative_to(content_dir)
        if path.is_dir() or path.is_symlink():
            continue
        if path.name in RUNTIME_STATE or "db" in rel.parts[:-1]:
            continue
        yield path, Path("content") / rel


def tree_files(root_dir, top, suffixes):
    """Files under ROOT/<top> with one of `suffixes`, archived under `top/`."""
    base = root_dir / top
    for path in sorted(base.rglob("*")):
        if path.is_file() and not path.is_symlink() and path.suffix in suffixes:
            yield path, Path(top) / path.relative_to(base)


def build_info(version, platform, build_type):
    commit = git("rev-parse", "HEAD") or "unknown"
    describe = git("describe", "--tags", "--dirty", "--always") or version
    date = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    return (f'version = "{version}"\n'
            f'describe = "{describe}"\n'
            f'commit = "{commit}"\n'
            f'date = "{date}"\n'
            f'build_type = "{build_type}"\n'
            f'platform = "{platform}"\n')


def write_zip(dest, entries):
    """entries: (arcname, source Path | bytes, executable)."""
    names = [e[0] for e in entries]
    assert len(names) == len(set(names)), "duplicate archive entries"
    tmp = dest.with_suffix(".zip.part")
    with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED) as zf:
        for arcname, src, executable in entries:
            info = zipfile.ZipInfo(arcname, date_time=(2020, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = EXEC_ATTR if executable else FILE_ATTR
            zf.writestr(info, src if isinstance(src, bytes) else Path(src).read_bytes())
    os.replace(tmp, dest)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--build-dir", required=True, type=Path)
    ap.add_argument("--out-dir", required=True, type=Path)
    ap.add_argument("--platform", required=True,
                    help="linux-x86_64 | macos-universal | windows-x86_64")
    ap.add_argument("--build-type", required=True, choices=("release", "debug"))
    ap.add_argument("--version", required=True, help="release tag, e.g. v0.2.0")
    ap.add_argument("--project", default=(ROOT / "PROJECT_NAME").read_text().strip())
    args = ap.parse_args()

    if not re.fullmatch(r"v\d+\.\d+\.\d+", args.version):
        sys.exit(f"package_release: version '{args.version}' is not a clean vX.Y.Z tag")

    entries = []
    for name in BINARIES:
        fname = exe_name(name, args.platform)
        src = args.build_dir / fname
        if not src.is_file():
            sys.exit(f"package_release: missing {src}")
        entries.append((fname, src, True))
    for name in OPTIONAL_BINARIES:
        fname = exe_name(name, args.platform)
        if (args.build_dir / fname).is_file():
            entries.append((fname, args.build_dir / fname, True))
    if args.platform.startswith("windows"):
        # vcpkg's dynamic triplet puts the runtime DLLs (protobuf, abseil, ...) next to the exes in
        # the build dir. Without them the installed server dies with STATUS_DLL_NOT_FOUND.
        entries += [(dll.name, dll, False) for dll in sorted(args.build_dir.glob("*.dll"))]
    for fname in EXTRA_FILES:
        entries.append((fname, ROOT / fname, False))
    entries += [(str(arc.as_posix()), src, False)
                for src, arc in content_files(ROOT / "content")]
    # Offline developer docs and Lua stubs (`vb docs`, `vb pack types`): the release carries exactly
    # the docs and API stubs of the engine it ships.
    entries += [(str(arc.as_posix()), src, False)
                for src, arc in tree_files(ROOT, "sdk", {".lua", ".txt", ".json", ".md"})]
    entries += [(str(arc.as_posix()), src, False)
                for src, arc in tree_files(ROOT, "docs", {".md", ".txt"})]
    # The menu fonts are baked into the client; their OFL texts ship with it.
    entries += [("licenses/" + src.name, src, False)
                for src in sorted((ROOT / "src/render/resources").glob("*.txt"))]
    entries.append(("BUILD_INFO.toml",
                    build_info(args.version, args.platform, args.build_type).encode(), False))

    args.out_dir.mkdir(parents=True, exist_ok=True)
    suffix = "-debug" if args.build_type == "debug" else ""
    game = args.out_dir / f"{args.project}-{args.version}-{args.platform}{suffix}.zip"
    write_zip(game, entries)
    print(f"wrote {game} ({game.stat().st_size} bytes)")

    if args.build_type == "release":
        vb_name = exe_name("vb", args.platform)
        cli = args.out_dir / f"vb-{args.version}-{args.platform}.zip"
        write_zip(cli, [(vb_name, args.build_dir / vb_name, True)])
        print(f"wrote {cli} ({cli.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
