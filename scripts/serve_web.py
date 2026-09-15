#!/usr/bin/env python3
"""Static file server for web/ that sets the Cross-Origin-Opener-Policy and
Cross-Origin-Embedder-Policy response headers, with an optional dev-only asset
mounting endpoint.

Why COOP/COEP: re3_wasm.js/.wasm are built with USE_PTHREADS=1 (see
src/CMakeLists.txt). Browsers only allow SharedArrayBuffer -- which Emscripten's
pthread pool needs to hand memory to worker threads -- on a "cross-origin isolated"
page, and a page only becomes cross-origin isolated if the server sends both of
these headers on every response. A plain `python -m http.server` does not; see
docs/BROWSER_RUNTIME.md for what that failure looks like. This is a *hosting*
concern -- any real deployment of web/ needs to set the same two headers.

Usage:
    python scripts/serve_web.py [port] [directory]
    python scripts/serve_web.py [port] [directory] --dev-assets PATH

--dev-assets PATH (task 8, development-only asset mounting):
    Serves a file listing + raw bytes of PATH (e.g. your local gamefiles/) under
    /__dev_assets/, for web/vfs.js's mountFromDevServer() to pull into the running
    module's virtual filesystem at load time. This is for local development only:

      - It is OFF unless you pass this flag explicitly -- there is no default path,
        and nothing under gamefiles/ (or wherever you point it) is served otherwise.
      - It only binds to 127.0.0.1 (see main()), so it's not reachable from other
        machines on your network by default.
      - It serves the directory tree as-is, straight off disk, on every request --
        nothing is cached, copied, packaged, or written into this repository.
      - web/vfs.js additionally only calls mountFromDevServer() when the page is
        loaded with ?devAssets=1 (see launcher.js), so a normal page load never
        touches this endpoint even when the server has it enabled.

    This is *not* how end users are meant to get their assets into the page (that's
    the "user-provided asset directory" folder-picker flow, task 6B) -- it exists so
    a developer working on this repo can point at their own local GTA III install
    without a picker dialog every reload. See docs/GAME_ASSETS.md.
"""
import argparse
import http.server
import json
import os
import posixpath
import sys
import urllib.parse

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_WEB_DIR = os.path.join(REPO_ROOT, "web")
DEV_ASSETS_PREFIX = "/__dev_assets"


def build_dev_assets_index(root):
    """Relative (POSIX-style) path -> absolute on-disk path, for every file under root."""
    index = {}
    for dirpath, _dirnames, filenames in os.walk(root):
        rel_dir = os.path.relpath(dirpath, root)
        for name in filenames:
            rel = name if rel_dir == "." else os.path.join(rel_dir, name)
            index[rel.replace(os.sep, "/")] = os.path.join(dirpath, name)
    return index


def make_handler(web_dir, dev_assets_root):
    dev_index = build_dev_assets_index(dev_assets_root) if dev_assets_root else None
    if dev_assets_root:
        print(f"[dev-assets] serving {len(dev_index)} file(s) from {dev_assets_root} under {DEV_ASSETS_PREFIX}/")
        print("[dev-assets] WARNING: local development only -- never deploy this server/flag as-is.")

    class Handler(http.server.SimpleHTTPRequestHandler):
        def end_headers(self):
            self.send_header("Cross-Origin-Opener-Policy", "same-origin")
            self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
            # SimpleHTTPRequestHandler sends Last-Modified but no Cache-Control,
            # so browsers apply heuristic freshness caching (roughly 10% of the
            # file's Last-Modified age, per RFC 7234) and can keep serving a
            # stale launcher.js/saves.js/etc for minutes after a rebuild --
            # across tabs and even fresh ones, since the HTTP cache is shared
            # per browser profile, not per tab. This is a local dev server, so
            # always force revalidation instead.
            self.send_header("Cache-Control", "no-cache")
            super().end_headers()

        def do_GET(self):
            if dev_index is not None and self.path.startswith(DEV_ASSETS_PREFIX + "/"):
                return self._serve_dev_asset()
            return super().do_GET()

        def _serve_dev_asset(self):
            sub = urllib.parse.unquote(self.path[len(DEV_ASSETS_PREFIX) + 1:])
            if sub == "manifest.json":
                body = json.dumps(sorted(dev_index.keys())).encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return
            if sub.startswith("file/"):
                rel = posixpath.normpath(sub[len("file/"):])
                if rel.startswith("..") or rel not in dev_index:
                    self.send_error(404, f"No such dev asset: {rel}")
                    return
                path = dev_index[rel]
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(os.path.getsize(path)))
                self.end_headers()
                with open(path, "rb") as f:
                    self.wfile.write(f.read())
                return
            self.send_error(404)

    return lambda *args, **kwargs: Handler(*args, directory=web_dir, **kwargs)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port", nargs="?", type=int, default=8080)
    ap.add_argument("directory", nargs="?", default=DEFAULT_WEB_DIR)
    ap.add_argument("--dev-assets", metavar="PATH", default=None,
                     help="serve PATH (e.g. your local gamefiles/) under /__dev_assets/ for web/vfs.js's dev-mount flow (task 8). Off by default.")
    args = ap.parse_args()

    if args.dev_assets and not os.path.isdir(args.dev_assets):
        print(f"error: --dev-assets path does not exist or is not a directory: {args.dev_assets}", file=sys.stderr)
        return 1

    handler = make_handler(os.path.abspath(args.directory), os.path.abspath(args.dev_assets) if args.dev_assets else None)
    with http.server.ThreadingHTTPServer(("127.0.0.1", args.port), handler) as httpd:
        print(f"Serving {args.directory} on http://127.0.0.1:{args.port} (COOP/COEP enabled)")
        httpd.serve_forever()
    return 0


if __name__ == "__main__":
    sys.exit(main())
