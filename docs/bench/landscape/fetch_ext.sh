#!/usr/bin/env bash
# Downloads the CC0 Poly Haven models the fire shots use into ext/ (gitignored: binaries stay out of git).
# Credits (polyhaven.com, CC0): dry_branches_medium_01, dead_tree_trunk, dead_tree_trunk_02, tree_stump_01.
# The rigged walker: GDQuest's mannequin (godot-3d-mannequin, CC BY 4.0, "GDQuest and contributors").
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
mkdir -p ext/mannequin
curl -sSf -o ext/mannequin/mannequiny-0.3.0.glb https://raw.githubusercontent.com/GDQuest/godot-3d-mannequin/master/godot/assets/3d/mannequiny/mannequiny-0.3.0.glb
