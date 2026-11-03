# SculptureABC - SPDX-License-Identifier: GPL-3.0-or-later
"""Builds the Chinese interface font: Noto Sans TC (SIL OFL 1.1) at weight 400, cut down to the characters the
interface uses (mobile/i18n/*.json), so the web page stays small.

    python mobile/tools/make-font-subset.py            rebuild the font
    python mobile/tools/make-font-subset.py --check    only report (exit 1 if the font lacks characters)

Source: the variable Noto Sans TC (NotoSansTC-VF.ttf, Windows ships it in C:/Windows/Fonts; or Google Fonts'
NotoSansTC[wght].ttf), set with --source. Output: mobile/assets/fonts/NotoSansTC-Subset.ttf plus
NotoSansTC-Subset.chars.txt (the characters it carries). Needs fontTools (pip install fonttools).
Also lists interface strings written as T("...") in mobile/src that the Chinese table lacks."""
import argparse
import glob
import json
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
OUT = os.path.join(ROOT, 'mobile', 'assets', 'fonts', 'NotoSansTC-Subset.ttf')
CHARS = os.path.join(ROOT, 'mobile', 'assets', 'fonts', 'NotoSansTC-Subset.chars.txt')
# Punctuation the code may add on its own, besides what the tables use.
EXTRA = '，。、：；（）！？…／％「」・—'


def table_chars():
    chars = set(EXTRA)
    for path in glob.glob(os.path.join(ROOT, 'mobile', 'i18n', '*.json')):
        with open(path, encoding='utf-8') as f:
            for value in json.load(f).values():
                chars.update(value)
    # Only what Inter (Latin) doesn't cover: everything past Latin-1 and general punctuation.
    return ''.join(sorted(c for c in chars if ord(c) >= 0x2E80 or ord(c) in range(0x3000, 0x3100)))


def missing_keys():
    with open(os.path.join(ROOT, 'mobile', 'i18n', 'zh-TW.json'), encoding='utf-8') as f:
        table = json.load(f)
    keys = set()
    for path in glob.glob(os.path.join(ROOT, 'mobile', 'src', '*.cpp')):
        with open(path, encoding='utf-8') as f:
            text = f.read()
        text = re.sub(r'"\s*\n\s*"', '', text)  # "joined " "literals"
        keys.update(re.findall(r'\bT\("((?:[^"\\]|\\.)*)"\)', text))
        # Brush names and descriptions are passed to T() from a table.
        for m in re.finditer(r'\{ICON_LC_\w+, "([^"]*)", "([^"]*)"\}', text):
            keys.update(m.groups())
    keys = {k.split('##')[0] for k in keys}
    return sorted(k for k in keys if k and k not in table)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--source', default=r'C:/Windows/Fonts/NotoSansTC-VF.ttf')
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--if-needed', action='store_true', help='rebuild only when the characters changed')
    args = ap.parse_args()

    for key in missing_keys():
        print(f'not translated: {key!r}')
    chars = table_chars()
    have = open(CHARS, encoding='utf-8').read() if os.path.exists(CHARS) else ''
    lacking = sorted(set(chars) - set(have)) if os.path.exists(OUT) else sorted(chars)
    if args.check:
        if lacking:
            print(f'the font lacks {len(lacking)} characters: {"".join(lacking[:40])}')
            return 1
        print(f'font ok ({len(have)} characters)')
        return 0
    if args.if_needed and not lacking and have == chars:
        print(f'font up to date ({len(chars)} characters)')
        return 0

    from fontTools.ttLib import TTFont
    from fontTools.varLib import instancer
    from fontTools import subset

    font = TTFont(args.source)
    if 'fvar' in font:
        font = instancer.instantiateVariableFont(font, {'wght': 400}, updateFontNames=False)
    options = subset.Options()
    options.layout_features = ['*']
    options.name_IDs = ['*']
    options.name_languages = ['*']
    options.notdef_outline = True
    options.hinting = False
    sub = subset.Subsetter(options)
    sub.populate(unicodes=[ord(c) for c in chars])
    sub.subset(font)
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    font.save(OUT)
    with open(CHARS, 'w', encoding='utf-8', newline='\n') as f:
        f.write(chars)
    print(f'wrote {OUT}: {len(chars)} characters, {os.path.getsize(OUT) // 1024} KB')
    return 0


if __name__ == '__main__':
    sys.exit(main())
