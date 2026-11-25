#!/usr/bin/env bash
# Editor screenshots for the site: shots.sh <shot> <focus x> <focus y> <focus z> <distance> <pitch> <yaw> [godot args...]
# Writes the editor's view state for world.tscn (where the 3D view looks), then runs shots.gd in the editor under Xvfb.
# Needs the CC0 art (docs/bench/landscape/fetch_art.sh) linked as art/terrain_layers and art/trees_scans.
set -euo pipefail
cd "$(dirname "$0")"
SHOT=$1; FX=$2; FY=$3; FZ=$4; DIST=$5; PITCH=$6; YAW=$7; shift 7
G=${GODOT:-godot}
MD5=$(printf '%s' "res://world.tscn" | md5sum | cut -d' ' -f1)
mkdir -p .godot/editor
cat > ".godot/editor/world.tscn-editstate-$MD5.cfg" <<CFG
[editor_states]

3D={
"show_grid": false,
"show_origin": false,
"viewport_mode": 1,
"viewports": [{
"position": Vector3($FX, $FY, $FZ),
"distance": $DIST,
"x_rotation": $PITCH,
"y_rotation": $YAW,
"orthogonal": false
}]
}
CFG
VK_ICD_FILENAMES=${VK_ICD_FILENAMES:-/usr/share/vulkan/icd.d/lvp_icd.json} xvfb-run -a -s "-screen 0 1920x1080x24" \
	"$G" ${GODOT_ARGS:-} --path . --editor world.tscn --resolution 1920x1080 --position 0,0 -- --shot="$SHOT" "$@"
