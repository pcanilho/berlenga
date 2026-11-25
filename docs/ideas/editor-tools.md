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

Built:

- `TerrainGenerator3D.generate_on_ready` (on): a saved scene generates its ground when the node is ready, in the
  editor and in the game, unless a script generated it first (deferred a frame). `water_on_ready` (on) then builds
  the sea, lakes and rivers (`water_river_min_area`) as internal children that are never saved, rebuilt after the
  Generate button and after touch-ups. Generate no longer fails without a ground node: a Landscape3D reads the
  generator itself. Benches that generate in their own code set `generate_on_ready = false`.
- `Landscape3D.auto_rebuild` (on): rebuilds once its generator source has a result and each time it generates again
  (a script's own `rebuild()` first cancels the deferred one); a TerrainMaterial3D material is pointed at that
  generator (`use_generator`, new `get_generator`). Rebuild button.
- `TerrainDataMap3D`: with `bake_on_ready` and a generator as height source, bakes once the generator has a result and
  again each time it generates, in the editor too. A queued bake is skipped when a script baked in the meantime.
- `editor_preview` (on) on VegetationField3D (Populate and Clear buttons), GrassField3D and RockField3D (Rebuild
  button): in the scene being edited they place as in the game round the editor's 3D view
  (`WorldContracts.get_view_camera`), without the game-time systems (trampling, fire, floods, rockfall, far bakes).
  Nothing they place is saved.
- Bench: `docs/bench/editor`. `make_world.gd` saves a scene of system nodes only; `check.gd` opens it in the headless
  editor and runs it, and checks ground, water, material, data map, plants, grass and stones appear with no script,
  and that saving keeps only the authored nodes.

Left for later: weather, snow and ground deformation previews (they need time stepping); a project world cache
folder so the cache ships with a level (`world/cache/path` already accepts one).

## B. Mask painter

Reference: Terrain3D's paint tools and undo by region; PCG's Paint tool with data layers.

Built (`modules/world/editor/world_mask_paint_plugin.cpp`): a "Paint Mask" toggle in the 3D toolbar for any selected
node with a mask slot: a Texture2D or Image property named `*_map` or `*_mask` with a matching `*_rect` property, or
on a node with `get_world_rect()` (the generator's `uplift_map` and `sea_map`). That covers
`VegetationField3D.density_mask` and `biome_map`, `GroundSurface3D.type_map` and `mud_map`, `WeatherMap3D.painted_map`
and `canopy_map`, and `TerrainSculptLayer3D.height_map`.

- Toolbar: slot, channel, operation (Add, Subtract, Replace, Smooth), Value, Radius (m), Strength, the texel of a new
  map, and Fill. Shift+LMB subtracts, Ctrl+LMB smooths.
- The brush ray-marches to the generated ground (any TerrainGenerator3D in the scene), else a collider, else the
  node's height. A decal shows the painted channel.
- Undo: each stroke stores only the pixels it changed, before and after (Terrain3D's backup_region).
- A painted texture is updated in place, so nodes sharing it (biome fields) all see the paint; every node using it is
  told the changed rect through `notify_<slot>_changed(area)` when it has one. VegetationField3D re-places only the
  tiles there while streaming.
- Storage: a new map of a saved scene is saved next to it as `<scene>_<node>_<slot>.res` when the scene is saved.

Left for later: masks in 256 m tiles for open worlds (today one image per slot); pen pressure.

## C. Hand placement and keep-out areas

Reference: PCG Biome Core exclusions (`BiomeExclusion`, `BiomePath`); Unreal foliage single place.

Built:

- `WorldExclusion3D` (a Path3D): a closed curve is an area, an open one a track of `width`, no curve a box of `size`
  (rotation and scale apply); `fade` softens the edge; `kinds` flags Plants, Grass, Rocks. Moving or editing one
  announces the old and new bounds through `WorldContracts` signal `exclusions_changed(area)`.
  VegetationField3D (all three placement passes), GrassField3D (CPU tiles, the GPU fill falls back to the CPU in a
  touched tile; far rings) and RockField3D (coarse layers) keep out and place only that ground again.
- `PlacedPlant3D`: one variant of a PlantSpecies at the node's transform; draws it and keeps procedural plants (and
  stones, and grass with `clear_grass`) out of its footprint (`clearance`).

Left for later: an Instance paint tool that paints and erases placed plants in tiles; `PlacedRock3D`; ground detail
stones.

## D. Biome map

Reference: PCG Biome Core biome definitions and texture biomes.

Built: fields that share one painted `biome_map` (with `biome_map_rect`) each take a `biome_channel`. A field's
plants are kept by its channel's share there, normalised when the painted channels sum past 1, so overlapping paint
splits the ground; outside the map a field grows everywhere. Up to four biomes per map; paint them with B. A field
keeps its one Biome, so every existing rule and species order is unchanged.

Left for later: priority between biomes (PCG's lower-priority-wins) instead of shares.

## E. Freehand sculpt layer

Reference: Unreal Landscape edit layers; Terrain3D sculpt operations.

Built: `TerrainSculptLayer3D` (a TerrainBrush3D) under the generator holds a painted map of offsets in metres
(`height_map`, `height_map_rect`, `height_scale`), painted with B: Add raises by Value metres, Subtract lowers,
Replace eases to Value, Smooth smooths. The generator adds its offsets at the grid's nodes in the touch-ups, so lakes
and rivers re-solve round sculpted ground, and adds the strokes finer than its cells when it draws the ground (each
layer's offset less the nodes' interpolated sum). Layers stack in tree order; turning one off restores the ground.

## Rules for all five

- No visual change in the game: tools only change what a designer authors. Each tool gets a bench check, and a
  raster and path-traced pair of a painted or placed result at 100% crop.
- Editor work runs on worker threads within the same budgets as in the game.
- Each tool gets a site line when it lands.
