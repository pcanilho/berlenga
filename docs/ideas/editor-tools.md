# Editor tools for the world systems

Level designers set up the world systems through nodes and the inspector. Before this work the only tool in the
editor was the generator's Generate button. Everything else (Landscape3D, VegetationField3D, GrassField3D,
RockField3D, snow, weather, ground) skipped its work in the editor, so a level had to be built from a script, as
the landscape bench is. This note plans five tools, in build order. Each one follows an established editor
workflow, named below.

## What established tools do

- **Terrain3D** (Godot, MIT; never bundled, called by name only). `Terrain3DEditor` tools: Sculpt (raise, lower,
  flatten, expand, reduce, slope, smooth), Height, Texture, Color, Roughness (wetness), Angle, Scale, Autoshader,
  Holes, Navigation, Instancer (paints MultiMesh instances), Region. Operations: ADD, SUBTRACT, REPLACE, AVERAGE,
  GRADIENT. Brush data: image, size, strength, pen pressure, slope range, align to view, gamma, spin. Painting
  runs on the CPU. Bulk edits read a region's Image, edit it and push it with `update_maps`. Undo:
  `start_operation`, `backup_region` per touched region, `stop_operation` commits the edited regions as one undo
  step.
- **Unreal Landscape edit layers.** Sculpt and paint go into named layers that are composited over the base, so a
  layer can be hidden, reordered or cleared without losing the rest. TerrainDeformer3D already uses this model for
  brushes.
- **Unreal PCG editor mode** (UE 5.9, experimental). Tools: Draw Spline, Draw Spline Surface (fills a closed
  spline), Paint (raycast onto collision, adds points; Shift removes), Volume (drag a footprint, then a height).
  The tool adds a PCG component to the selected actor, or spawns a new one. Data layers choose what a paint
  stroke writes to. Generated content previews in the editor and is saved with the component.
- **Unreal PCG Biome Core.** Biome actors (volume, spline or texture), each with a Biome Definition: name, unique
  RGB colour and priority (lower wins). Each biome holds assets with generators and filters. A global pass merges
  overlapping biomes by priority. Hand-placed content is kept by exclusion volumes or closed splines tagged
  `BiomeExclusion`, and open splines tagged `BiomePath`. Generation can be partitioned into 256 m cells for World
  Partition.
- **Unreal foliage mode.** Paint, erase, single-instance place and reapply, against a list of foliage types.
  Instances are stored per level and per partition cell.

## A. Editor preview and bake

Reference: PCG's Generate and preview on the component; Terrain3D's live editor view.

- Each system gets an editor preview that follows the editor camera, as Landscape3D's `_camera()` already does:
  Landscape3D, VegetationField3D, GrassField3D, RockField3D, SnowSurface3D, GroundSurface3D and WeatherMap3D.
  It is a per-node `editor_preview` setting, on by default, with tool buttons to rebuild or clear (`Rebuild`,
  `Populate`, `Clear`). Work stays on worker threads with the same budgets as in the game, so the editor stays
  responsive. Wind, rain and snow show their resting state, without time stepping.
- `TerrainGenerator3D.generate_on_ready`: a saved scene generates its ground when loaded, in the editor and in the
  game, and Landscape3D rebuilds when its source finishes. The result stays derived rather than saved: the
  generator is deterministic, and its expensive stages already go through the world cache.
- **Project world cache** (`world/cache/path` set to a folder in the project): the cache ships with the level, so
  the first launch on another machine skips the minutes of erosion and dunes. A game keeps that folder in LFS.
- The test is a scene file with no script that opens in the editor with ground, plants, rocks and grass showing,
  and runs the same.

## B. Mask painter

Reference: Terrain3D's paint tools and undo by region; PCG's Paint tool with data layers.

- One editor plugin (`modules/world/editor`) paints any mask slot the systems already read:
  `VegetationField3D.density_mask` (per channel, matched by `PlantSpecies.mask_channel`),
  `TerrainGenerator3D.uplift_map` and `sea_map`, `GroundSurface3D.type_map` and `mud_map`, and
  `WeatherMap3D.painted_map`. The painter lists the selected node's mask slots.
- Brush: size, strength, falloff, pen pressure, channel, slope limits. Operations: Add, Subtract, Replace, Smooth
  (Terrain3D's set). Ctrl inverts and Shift smooths, as in the cloth painter.
- Rays hit the ground through WorldGround, so painting works on Landscape3D, Terrain3D or a HeightMapShape3D.
- Undo: tiles touched by a stroke are backed up and committed as one undo step (Terrain3D's backup_region).
- Storage: the mask is saved as an image resource next to the scene, created at the slot's rect when missing.
  The consumer re-reads only the painted rect (vegetation re-places only its touched tiles).
- Scale: masks are kept in 256 m tiles (PCG's partition size), so an open world loads only the tiles it streams.
  Existing single-image slots keep working.

## C. Hand placement and keep-out areas

Reference: PCG Biome Core exclusions (`BiomeExclusion`, `BiomePath`); Unreal foliage single place.

- `WorldExclusion3D`: a box or closed Path3D. Its kind flags pick what it keeps out: plants, grass, rocks, ground
  detail. An open path is a track of a set width. It is registered through a new WorldContracts slot, so every
  field reads the same areas, and moving one re-places only the tiles it touched.
- `PlacedPlant3D`: one plant of a chosen species and variant, at the node's transform. Fields keep its footprint
  clear and treat it like their own plants (canopy, shade, wind, PT). Rocks get `PlacedRock3D`.
- Instance paint: the painter's Instance tool paints and erases placed plants or rocks of a chosen species. They
  are saved per 256 m tile as resources, so regeneration never removes them.

## D. Biome map

Reference: PCG Biome Core biome definitions (colour and priority) and texture biomes.

- `BiomeMap`: a list of entries (Biome, colour, priority) and a painted index map (painted with B).
  VegetationField3D reads it: each species' weight is multiplied by its biome's share at that point, and
  boundaries blend over `blend_width` so edges mix rather than cut.
- Biome volumes: `WorldExclusion3D` gets an "override" kind that forces a biome inside it, as PCG's volume biomes.
- The default stays rule-driven: without a map, a field uses its one Biome, as today.

## E. Freehand sculpt layer

Reference: Unreal Landscape edit layers; Terrain3D sculpt operations.

- `TerrainSculptLayer3D` under the generator: a height-offset layer in 256 m tiles, painted with B's plugin using
  the sculpt operations Raise, Lower, Flatten, Smooth and Slope.
- It composites after the generated ground with the existing touch-up path (`apply_touch_ups`), so lakes and
  rivers re-solve around sculpted ground and the erosion detail fades where the layer flattened.
- Layers stack in tree order. Hiding one restores the ground under it, because the deformer keeps base heights.

## Rules for all five

- No visual change in the game: tools only change what a designer authors. Each tool gets a bench check, and a
  raster and path-traced pair of a painted or placed result at 100% crop.
- Editor work runs on worker threads within the same budgets as in the game.
- Each tool gets a site line when it lands.
