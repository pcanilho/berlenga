# Vegetation placement (stage 1)

Stage 1 of vegetation.md: species, biomes and placement driven by the terrain data maps
(`TerrainDataMap3D`, terrain-data-maps.md).

## How others do it

- **Horizon Zero Dawn** (van Muijden, GDC 2017): each species has a density graph over world
  data. Placement is per tile on the GPU: a tiled point pattern with a fixed order is dithered
  against density (a point is kept when its rank in the pattern is below the density), so
  density changes smoothly and the result is deterministic. Species are placed in priority
  order (trees, then bushes, then ground cover); each placed plant writes its footprint into a
  map, and later species read it, so bushes do not grow inside trunks and grass thins under
  trees.
- **Far Cry 5** (Carrier, GDC 2018): biome rules in Houdini over terrain analysis (slope,
  curvature, flow, distance to water); the same "big first, then fill" order.
- **Unreal PCG**: surface sampler (Poisson-like points) → density filters by attribute (slope,
  height, layer weight) → self-pruning by bounds (larger first) → static mesh spawner. Its
  "self pruning" is the footprint step.
- **ProtonScatter** (Godot, MIT, code read): Poisson or random points, then modifier stack
  (slope filter by ray normal, noise, proximity); per-node, not tiled; no priorities between
  species except by separate scatter nodes with exclude shapes.
- **Point pattern**: Bridson (2007) fast Poisson disk sampling gives blue-noise points with a
  minimum distance; a toroidal (wrapping) tile repeats seamlessly, and a random rank per point
  gives Horizon's ordered dithering.

## What we build (`modules/vegetation`)

- **`PlantSpecies`** (Resource): mesh variants, `spacing` (minimum distance between two plants
  of the species), `footprint` (radius other species keep clear of), `density` (fraction of
  the Poisson points kept where every rule is fully met), scale and tilt ranges, and rules on
  the maps: `slope`, `height`, `curvature`, `wetness`, `water_depth`, `water_distance`, each a
  range with a soft edge (a smoothstep over `*_fade`). A rule with an empty range is off.
- **`Biome`** (Resource): species in priority order.
- **`VegetationField3D`** (Node3D): `biome`, `data_map` (a `TerrainDataMap3D`), `tile_size`,
  `seed`. `populate()` places every tile of the data map: for each species in order, its
  Poisson tile (Bridson, toroidal, built once per spacing and cached) is offset per tile, each
  point keeps when `rank < density x rules`, and is rejected inside the footprint of anything
  placed before it. Output: one MultiMeshInstance3D per species variant per tile (culling and
  persistent RT slots work per tile). Plants stand on the map height, tilt towards the ground
  normal by `align_to_ground`, random yaw and scale. Deterministic for a seed.
- Plants carry their placement in MultiMesh custom data (variant, scale, a random value), so
  materials and the bone wind (stage 4) can vary per plant.

CPU first: a tile of 32 m holds a few thousand candidates; tiles are independent, so they run
on worker threads. GPU placement (Horizon) is needed only when grass-density layers are
placed every frame, which is stage 2's grass field, not this one.

## Checks (bench `docs/bench/vegetation`)

The terrain-data valley with a wet floor, river and lake, three species built in code
(tree, bush, reed): reeds only in shallow water and wet ground near it, trees only on dry
gentle slopes, no bush inside a tree footprint, minimum spacing respected, same seed gives
the same result, and the placement cost per tile.

## Results

Bench `docs/bench/vegetation --test` on the 256 m valley (map 256 cells, tiles 32 m): trees
(spacing 5 m, footprint 2 m, slope under 20 degrees, 6 m from water), bushes (1.5 m, slope
under 35 degrees, 2 m from water), reeds (0.4 m, water 0 to 15 cm deep or within 3 m of it).
All checks pass:

- 832 trees, 6,183 bushes, 5,884 reeds;
- no reed outside the banks and shallows; no tree in water, on steep ground or within 3 m of
  water, and only 6 inside the 3 m fade band;
- no bush inside a tree footprint;
- minimum spacing 5.12, 1.50 and 0.40 m (Poisson radius 5, 1.5, 0.4), also across tiles;
- trees stand within 2 cm of the analytic ground;
- the same seed gives the same field; another seed gives another.

Cost: 45 ms for the whole 256 m field (12,899 plants) on the cloud CPU, once; drawing is
ordinary MultiMeshes, one per mesh per tile. Raster and path traced frames of the field
match (docs/bench/vegetation `--out`).
