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

## Geology rules (soil, rock, scree)

Slope alone lets trees stand on bare cliffs and scree fans that happen to be gentle. Real plant
cover follows the ground: trees need soil deep enough to root (most temperate trees want half
a metre or more; on thin soil over rock they are stunted or absent), grasses and herbs take
thin soil, and bare rock carries lichens and mosses only. Scree is too loose and dry for most
plants, with a few specialists.

How others do it: Ghost of Tsushima and Horizon drive species from painted or generated
ground-type layers (the same rule as a material layer weight in Unreal PCG's density filter);
Far Cry 5 and Wildlands derive soil and rock masks from their terrain erosion and feed them
to the biome rules; World Machine and Gaea export soil depth, flow and talus masks for the same
purpose. All of them treat the ground's material as one more map the placement reads.

What we build: `TerrainDataMap3D.geology_source` points at a `TerrainGenerator3D` (or any node
with `get_geology_at(pos) -> Color(soil m, rock, talus, ...)`); the bake copies soil depth,
rock exposure and talus into three more channels (`CHANNEL_SOIL_DEPTH`, `CHANNEL_ROCK`,
`CHANNEL_TALUS`, `get_soil_depth_at`, `get_rock_at`, `get_talus_at`). `PlantSpecies` gets
three more rules, `soil_depth`, `rock` and `talus`, ranges with soft edges like the others;
they apply only when the map has geology, so existing scenes are unchanged. `GrassField3D`
reads the same species rules, so grass thins on rock too. Suggested ranges: trees soil 0.5 m
and up, rock under 0.3, talus under 0.2; shrubs soil 0.2 m and up; grasses soil 0.05 m and up;
moss and lichen any soil, rock welcome.

Check (bench `docs/bench/vegetation --test`): a geology node with bare rock east of 60 m and
thin soil from 40 to 60 m; trees set to need 0.5 m of soil and under 0.3 rock place none past
40 m and still fill the rest; without a geology source the field is identical to before.
Cost: one generator lookup per map cell at bake time.

## Canopy shade rule

The forest floor is set by light more than by soil: bilberry, ferns, wood sorrel and mosses take the
shade under crowns, meadow grasses and flowers give up there. Field botany ranks species by it
(Ellenberg's light indicator values, L 1 to 9), and growth models compute it as the light a crown
lets through, which falls off exponentially with the foliage it crosses (Beer-Lambert in canopies,
Monsi and Saeki 1953). SpeedTree and Houdini forest set-ups do the same thing with a "shade mask"
painted from the placed trees before the understorey is scattered.

What we built:

- `PlantSpecies.canopy_radius` and `canopy_opacity`: a species with a crown casts shade. Each crown
  stops `canopy_opacity` of the light under its core, fading to nothing at 1.4 crown radii (sky light
  still reaches in under the edge); overlapping crowns multiply their transmission.
- `PlantSpecies.shade_range` / `shade_fade` (rule `RULE_SHADE`): where a species grows, from 0 (open
  sky) to 1. The default (0, 1) costs nothing: shade is only computed for species that narrow it.
- `VegetationField3D.get_shade_at()`: the same shade for other systems (ground tint, grass density,
  snow under trees later).
- Order matters as for footprints: canopy species first in the `Biome`, the understorey after.
- Shade is canopy only. Sun-facing slopes and terrain shadow come from the terrain thread's climate
  channels (insolation) when those land, and combine with this.

Checks (bench `docs/bench/vegetation`): a crown core gives the crown's opacity, shade-loving bushes
land only where shade >= 0.5 (639 of 6183 open-ground bushes), trees are unchanged, and with no
canopy the field is exactly the old one.
