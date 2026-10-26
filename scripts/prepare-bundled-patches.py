#!/usr/bin/env python3
"""Prepare patch XML files to ship inside the AppImage.

    python3 prepare-bundled-patches.py bundled-patches shadPS4 ~/.local/share/shadPS4/patches/shadPS4/Driveclub.xml
    python3 prepare-bundled-patches.py bundled-patches driveclub-720p Driveclub-720p-internal.xml --title-id CUSA00093

For each XML file it
  * copies it to OUT_DIR/NAME/ with every patch switched OFF (isEnabled="false"), so that nothing
    changes until the user turns it on in the launcher (use --keep-enabled to leave the file as it is);
    patches marked Always="true" are never touched,
  * adds the file to OUT_DIR/NAME/files.json with the title ids found in its <TitleID> block (or the
    ones given with --title-id),
  * rewrites OUT_DIR/CREDITS.txt with the authors named in every XML below OUT_DIR.

The patch files are other people's work. Before you ship them, check that you may redistribute them
and keep CREDITS.txt with them. Nothing is changed in the original files.
"""
import argparse
import json
import pathlib
import re
import sys

TAG = re.compile(r'<Metadata(?:\s+[\w:\-]+\s*=\s*(?:"[^"]*"|\'[^\']*\'))*\s*/?>')
ATTR = re.compile(r'(\s)isEnabled\s*=\s*(?:"[^"]*"|\'[^\']*\')')
TITLE_IDS = re.compile(r"<ID>\s*(CUSA\d{5})\s*</ID>")
SUPPORTED_TYPES = {"byte", "bytes", "bytes16", "bytes32", "bytes64", "float32", "float64", "utf8", "utf16",
                   "mask", "mask_jump32"}
OVERLAP = ("60 fps", "resolution patch", "internal resolution", "output resolution", "memory for up to")


def switch_off(text):
    """Set isEnabled="false" on every <Metadata> that is not Always="true"."""
    def fix(match):
        tag = match.group(0)
        if re.search(r'Always\s*=\s*"true"', tag):
            return tag
        if ATTR.search(tag):
            return ATTR.sub(r'\1isEnabled="false"', tag)
        end = "/>" if tag.endswith("/>") else ">"
        return tag[: -len(end)].rstrip() + ' isEnabled="false"' + end
    return TAG.sub(fix, text)


def attribute(tag, name):
    found = re.search(r'\b%s\s*=\s*(?:"([^"]*)"|\'([^\']*)\')' % name, tag)
    return (found.group(1) or found.group(2) or "").strip() if found else ""


def credits(out_dir):
    lines = ["Credits for the patch files bundled with this build", "",
             "The patches below were made by other people and are shipped with their names, as listed in",
             "the Author field of each file. Please keep this file with them.", ""]
    for xml in sorted(out_dir.glob("*/*.xml")):
        authors = {}
        for tag in TAG.findall(xml.read_text(encoding="utf-8", errors="replace")):
            for author in re.split(r"\s*(?:,|&| and )\s*", attribute(tag, "Author")):
                if author:
                    authors.setdefault(author, []).append(attribute(tag, "Name"))
        lines.append("%s/%s" % (xml.parent.name, xml.name))
        lines += ["  %s: %s" % (a, "; ".join(n for n in names if n)) for a, names in sorted(authors.items())] or ["  (no author listed)"]
        lines.append("")
    (out_dir / "CREDITS.txt").write_text("\n".join(lines), encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description="Prepare patch XML files to ship inside the AppImage.")
    parser.add_argument("out_dir")
    parser.add_argument("name", help="folder name inside out_dir (for example shadPS4)")
    parser.add_argument("xml", nargs="+")
    parser.add_argument("--title-id", action="append", default=[], help="title id when the XML has none")
    parser.add_argument("--keep-enabled", action="store_true", help="do not switch the patches off")
    args = parser.parse_args()

    out = pathlib.Path(args.out_dir) / args.name
    out.mkdir(parents=True, exist_ok=True)
    index_path = out / "files.json"
    index = json.loads(index_path.read_text()) if index_path.exists() else {}
    for source in map(pathlib.Path, args.xml):
        text = source.read_text(encoding="utf-8", errors="replace")
        ids = sorted(set(TITLE_IDS.findall(text))) or args.title_id
        if not ids:
            sys.exit("%s has no <TitleID> block: give the title ids with --title-id" % source.name)
        tags = TAG.findall(text)
        types = set(re.findall(r'<Line\b[^>]*\bType\s*=\s*"([^"]+)"', text))
        names = [attribute(t, "Name") for t in tags]
        (out / source.name).write_text(text if args.keep_enabled else switch_off(text), encoding="utf-8")
        index[source.name] = ids
        print("%s: %d patch(es), title ids %s" % (source.name, len(tags), ", ".join(ids)))
        unknown = sorted(types - SUPPORTED_TYPES)
        if unknown:
            print("  warning: Line types the emulator does not apply: %s" % ", ".join(unknown))
        overlap = [n for n in names if any(k in n.lower() for k in OVERLAP)]
        if overlap:
            print("  note: these overlap with features this build already has; leave them off unless you know why:")
            for n in overlap:
                print("    - %s" % n)
    index_path.write_text(json.dumps(index, indent=2) + "\n", encoding="utf-8")
    credits(pathlib.Path(args.out_dir))
    print("\nWrote %s, files.json and %s" % (out, pathlib.Path(args.out_dir) / "CREDITS.txt"))
    print("Check that you may redistribute these files before shipping them.")


if __name__ == "__main__":
    main()
