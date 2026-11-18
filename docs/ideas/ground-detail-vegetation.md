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

## Built (18 Nov): items 1 and 2

- `modules/vegetation/vegetation_detail.h`: `pixel_angle()`, `reach()`, `trace_instanced()`.
- `PlantSpecies.detail_size` (what shows of one item: a leaf's or twig's length, a cushion's width, a seedling's
  height), set by `PlantGenerator.tag_species()` for litter, twigs, mosses, small rosettes and seedlings.
  `VegetationField3D.detail_reach` (on), `detail_pixels` (3), `detail_reference_height` (0: the viewport),
  `detail_max_range` (40 m). The range is never shorter than the hand value. At 1080p and 75 degrees: leaf litter
  15 to 28 m, twigs 18 to 33 m, moss and clover 15 to 17 m, seedlings 30 to 40 m.
- Every ranged species' cell now ends a half diagonal past its range (the range is measured to a cell's centre),
  and tiles bigger than the range are cut into cells of the range (at least 16 m, as cells are draw calls).
- The path tracer captures and merges any material whose vertex shader moves it, and the plant shader does
  (wind, thinning by rank), so `trace_instanced()` on plant cells changes nothing today; it is there for static
  items such as item 3's single leaves.

Cost, vegetation bench `--floor` (oak wood floor under crowns, 16 m tiles, streamed 60 m, 1080p reach), lavapipe
640x360, off then on: walking 30 m at 0.1 m a frame, main-thread streaming mean 0.07 / 0.07 ms, p95 0.39 / 0.35 ms;
memory +85 / +98 MB, video +104.5 / +107.6 MB; 1.654M / 1.668M primitives, 3057 / 3206 draw calls; raster frame
4.0 / 5.1 s (software rasteriser; more leaves past 15 m that are no longer shrunk to nothing). Path traced: the
cloud runs out of memory on this floor with the reach off as well as on, so the PT frame cost goes to the PC queue.
