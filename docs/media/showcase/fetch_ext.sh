#!/usr/bin/env bash
# Downloads the CC0 Poly Haven assets the fluid shot uses into ext/ (not committed: binaries stay out of git).
# Credits: wooden_bucket_01 (James Ray Cock), large_grey_tiles, plastered_stone_wall, metal_plate; campfire shot:
# rock_moss_set_01 (Kless Gyzen), dry_branches_medium_01 (Rico Cilliers), forest_ground_04, burned_ground_01;
# polyhaven.com, CC0.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p ext/bucket/textures
api=https://api.polyhaven.com/files
model() { # model id, folder
	mkdir -p "ext/$2/textures"
	curl -sS "$api/$1" | python3 -c '
import json, sys
g = json.load(sys.stdin)["gltf"]["1k"]["gltf"]
print(sys.argv[1] + "_1k.gltf", g["url"])
for k, v in g["include"].items():
    print(k, v["url"])' "$1" | while read -r f u; do curl -sSf -o "ext/$2/$f" "$u"; done
}
model wooden_bucket_01 bucket
model rock_moss_set_01 rocks
model dry_branches_medium_01 branches
for t in large_grey_tiles plastered_stone_wall metal_plate forest_ground_04 burned_ground_01; do
	mkdir -p "ext/$t"
	curl -sS "$api/$t" | python3 -c '
import json, sys
d = json.load(sys.stdin)
for k in ["Diffuse", "nor_gl", "Rough", "AO"]:
    print(k, d[k]["1k"]["jpg"]["url"])' | while read -r k u; do curl -sSf -o "ext/$t/$k.jpg" "$u"; done
done
