#!/usr/bin/env python3
# SculptForge - open source digital sculpting
# SPDX-License-Identifier: GPL-3.0-or-later
"""Copy the brush gallery into the website and write its cards into brushes.html.

    python3 sync_website.py [gallery folder] [website folder]

The site bundles a snapshot (website/gallery/: index.json, brushes, previews)
so brushes.html works without JavaScript and before the gallery repository is
published; in the browser it switches to the live index when it can reach it.
Run after build_index.py. Standard library only.
"""

import html
import json
import re
import shutil
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent


def card(b, base):
    """The same markup as the card() function in brushes.html."""
    e = lambda s: html.escape(str(s), quote=True)
    tags = "".join(f"<span>{e(t)}</span>" for t in b["tags"])
    name = b["file"].rsplit("/", 1)[-1]
    return (f'          <article class="brush" data-tags="{e(" ".join(b["tags"]))}">'
            f'<img src="{e(base + b["preview"])}" alt="{e(b["name"])} brush preview" width="512" height="384" loading="lazy">'
            f'<div class="brush-body"><h3>{e(b["name"])}</h3><p class="by">by {e(b["author"])}</p>'
            f'<p class="desc">{e(b["description"])}</p><p class="brush-tags">{tags}</p>'
            f'<a class="btn small" href="{e(base + b["file"])}" download="{e(name)}" title="Download the .sfbrush file">Download</a></div></article>')


def main():
    gallery = Path(sys.argv[1]) if len(sys.argv) > 1 else HERE / "repo"
    website = Path(sys.argv[2]) if len(sys.argv) > 2 else HERE.parents[1] / "website"
    index = json.loads((gallery / "index.json").read_text(encoding="utf-8"))

    out = website / "gallery"
    if out.exists():
        shutil.rmtree(out)
    for sub in ("brushes", "previews"):
        (out / sub).mkdir(parents=True)
    for b in index["brushes"]:
        shutil.copy2(gallery / b["file"], out / b["file"])
        shutil.copy2(gallery / b["preview"], out / b["preview"])
    shutil.copy2(gallery / "index.json", out / "index.json")

    page = website / "brushes.html"
    text = page.read_text(encoding="utf-8")
    cards = "\n".join(card(b, "gallery/") for b in index["brushes"])
    text, n = re.subn(r"(<!-- brushes:start[^>]*-->\n).*?(<!-- brushes:end -->)",
                      lambda m: m.group(1) + cards + "\n" + m.group(2), text, flags=re.S)
    if n != 1:
        sys.exit("error: brushes.html has no <!-- brushes:start --> ... <!-- brushes:end --> block")
    text = re.sub(r"<!-- brushes:count -->.*?<!-- /brushes:count -->",
                  f"<!-- brushes:count -->{len(index['brushes'])} brushes<!-- /brushes:count -->", text)
    page.write_text(text, encoding="utf-8")
    print(f"{len(index['brushes'])} brushes -> {out} and {page}")


if __name__ == "__main__":
    main()
