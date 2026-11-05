#!/usr/bin/env python3
"""CHANGELOG.md for the release workflow.

  changelog.py notes <file>              print the Unreleased entries of <file>
  changelog.py move <released> <tag> <date>
                                          in ./CHANGELOG.md, move the Unreleased entries
                                          that <released> (the changelog as built) had
                                          under a new "## <tag> (<date>)" heading

Entries are grouped by "### <kind>" headings under "## Unreleased". Entries added
after the release's commit stay under Unreleased.
"""

import re
import sys

UNRELEASED = "## Unreleased"


def split(text):
    """(head, unreleased body lines, rest) of a changelog."""
    lines = text.split("\n")
    start = lines.index(UNRELEASED)
    end = len(lines)
    for i in range(start + 1, len(lines)):
        if lines[i].startswith("## "):
            end = i
            break
    return lines[: start + 1], lines[start + 1 : end], lines[end:]


def entries(body):
    """{kind: [entry lines]} of an Unreleased body, kinds in order."""
    out = {}
    kind = None
    for line in body:
        m = re.match(r"^### (.+)$", line)
        if m:
            kind = m.group(1).strip()
            out.setdefault(kind, [])
        elif line.startswith("- ") and kind:
            out[kind].append(line)
    return {k: v for k, v in out.items() if v}


def render(groups):
    lines = []
    for kind, items in groups.items():
        lines += ["", f"### {kind}"] + items
    return lines


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "notes":
        with open(sys.argv[2], encoding="utf-8") as f:
            groups = entries(split(f.read())[1])
        print("\n".join(render(groups)).strip() or "No changelog entries.")
        return
    if len(sys.argv) == 5 and sys.argv[1] == "move":
        with open(sys.argv[2], encoding="utf-8") as f:
            released = entries(split(f.read())[1])
        with open("CHANGELOG.md", encoding="utf-8") as f:
            head, body, rest = split(f.read())
        if not released:
            print("Nothing to move.")
            return
        done = {line for items in released.values() for line in items}
        remaining = entries(body)
        remaining = {k: [e for e in v if e not in done] for k, v in remaining.items()}
        remaining = {k: v for k, v in remaining.items() if v}
        section = ["", f"## {sys.argv[3]} ({sys.argv[4]})"] + render(released)
        text = "\n".join(head + render(remaining) + section + [""] + rest)
        text = re.sub(r"\n{3,}", "\n\n", text)
        with open("CHANGELOG.md", "w", encoding="utf-8") as f:
            f.write(text)
        return
    sys.exit(__doc__)


if __name__ == "__main__":
    main()
