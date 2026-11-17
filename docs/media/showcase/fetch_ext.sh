#!/usr/bin/env bash
# Downloads the CC0 Poly Haven assets the fluid shot uses into ext/ (not committed: binaries stay out of git).
# Credits: wooden_bucket_01 (James Ray Cock), large_grey_tiles, plastered_stone_wall, metal_plate; polyhaven.com, CC0.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p ext/bucket/textures
api=https://api.polyhaven.com/files
curl -sS "$api/wooden_bucket_01" | python3 -c '
import json, sys
g = json.load(sys.stdin)["gltf"]["1k"]["gltf"]
print("wooden_bucket_01_1k.gltf", g["url"])
for k, v in g["include"].items():
    print(k, v["url"])' | while read -r f u; do curl -sSf -o "ext/bucket/$f" "$u"; done
for t in large_grey_tiles plastered_stone_wall metal_plate; do
	mkdir -p "ext/$t"
	curl -sS "$api/$t" | python3 -c '
import json, sys
d = json.load(sys.stdin)
for k in ["Diffuse", "nor_gl", "Rough", "AO"]:
    print(k, d[k]["1k"]["jpg"]["url"])' | while read -r k u; do curl -sSf -o "ext/$t/$k.jpg" "$u"; done
done
