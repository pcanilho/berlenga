#!/usr/bin/env bash
# Copies optional art from the private art repository into art/ (gitignored, never committed).
# Usage: GITEA_LFS_TOKEN=... bash fetch_art.sh [commit]
set -euo pipefail
: "${GITEA_LFS_TOKEN:?GITEA_LFS_TOKEN is not set in this environment}"
REV="${1:-e9c82fe}"
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="${ART_CACHE:-/tmp/bench-art-src}"
AUTH=(-c credential.helper= -c 'credential.https://gitea.canilho.net.helper=!f() { echo username=oauth2; echo "password=$GITEA_LFS_TOKEN"; }; f')
# path in the art repository -> folder under art/
PARTS=("vehicles/offroad_buggy/godot:offroad_buggy" "props/old_wooden_hut/godot:old_wooden_hut" "terrain/layers:terrain_layers")
if [ ! -d "$SRC/.git" ]; then
	GIT_LFS_SKIP_SMUDGE=1 git "${AUTH[@]}" clone -q --no-checkout https://gitea.canilho.net/gitea_admin/echoes-of-silence-lfs.git "$SRC"
fi
cd "$SRC"
# The helper reads the token from the environment at run time; the token itself is never written to disk.
git config --local credential.helper ""
git config --local --add credential.https://gitea.canilho.net.helper '!f() { echo username=oauth2; echo "password=$GITEA_LFS_TOKEN"; }; f'
git lfs install --local >/dev/null
git sparse-checkout set --no-cone $(for p in "${PARTS[@]}"; do echo "/${p%%:*}/"; done)
GIT_LFS_SKIP_SMUDGE=1 git "${AUTH[@]}" fetch -q origin
GIT_LFS_SKIP_SMUDGE=1 git checkout -q "$REV"
INC=$(for p in "${PARTS[@]}"; do printf '%s/**,' "${p%%:*}"; done)
git "${AUTH[@]}" lfs pull --include="${INC%,}"
mkdir -p "$HERE/art"
for p in "${PARTS[@]}"; do
	rm -rf "$HERE/art/${p##*:}"
	cp -r "$SRC/${p%%:*}" "$HERE/art/${p##*:}"
done
# The ground scans are read from their files by the benches (no import): keep Godot out of that folder.
[ -d "$HERE/art/terrain_layers" ] && touch "$HERE/art/terrain_layers/.gdignore"
# 8K textures import lossless (compress/mode=0: VRAM compression of 8K hangs the cloud import), capped at 4K.
find "$HERE/art" -path "$HERE/art/terrain_layers" -prune -o -type f \( -iname '*.png' -o -iname '*.jpg' -o -iname '*.jpeg' -o -iname '*.webp' \) -print | while read -r t; do
	[ -f "$t.import" ] && grep -q '^compress/mode=0' "$t.import" && continue
	printf '[remap]\n\nimporter="texture"\ntype="CompressedTexture2D"\n\n[params]\n\ncompress/mode=0\nmipmaps/generate=true\nprocess/size_limit=4096\n' > "$t.import"
done
echo "art at $REV: $(cd "$HERE/art" && du -sh . | cut -f1)"
