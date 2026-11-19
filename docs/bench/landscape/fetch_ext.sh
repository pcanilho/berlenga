#!/usr/bin/env bash
# Downloads the CC0 Poly Haven models the fire shots use into ext/ (gitignored: binaries stay out of git).
# Credits (polyhaven.com, CC0): dry_branches_medium_01, dead_tree_trunk, dead_tree_trunk_02, tree_stump_01.
set -euo pipefail
cd "$(dirname "$0")"
api=https://api.polyhaven.com/files
for m in dry_branches_medium_01 dead_tree_trunk dead_tree_trunk_02 tree_stump_01; do
	mkdir -p "ext/$m/textures"
	curl -sS "$api/$m" | python3 -c '
import json, sys
g = json.load(sys.stdin)["gltf"]["1k"]["gltf"]
print(sys.argv[1] + "_1k.gltf", g["url"])
for k, v in g["include"].items():
    print(k, v["url"])' "$m" | while read -r f u; do curl -sSf -o "ext/$m/$f" "$u"; done
done
