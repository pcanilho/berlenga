# Ground detail for the forest floor: leaf litter, twigs, small plants

Follows [ground-detail.md](ground-detail.md) (stones of 1 to 8 cm as instanced geometry near the camera, on by
default since 18 Nov). The vegetation system owns the forest floor below 1 cm sticks (trees own sticks of 1 cm and
up); this note applies the ground-detail pattern there, inside VegetationField3D, not as a second placer.

## What is there

- `PlantGenerator` presets `LEAF_LITTER`, `NEEDLE_LITTER`, `TWIGS`, `MOSS`, `FEATHER_MOSS`, `CLOVER`, `SEEDLING`,
  `WOOD_SORREL`, `LIVERWORT` and others, placed by `VegetationField3D` as `PlantSpecies` with rules (crown litter
  under trees, shade, slope, paths, deadwood), on worker threads in streaming tiles.
- Each species has a hand-set `visibility_range` (litter 15 m, twigs 18 m, seedlings 30 m), measured by Godot to the
  centre of the whole MultiMesh (one per tile and mesh variant, 32 m tiles), so a short range drops a tile at once.
- Litter density follows the stand's leaf cover (decay since leaf fall), one litter patch every 0.17 m.

## Established technique

- Dead-leaves model (Matheron 1968; Kendall and Thonnes, Warwick): leaves fall in sequence and occlude the ones
  under them; the top layer is what shows. Same sequential deposition the rock field uses (Visscher and Bolsterli),
  so a litter class can reuse it: largest first, each leaf resting on what is down.
- Environmental objects (Grosbellet, Peytavie, Guerin, Galin 2016, Computer Graphics Forum): litter and debris as
  objects placed by the processes that drop them (crown overhang, wind, slope), not by a texture.
- GPU and worker placement around the camera in tiles (Horizon Zero Dawn, van Muijden GDC 2017; Ghost of Tsushima
  grass): already how the vegetation streams.

## Proposal

1. `modules/vegetation/vegetation_detail.h`: the shared rules from the rock field's ground detail.
   - Reach: an item of height or length `s` is drawn out to `s / (pixels * 2 tan(fov/2) / height)` (3 px at the
     camera's field of view and the viewport's height, or a reference height).
   - `RenderingServer.multimesh_set_rt_instanced` on its MultiMeshes, so the path tracer adds one TLAS entry per item
     over the shared shape instead of a merged copy of every leaf.
2. In `VegetationField3D`, species under about 5 cm (litter, twigs, mosses, clover, seedlings) take their range from
   the reach rule instead of the hand value, and are cut into cells small enough that the range works per cell
   (as the near-swap cells already do).
3. A near-camera class per litter species: single leaves, needles and twig ends at the full deposition density within
   their reach, on top of the patch meshes, so the floor near the camera is covered leaf on leaf, as the dead-leaves
   model gives.
4. New small-plant presets where the floor is thin today (tiny herbs and grass tufts under 5 cm, moss cushions), as
   `PlantGenerator` presets and `VegetationField3D` species like `CLOVER` and `MOSS`.

GrassField3D (the grass blades) is left alone.

## Checks

Raster and path-traced pairs on the vegetation bench at 100% crop (path tracing must not lose a leaf), the walking
cost on the main thread (`--walk`), and memory with the feature on and off.
