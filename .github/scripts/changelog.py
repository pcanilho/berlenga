#!/usr/bin/env python3
"""CHANGELOG.md and changelog.d/ for the release workflow.

  changelog.py notes <changelog> [<fragdir>]
        print the unreleased entries: those under "## Unreleased" in <changelog>
        plus every fragment in <fragdir>
  changelog.py check <fragment>
        exit 1 unless <fragment> holds at least one entry under a "### <kind>" heading
  changelog.py move <released-changelog> <tag> <date> [<released-fragdir>]
        in ./CHANGELOG.md, put the entries the release was built with (Unreleased of
        <released-changelog> plus the fragments in <released-fragdir>) under a new
        "## <tag> (<date>)" heading, and delete those fragments from ./changelog.d

A pull request adds its entry as its own file, changelog.d/<name>.md:

    ### Fixed
    - What changes for someone using the engine, in one line (#123).

so two pull requests never edit the same lines. Entries written straight under
"## Unreleased" in CHANGELOG.md still work. Entries added after the release's commit
stay unreleased.
"""

import os
import re
import sys

UNRELEASED = "## Unreleased"
FRAGMENTS = "changelog.d"


def split(text):
    """(head, unreleased body lines, rest) of a changelog."""
    lines = text.split("\n")
    if UNRELEASED not in lines:
        return lines, [], []
    start = lines.index(UNRELEASED)
    end = len(lines)
    for i in range(start + 1, len(lines)):
        if lines[i].startswith("## "):
            end = i
            break
    return lines[: start + 1], lines[start + 1 : end], lines[end:]


def entries(body):
    """{kind: [entry lines]} of an Unreleased body or a fragment, kinds in order."""
    out = {}
    kind = None
    for line in body:
        m = re.match(r"^### (.+)$", line)
        if m:
            kind = m.group(1).strip()
            out.setdefault(kind, [])
        elif line.startswith("- ") and kind:
            out[kind].append(line.rstrip())
    return {k: v for k, v in out.items() if v}


def merge(*groups):
    out = {}
    for g in groups:
        for kind, items in g.items():
            bucket = out.setdefault(kind, [])
            bucket += [e for e in items if e not in bucket]
    return out


def fragment_files(fragdir):
    if not fragdir or not os.path.isdir(fragdir):
        return []
    return sorted(
        f for f in os.listdir(fragdir) if f.endswith(".md") and f.lower() != "readme.md"
    )


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def fragments(fragdir):
    return merge(*(entries(read(os.path.join(fragdir, f)).split("\n")) for f in fragment_files(fragdir)))


def unreleased(changelog, fragdir=None):
    return merge(entries(split(read(changelog))[1]), fragments(fragdir) if fragdir else {})


def render(groups):
    lines = []
    for kind, items in groups.items():
        lines += ["", f"### {kind}"] + items
    return lines


def main():
    args = sys.argv[1:]
    if args[:1] == ["notes"] and len(args) in (2, 3):
        groups = unreleased(args[1], args[2] if len(args) == 3 else None)
        print("\n".join(render(groups)).strip() or "No changelog entries.")
        return
    if args[:1] == ["check"] and len(args) == 2:
        sys.exit(0 if entries(read(args[1]).split("\n")) else 1)
    if args[:1] == ["move"] and len(args) in (4, 5):
        released_dir = args[4] if len(args) == 5 else None
        released = unreleased(args[1], released_dir)
        head, body, rest = split(read("CHANGELOG.md"))
        for f in fragment_files(released_dir):
            if os.path.exists(os.path.join(FRAGMENTS, f)):
                os.remove(os.path.join(FRAGMENTS, f))
        if not released:
            print("Nothing to move.")
            return
        if any(line.startswith(f"## {args[2]} ") for line in rest):
            print(f"{args[2]} is already in CHANGELOG.md.")
            return
        done = {line for items in released.values() for line in items}
        remaining = {k: [e for e in v if e not in done] for k, v in entries(body).items()}
        remaining = {k: v for k, v in remaining.items() if v}
        section = ["", f"## {args[2]} ({args[3]})"] + render(released)
        text = "\n".join(head + render(remaining) + section + [""] + rest)
        text = re.sub(r"\n{3,}", "\n\n", text)
        with open("CHANGELOG.md", "w", encoding="utf-8") as f:
            f.write(text)
        return
    sys.exit(__doc__)


if __name__ == "__main__":
    main()
