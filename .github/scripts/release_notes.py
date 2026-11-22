#!/usr/bin/env python3
"""Release notes for the release workflow.

  release_notes.py <changes.md> <dist dir> <short sha> <owner/repo>

<changes.md> is the changelog's Unreleased entries (changelog.py notes). The dist dir
holds the zips and each build's hashes-*.md (its executables' SHA-256). Prints the notes:
the changes, then one table row per build with its download and the zip's SHA-256.
"""

import glob
import hashlib
import os
import sys

# zip name part -> (build, what it runs)
BUILDS = [
    ("windows", "Windows x86_64", "Vulkan, path tracing, DLSS Ray Reconstruction (NVIDIA DLLs not included, see below)"),
    ("macos-universal", "macOS universal", "Apple Silicon and Intel; raster only"),
    ("linux-x86_64", "Linux x86_64", "Vulkan, path tracing (no DLSS)"),
    ("linux-arm64", "Linux arm64", "Vulkan, path tracing (no DLSS)"),
]


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    changes_path, dist, short, repo = sys.argv[1:]
    with open(changes_path, encoding="utf-8") as f:
        changes = f.read().strip()

    rows = []
    for key, name, runs in BUILDS:
        zips = glob.glob(os.path.join(dist, f"godot-fork-{key}-*.zip"))
        if not zips:
            continue
        zip_name = os.path.basename(zips[0])
        url = f"https://github.com/{repo}/releases/download/build-{short}/{zip_name}"
        rows.append(f"| {name} | {runs} | [{zip_name}]({url}) | `{sha256(zips[0])}` |")

    exe_hashes = []
    for path in sorted(glob.glob(os.path.join(dist, "hashes-*.md"))):
        with open(path, encoding="utf-8") as f:
            for line in f.read().splitlines():
                if line.startswith("- ") and ": " in line:
                    file, digest = line[2:].rsplit(": ", 1)
                    exe_hashes.append(f"| `{file}` | `{digest.strip()}` |")

    out = [changes, "", "## Downloads", "", f"Editor builds from `main` at `{short}`.", ""]
    out += ["| Build | Runs | Download | SHA-256 of the zip |", "| --- | --- | --- | --- |"] + rows
    out += [
        "",
        "- Windows: the DLSS, Streamline and Reflex DLLs are not included. Take them from NVIDIA's Streamline and DLSS SDKs (or the NVIDIA-RTX/godot releases) and place them next to the exe; they stay under NVIDIA's terms.",
    ]
    if any("macOS" in r for r in rows):
        out.append("- macOS: ad-hoc signed, not notarized. On first launch right-click Godot.app and choose Open, or run `xattr -cr Godot.app`.")
    if exe_hashes:
        out += ["", "<details><summary>SHA-256 of the executables</summary>", "", "| File | SHA-256 |", "| --- | --- |"] + exe_hashes + ["", "</details>"]
    print("\n".join(out))


if __name__ == "__main__":
    main()
