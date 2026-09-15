#!/usr/bin/env python3
"""Check whether a local GTA III installation has the files re3 needs.

This is a pre-flight check you run *before* trying to mount your assets into
the browser build -- it never reads or copies the game data itself, only file
*names*, and it never writes anything into this repository. See
docs/GAME_ASSETS.md for the full picture (categories, where the WASM build
looks for assets, how this tool relates to the in-browser validator in
web/vfs.js).

Usage:
    python scripts/validate_assets.py [path-to-your-gta3-install]

Defaults to gamefiles/ (relative to the repo root) if no path is given, which
is also where the native build's CMake install step and this tool's docs
expect a user-supplied installation to live.

Exit code is 0 if every *required* asset was found, 1 otherwise (so this can
be used as a CI/pre-build gate). Optional assets are reported but never affect
the exit code.
"""
import argparse
import json
import os
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_MANIFEST = os.path.join(REPO_ROOT, "web", "asset-manifest.json")
DEFAULT_ASSET_ROOT = os.path.join(REPO_ROOT, "gamefiles")

# CFileLoader::LoadLevel (src/core/FileLoader.cpp) keywords that are followed
# by a single relative path token. Keep in sync with that switch if it changes.
GTA3_DAT_PATH_KEYWORDS = {
    "IDE", "IPL", "COLFILE", "MODELFILE", "HIERFILE", "TEXDICTION", "CDIMAGE",
}


def build_case_insensitive_index(root):
    """Map lowercased relative-path -> actual on-disk relative path, so we can
    check existence the same way CFileMgr/casepath() do (case-insensitively,
    treating '/' and '\\' the same)."""
    index = {}
    for dirpath, _dirnames, filenames in os.walk(root):
        rel_dir = os.path.relpath(dirpath, root)
        for name in filenames:
            rel = name if rel_dir == "." else os.path.join(rel_dir, name)
            key = rel.replace("\\", "/").lower()
            index[key] = rel.replace("\\", "/")
        # also index directories themselves, for manifest entries like "audio"
        if rel_dir != ".":
            index.setdefault(rel_dir.replace("\\", "/").lower(), rel_dir.replace("\\", "/"))
    return index


def exists_ci(index, rel_path):
    return index.get(rel_path.replace("\\", "/").lower())


def parse_gta3_dat(asset_root, index):
    """Best-effort parse of data/gta3.dat to find every IDE/IPL/etc it lists,
    for a more thorough check than the manifest's curated core-file list.
    Returns a list of relative paths (as written in the file); returns []
    if gta3.dat itself couldn't be found/read."""
    real = exists_ci(index, "data/gta3.dat")
    if not real:
        return []
    paths = []
    try:
        with open(os.path.join(asset_root, real), "r", errors="replace") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                parts = line.split(None, 1)
                if len(parts) != 2:
                    continue
                keyword, rest = parts[0].upper(), parts[1].strip()
                if keyword == "COLFILE":
                    # COLFILE <level-number> <path> (src/core/FileLoader.cpp:92-98)
                    # -- the path is the *second* token, not the first.
                    fields = rest.split(None, 1)
                    if len(fields) == 2:
                        paths.append(fields[1].split()[0].replace("\\", "/"))
                elif keyword in GTA3_DAT_PATH_KEYWORDS:
                    # IDE/IPL/MODELFILE/HIERFILE/TEXDICTION/CDIMAGE: path is the
                    # first (and generally only meaningful) token after the keyword.
                    path = rest.split()[0]
                    paths.append(path.replace("\\", "/"))
    except OSError as e:
        print(f"warning: found data/gta3.dat but couldn't read it: {e}", file=sys.stderr)
    return paths


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("asset_root", nargs="?", default=DEFAULT_ASSET_ROOT,
                     help=f"path to your GTA III installation (default: {DEFAULT_ASSET_ROOT})")
    ap.add_argument("--manifest", default=DEFAULT_MANIFEST, help="path to asset-manifest.json")
    ap.add_argument("--skip-gta3dat-scan", action="store_true",
                     help="don't parse data/gta3.dat for the full IDE/IPL map list, just check the curated core files")
    ap.add_argument("--quiet", action="store_true", help="only print missing files and the final summary")
    args = ap.parse_args()

    if not os.path.isdir(args.asset_root):
        print(f"error: asset root does not exist or is not a directory: {args.asset_root}", file=sys.stderr)
        return 1

    with open(args.manifest, "r") as f:
        manifest = json.load(f)

    print(f"Scanning {args.asset_root} ...")
    index = build_case_insensitive_index(args.asset_root)
    print(f"Found {len(index)} files/directories.\n")

    missing_required = []
    missing_optional = []

    def check(entries, bucket):
        for entry in entries:
            path = entry["path"]
            real = exists_ci(index, path)
            if real:
                if not args.quiet:
                    print(f"  ok       {path}")
            else:
                bucket.append(path)
                print(f"  MISSING  Missing game asset: {path}  ({entry.get('why', '')})")

    print("Required game assets:")
    check(manifest["gameAssets"]["required"], missing_required)

    print("\nOptional game assets:")
    check(manifest["gameAssets"]["optional"], missing_optional)

    if not args.skip_gta3dat_scan:
        print("\nFull level manifest (data/gta3.dat) scan:")
        dat_paths = parse_gta3_dat(args.asset_root, index)
        if not dat_paths:
            print("  (data/gta3.dat not found or empty -- skipping; core-file check above already reports it as missing if required)")
        else:
            dat_missing = 0
            for path in dat_paths:
                if not exists_ci(index, path):
                    dat_missing += 1
                    missing_required.append(path)
                    print(f"  MISSING  Missing game asset: {path}  (referenced by data/gta3.dat)")
            if dat_missing == 0:
                print(f"  ok       all {len(dat_paths)} files referenced by data/gta3.dat are present")
            else:
                print(f"  {dat_missing} of {len(dat_paths)} files referenced by data/gta3.dat are missing")

    print("\n" + "=" * 60)
    if missing_required:
        print(f"FAIL: {len(missing_required)} required asset(s) missing, {len(missing_optional)} optional asset(s) missing.")
        return 1
    print(f"OK: all required assets present ({len(missing_optional)} optional asset(s) missing, see above).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
