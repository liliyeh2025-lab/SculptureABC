#!/usr/bin/env python3
# SculptForge - open source digital sculpting
# SPDX-License-Identifier: GPL-3.0-or-later
"""Regenerates the keyboard table in docs/guide/shortcuts.md from the app's action registry.

    tools/guide/update_shortcuts.py --app build/sculptforge          # rewrite the table
    tools/guide/update_shortcuts.py --app build/sculptforge --check  # fail if it is out of date

The app writes the table itself (UI test step `dump_shortcuts`), so the guide lists exactly the
shortcuts the Controls window (F1) shows. Needs a display (or Xvfb), like the UI tests.
"""
import argparse
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
PAGE = ROOT / "docs" / "guide" / "shortcuts.md"
BEGIN = "<!-- BEGIN GENERATED: shortcuts -->"
END = "<!-- END GENERATED: shortcuts -->"


def generate(app: str) -> str:
    with tempfile.TemporaryDirectory() as out:
        script = ROOT / "tools" / "guide" / "dump_shortcuts.sfui"
        run = subprocess.run([app, "--ui-test", str(script), "--ui-out", out], capture_output=True, text=True)
        table = pathlib.Path(out) / "shortcuts-table.md"
        if run.returncode != 0 or not table.exists():
            sys.exit(f"{app} --ui-test failed:\n{run.stdout}{run.stderr}")
        return table.read_text(encoding="utf-8").strip() + "\n"


def splice(page: str, table: str) -> str:
    if BEGIN not in page or END not in page:
        sys.exit(f"{PAGE}: the {BEGIN} / {END} markers are missing")
    head, rest = page.split(BEGIN, 1)
    _, tail = rest.split(END, 1)
    return f"{head}{BEGIN}\n\n{table}\n{END}{tail}"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--app", required=True, help="path to the sculptforge binary")
    ap.add_argument("--check", action="store_true", help="only check that the table is current")
    args = ap.parse_args()
    page = PAGE.read_text(encoding="utf-8")
    new = splice(page, generate(args.app))
    if args.check:
        if new != page:
            print(f"{PAGE} is out of date: run tools/guide/update_shortcuts.py --app {args.app}")
            return 1
        print("shortcuts table is current")
        return 0
    if new != page:
        PAGE.write_text(new, encoding="utf-8")
        print(f"updated {PAGE}")
    else:
        print("shortcuts table already current")
    return 0


if __name__ == "__main__":
    sys.exit(main())
