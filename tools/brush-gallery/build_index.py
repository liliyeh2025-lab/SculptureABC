#!/usr/bin/env python3
# SculptForge - open source digital sculpting
# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate the community brush gallery and write its index.json.

    python3 build_index.py repo                  # validate every brush, write repo/index.json
    python3 build_index.py repo --validate-only  # pull requests: validate, write nothing
    python3 build_index.py repo --check          # fail if index.json is out of date

Every brushes/<name>.sfbrush must be plain brush data: a JSON object with only
recipe parameters and gallery metadata, values in range, named after its file,
with a previews/<name>.png next to it. Each brush is round-tripped through
`sculptforge-cli` (brush_load + brush_save), so the app itself decides whether
it is valid; anything it would clamp or drop is reported. The index lists
{name, description, author, tags, file, preview, sha256, added} per brush
(docs/brushes.md in the SculptForge repo describes the format). Standard
library only.
"""

import argparse
import datetime
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

INDEX_FORMAT = "sculptforge-brush-gallery"
INDEX_VERSION = 1
MAX_BRUSH_BYTES = 16 * 1024
MAX_PREVIEW_BYTES = 512 * 1024
PREVIEW_SIZE = (512, 384)
META_KEYS = {"name", "description", "author", "tags", "format", "version"}
TAG_RE = re.compile(r"^[a-z0-9][a-z0-9-]{0,23}$")


def slug(name):
    """File name for a brush name: "Dragon Scales" -> "dragon-scales"."""
    return re.sub(r"[^a-z0-9]+", "-", name.lower()).strip("-")


def find_cli(explicit):
    candidates = [explicit, os.environ.get("SCULPTFORGE_CLI")]
    root = Path(__file__).resolve().parents[2]
    for build in ("build", "build-release", "out/build"):
        candidates += [root / build / "sculptforge-cli", root / build / "sculptforge-cli.exe",
                       root / build / "Release" / "sculptforge-cli.exe"]
    candidates.append(shutil.which("sculptforge-cli"))
    for c in candidates:
        if c and Path(c).is_file():
            return str(c)
    sys.exit("error: cannot find sculptforge-cli; pass --cli PATH or set SCULPTFORGE_CLI")


def recipe_params(cli):
    """Recipe parameter names and types, from the app's own brush_create tool schema."""
    out = subprocess.run([cli, "--schema"], capture_output=True, text=True, check=True).stdout
    for tool in json.loads(out):
        if tool["name"] == "brush_create":
            props = tool["input_schema"]["properties"]
            return {k: v["type"] for k, v in props.items() if k not in ("name", "description")}
    sys.exit("error: sculptforge-cli has no brush_create command")


def quote(text):
    return '"' + str(text).replace("\\", "\\\\").replace('"', '\\"') + '"'


def round_trip(cli, path, tmp):
    """The brush as SculptForge loads and saves it, or an error message."""
    saved = Path(tmp) / "roundtrip.sfbrush"
    script = f"brush_load path={quote(path.resolve())}; brush_save path={quote(saved)}"
    r = subprocess.run([cli, "-q", "-c", script], capture_output=True, text=True)
    if r.returncode != 0:
        return None, (r.stderr or r.stdout).strip()
    return json.loads(saved.read_text(encoding="utf-8")), None


def png_size(path):
    data = path.read_bytes()[:24]
    if len(data) < 24 or data[:8] != b"\x89PNG\r\n\x1a\n" or data[12:16] != b"IHDR":
        return None
    return struct.unpack(">II", data[16:24])


def same(a, b):
    if isinstance(a, bool) or isinstance(b, bool):
        return a is b
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        return abs(a - b) <= 0.0005 + 1e-9
    return a == b


def date_added(repo, rel, previous):
    if rel in previous:
        return previous[rel]
    try:
        out = subprocess.run(["git", "-C", str(repo), "log", "--diff-filter=A", "--follow", "--format=%as", "--", rel],
                             capture_output=True, text=True, timeout=30).stdout.split()
        if out:
            return out[-1]
    except (OSError, subprocess.SubprocessError):
        pass
    return datetime.datetime.now(datetime.timezone.utc).date().isoformat()


def validate(repo, path, params, cli, tmp):
    """Returns (entry, errors) for one brush file."""
    errors = []
    rel = path.relative_to(repo).as_posix()
    raw = path.read_bytes()
    if len(raw) > MAX_BRUSH_BYTES:
        return None, [f"{rel}: {len(raw)} bytes, the limit is {MAX_BRUSH_BYTES}"]
    try:
        brush = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, ValueError) as e:
        return None, [f"{rel}: not valid UTF-8 JSON ({e})"]
    if not isinstance(brush, dict):
        return None, [f"{rel}: must be a JSON object"]

    unknown = sorted(set(brush) - set(params) - META_KEYS)
    if unknown:
        errors.append(f"{rel}: unknown keys {', '.join(unknown)} (brushes are recipe data only)")
    if brush.get("format") != "sfbrush" or brush.get("version") != 1:
        errors.append(f'{rel}: needs "format": "sfbrush" and "version": 1')
    name, description, author = brush.get("name"), brush.get("description"), brush.get("author")
    for key, value, limit in (("name", name, 64), ("description", description, 500), ("author", author, 64)):
        if not isinstance(value, str) or not value.strip():
            errors.append(f"{rel}: '{key}' must be a non-empty string")
        elif len(value) > limit:
            errors.append(f"{rel}: '{key}' is longer than {limit} characters")
    tags = brush.get("tags")
    if not isinstance(tags, list) or not 1 <= len(tags) <= 6 or not all(isinstance(t, str) and TAG_RE.match(t) for t in tags):
        errors.append(f"{rel}: 'tags' must be 1-6 lowercase words (a-z, 0-9, '-'), e.g. [\"skin\", \"texture\"]")
    if isinstance(name, str) and name.strip() and path.name != slug(name) + ".sfbrush":
        errors.append(f"{rel}: should be named {slug(name)}.sfbrush after the brush name")

    preview = Path("previews") / (path.stem + ".png")
    preview_path = repo / preview
    if not preview_path.is_file():
        errors.append(f"{rel}: missing preview {preview.as_posix()} (render it with tools/brush-gallery/render_previews.py)")
    else:
        size = png_size(preview_path)
        if size != PREVIEW_SIZE:
            errors.append(f"{preview.as_posix()}: must be a {PREVIEW_SIZE[0]}x{PREVIEW_SIZE[1]} PNG")
        if preview_path.stat().st_size > MAX_PREVIEW_BYTES:
            errors.append(f"{preview.as_posix()}: larger than {MAX_PREVIEW_BYTES // 1024} KB")

    # Let SculptForge itself judge the recipe: anything it clamps or ignores is an error.
    for key, kind in params.items():
        if key not in brush:
            continue
        v = brush[key]
        ok = {"number": isinstance(v, (int, float)) and not isinstance(v, bool), "boolean": isinstance(v, bool),
              "string": isinstance(v, str)}.get(kind, True)
        if not ok:
            errors.append(f"{rel}: '{key}' must be a {kind}")
    if not errors:
        saved, err = round_trip(cli, path, tmp)
        if err:
            errors.append(f"{rel}: SculptForge cannot load it: {err}")
        else:
            for key in list(params) + ["name", "description"]:
                if key in brush and not same(brush[key], saved.get(key)):
                    errors.append(f"{rel}: '{key}' = {json.dumps(brush[key])} is not valid "
                                  f"(SculptForge reads it as {json.dumps(saved.get(key))})")
    if errors:
        return None, errors
    return {
        "name": name,
        "description": description,
        "author": author,
        "tags": tags,
        "file": rel,
        "preview": preview.as_posix(),
        "sha256": hashlib.sha256(raw).hexdigest(),
    }, []


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("repo", nargs="?", default=str(Path(__file__).resolve().parent / "repo"),
                   help="gallery folder (with brushes/ and previews/)")
    p.add_argument("--cli", help="path to sculptforge-cli")
    mode = p.add_mutually_exclusive_group()
    mode.add_argument("--validate-only", action="store_true", help="validate, do not write index.json")
    mode.add_argument("--check", action="store_true", help="fail if index.json is missing or out of date")
    args = p.parse_args()

    repo = Path(args.repo).resolve()
    cli = find_cli(args.cli)
    params = recipe_params(cli)
    index_path = repo / "index.json"
    previous = {}
    if index_path.is_file():
        try:
            previous = {b["file"]: b["added"] for b in json.loads(index_path.read_text(encoding="utf-8"))["brushes"]}
        except (ValueError, KeyError, TypeError):
            pass

    brushes = sorted((repo / "brushes").glob("*.sfbrush"))
    if not brushes:
        sys.exit(f"error: no brushes in {repo / 'brushes'}")
    entries, errors = [], []
    with tempfile.TemporaryDirectory() as tmp:
        for path in brushes:
            entry, errs = validate(repo, path, params, cli, tmp)
            errors += errs
            if entry:
                entry["added"] = date_added(repo, entry["file"], previous)
                entries.append(entry)
    names = {}
    for e in entries:
        other = names.setdefault(e["name"].lower(), e["file"])
        if other != e["file"]:
            errors.append(f"{e['file']}: the name '{e['name']}' is already used by {other}")
    for png in sorted((repo / "previews").glob("*.png")):
        if not (repo / "brushes" / (png.stem + ".sfbrush")).is_file():
            errors.append(f"previews/{png.name}: no brush with this name")
    if errors:
        print("\n".join("error: " + e for e in errors), file=sys.stderr)
        print(f"{len(errors)} problem(s) in {len(brushes)} brushes", file=sys.stderr)
        return 1

    entries.sort(key=lambda e: e["name"].lower())
    text = json.dumps({"format": INDEX_FORMAT, "version": INDEX_VERSION, "brushes": entries}, indent=2,
                      ensure_ascii=False) + "\n"
    if args.validate_only:
        print(f"{len(entries)} brushes OK")
    elif args.check:
        current = index_path.read_text(encoding="utf-8") if index_path.is_file() else ""
        if current != text:
            print("error: index.json is out of date; run tools/brush-gallery/build_index.py", file=sys.stderr)
            return 1
        print(f"{len(entries)} brushes OK, index.json up to date")
    else:
        index_path.write_text(text, encoding="utf-8")
        print(f"{len(entries)} brushes OK, wrote {index_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
