# Procedural terrain and mountains: research and plan

Goal: generate believable ground, from continental mountain ranges down to gullies a few metres
wide, for worlds of tens of kilometres. It must be deterministic per seed, produce tiles that
join without seams, write into the ground the fork already uses (Terrain3D by name, or
`TerrainDeformer3D`'s `HeightMapShape3D` backend), and hand placement the maps it needs
(flow, sediment, exposed rock, wetness) through `TerrainDataMap3D`.

This note follows the fork's rule: survey the published methods and shipped pipelines first,
name the method, then build it. Nothing here is built yet.

## The one idea every serious pipeline shares

Plain fractal noise does not look like mountains. Real relief is made by water: valleys join
into trees (dendritic drainage), ridges are what is left between them, slopes sit at the angle
the rock can hold, and sediment fills the valley floors. Every tool and paper below is a way to
get that drainage structure, either by simulating it or by faking it locally:

1. **Simulate** erosion on a grid (hydraulic, thermal, stream power). Correct structure, but
   non-local: a cell's result depends on everything upstream, so it does not tile for free.
2. **Fake it locally** with noise whose gradients follow the slope (derivative-damped fBm,
   erosion filters). Every point is evaluated alone, so it tiles, streams and runs in a shader,
   but drainage is only plausible, not connected.

Shipped open worlds use both, at two scales: simulate coarse and global, then add local
detail per tile. That is the plan this note ends with.

## Tools artists use

- **World Machine** (Schmitt). Node graph of generators (Perlin, ridged, Voronoi) and filters;
  its Erosion device is a particle-and-grid hydraulic model with thermal weathering, sediment,
  flow and wear outputs. Large worlds use *tiled builds*: each tile is built with an apron and
  blended into its neighbours. Erosion is non-local, so tiles still disagree near seams unless
  the apron is wide; its forum has years of threads on exactly that.
- **Gaea** (QuadSpinner). Same family, GPU erosion nodes (Erosion 2 in Gaea 2, selective
  erosion added in 2.2), "Tiled Build" with overlap, and data outputs (flow, wear, deposits)
  for texturing and scattering. Its tiled builds face the same issue: erosion near a tile edge
  depends on ground outside the tile.
- **Houdini heightfields** (SideFX). `HeightField Erode` splits into `Erode Hydro`,
  `Erode Thermal` and `Erode Precipitation` (OpenCL). Layers: height, bedrock, debris,
  sediment, water, flow. Far Cry 5 (Carrier, GDC 2018 and HIVE 2018) and Ghost Recon Wildlands
  used it for flow maps, rivers, cliffs and biome rules on top of the base terrain.
- **Unreal Landscape.** Sculpt tools include Erosion and Hydro Erosion brushes (local
  smoothing-style passes, not a world simulation); Landmass blueprint brushes and Water bodies
  composite through edit layers; PCG reads the landscape. There is no built-in world
  generator; studios import from Gaea, World Machine or Houdini.
- **Terragen** has a Heightfield Erode node; Instant Terra (Wysilab) offers GPU erosion with
  tiled output. Same models as above.

## Shipped games

- **Ghost Recon Wildlands** (Ubisoft, 2017; GDC 2017 "Terrain Technology and Tools", 80.lv
  interview). World 64k x 64k samples, built in World Machine on an 80 machine farm. Erosion is
  non-local, so: "we pre render the big eroded land mass in one machine at a lower resolution,
  then distribute micro erosion and other costly details on the render farm using the Tile
  system. It fixes 90% of the tile joint." This is the two-scale scheme, from production.
- **Far Cry 5** (Carrier, GDC 2018 "Procedural World Generation of Far Cry 5"). Houdini bakes
  terrain analysis per tile (flow from an erosion pass, slope, curvature, water distance) and
  drives biomes, freshwater networks and cliff rocks from it.
- **Star Citizen, Star Engine** (Cloud Imperium; Planet Tech v5, Genesis and Starchitect,
  CitizenCon 2024 and 2025). Planets carry data maps: temperature, humidity, geology, soil type
  and depth, nutrients, plus derived sunlight and slope aspect. An erosion simulation run at
  development time produces an erosion map that places rocks and debris; flora is an *emergent
  biome* from per-species requirements competing on those maps. Starchitect is their tool for
  authoring planets this way. Takeaway for us: erosion is baked, and its value is as much the
  maps it leaves (where water went, what is exposed) as the heights.
- **Elite Dangerous** (Frontier, Stellar Forge; Odyssey rebuild 2021). Planet surfaces from
  physically derived parameters (geology, age, gravity) feeding layered procedural functions;
  Odyssey moved to a tile-based system and was criticised for visible repetition. Lesson:
  local procedural detail must not repeat at the scale a player can see.
- **Horizon Zero Dawn** (van Muijden, GDC 2017). Hand-shaped terrain, then GPU procedural
  placement from world data maps. Same consumer side as our vegetation module.
- **Outerra, Space Engine, No Man's Sky.** Fully runtime procedural: real or noise base, then
  local fractal amplification evaluated per tile. No erosion simulation; relief reads as
  noise up close, which is the limit of the local-only approach.

So the established production answer is: **coarse global erosion baked once, per-tile local
detail, and the erosion's side products (flow, sediment, rock) kept as maps.**

## Papers, by scale

Survey: Galin et al., *A Review of Digital Terrain Modeling*, Eurographics 2019 STAR. Every
method below is in it except the newer ones.

### Local, tileable (no simulation)

- **Fractional Brownian motion and ridged multifractal.** Musgrave, Kolb and Mace, *The
  synthesis and rendering of eroded fractal terrains*, SIGGRAPH 1989; Ebert et al.,
  *Texturing and Modeling* (ridged and hybrid multifractals, heterogeneity by altitude).
- **Derivative-damped fBm** (Quilez, "Elevated", 2009; iquilezles.org articles on noise
  derivatives and domain warping). Each octave is scaled by 1 / (1 + |accumulated gradient|²),
  so slopes get smoother and flats keep detail; domain warping bends features. Cheap, local.
- **Erosion filter / erosion noise.** Clay John, "Eroded Terrain Noise" (Shadertoy 2018);
  Fewes, "Terrain Erosion Noise" (2023); R. S. Johansen, *Fast and Gorgeous Erosion Filter*
  (blog and video, March 2026; code MPL 2.0). Uses the height gradient: per Worley-style cell,
  stripes are oriented along the downhill direction, giving gullies and ridges; octaves follow
  the slope already carved, so they branch. Every point is independent, so it is exact across
  tiles and can run in the terrain material too. Johansen's version also outputs a ridge map
  (ridges vs creases) for drainage streaks and texturing. Limit he states: gullies can stop
  halfway down a slope, so it is not a real drainage network.

### Grid simulations, metres to kilometres

- **Hydraulic erosion, virtual pipes.** Mei, Decaudin and Hu, *Fast Hydraulic Erosion
  Simulation and Visualization on GPU*, Pacific Graphics 2007: shallow water by virtual pipes
  between cells, sediment capacity C = Kc · sin(slope) · |v|, dissolve and deposit, advect
  sediment semi-Lagrangian, evaporate. Grid-parallel, so it maps to compute shaders.
- **Layered materials.** Št'ava, Beneš, Brisbin and Křivánek, *Interactive Terrain Modeling
  Using Hydraulic Erosion*, SCA 2008: several rock and soil layers with different
  erodibility, so hard strata stand out as cliffs and benches.
- **Thermal erosion (talus).** Musgrave et al. 1989; Olsen, *Realtime Procedural Terrain
  Generation*, 2004 (faster variant); Jákó and Tóth, *Fast Hydraulic and Thermal Erosion on
  the GPU*, CESCG 2011. Material above the angle of repose slides to lower neighbours.
- **Particle (droplet) erosion.** Beyer, *Implementation of a method for hydraulic erosion*,
  TU München 2015; the popular indie method (Sebastian Lague). Sequential, so poor on GPU and
  order-dependent, which breaks determinism under threading. Not chosen.

### Large scale, geomorphology

- **Uplift and stream power.** Cordonnier, Braun, Cani, Beneš, Galin, Peytavie and Guérin,
  *Large Scale Terrain Generation from Tectonic Uplift and Fluvial Erosion*, Eurographics 2016.
  dh/dt = U − K · A^m · S^n: uplift U raises rock, the stream power law (drainage area A,
  slope S) cuts it. At equilibrium you get mountain ranges with real dendritic river networks
  and ridges, at kilometre scale. Uses the implicit O(n) solver of Braun and Willett, *A very
  efficient O(n), implicit and parallel method to solve the stream power equation*,
  Geomorphology 2013 (receiver tree, stack order, lakes handled by joining basins).
- **Interactive GPU version.** Schott, Paris, Fournier, Guérin and Galin, *Large-scale Terrain
  Authoring through Interactive Erosion Simulation*, ACM TOG 2023 (SIGGRAPH 2023). Paint an
  uplift map, the stream power solve runs every frame on the GPU, mountains rise where uplift
  is high. Code MIT (github.com/H-Schott/StreamPowerErosion). I read `spe_shader.glsl`: one
  8x8 compute kernel does multiple-flow-direction drainage accumulation (each cell gathers
  from uphill neighbours weighted by slope^p), stream power along the steepest descent,
  optional hillslope diffusion (k_h · Laplacian) and debris slope, clamps to the receiver
  height so no pits form, then adds uplift · dt. Drainage converges over iterations instead of
  a global sort, which is why it fits a GPU.
- **Flow routing on GPU.** Jain, Kerbl, Gain, Finley and Cordonnier, *FastFlow: GPU
  Acceleration of Flow and Depression Routing for Landscape Simulation*, Pacific Graphics 2024:
  O(log n) parallel flow accumulation and depression routing, so the exact Braun-Willett step
  runs on GPU instead of the iterative approximation.
- **Debris flow on steep slopes.** Jain et al., *Efficient Debris-flow Simulation for Steep
  Terrain Erosion*, SIGGRAPH 2024. Fixes stream power's over-smooth steep faces: sharp
  couloirs and fans.
- **Analytical stream power.** Tzathas, Gailleton, Steer and Cordonnier, *Physically-based
  Analytical Erosion for fast Terrain Generation*, Eurographics 2024: solves the equilibrium
  stream power profile in closed form along the flow tree, so the steady state comes without
  time stepping.
- **Multi-scale.** Schott, Paris, Fournier, Guérin and Galin, *Terrain Amplification using
  Multi-Scale Erosion*, ACM TOG 2024 (SIGGRAPH 2024): run stream power at coarse scale, upsample,
  then rerun erosion at finer scales (stream power, thermal, deposition) so the detail follows
  the coarse drainage. The paper version of the Wildlands scheme.
- **Glaciers.** Cordonnier, Jouvet, Peytavie et al., *Forming Terrains by Glacial Erosion*,
  SIGGRAPH 2023 (U valleys, cirques). Later, if mountain worlds need alpine shapes.

### Analysis maps (already in the fork)

Slope (Horn 1981), curvature (Zevenbergen and Thorne 1987), Priority-Flood depression filling
(Barnes et al. 2014), multiple flow direction accumulation (Freeman 1991, Holmgren 1994) and
wetness index (Beven and Kirkby 1979) are in `TerrainDataMap3D` (docs/ideas/terrain-data-maps.md).
Generation only adds what analysis cannot recover: sediment thickness, exposed bedrock, talus.

## Godot addons (code read)

- **Terrain3D** (MIT, commit read 10 Nov): sculpting brushes, no generator and no erosion
  (`grep erosion src/` is empty). Its `backgrounds.glsl` only blends a flat ground level
  outside regions. Bulk writes go through `Terrain3DData.import_images()` (per region,
  faster than `set_pixel`), then `update_maps()`. We write into it by name, as the deformer does.
- **HTerrain** (Zylann, MIT): `tools/generator` has Perlin noise plus `erode.gdshader`, which
  is a morphological min/max filter with a slope factor, not hydraulic or thermal erosion.
  Fine for blobs, does not produce drainage.
- **Procedural-Terrain-Generator-for-Godot** (EmberNoGlow, public domain): meshes a given
  heightmap image; no generation or erosion.
- Asset library erosion demos (Erosion Simulator, Realtime-Erosion-Simulation, Acerola
  "Dirt Jam" entries): droplet or pipe-model toys on one grid, not tiled, not deterministic.

None is worth incorporating. The stream power kernel (MIT) and the erosion filter idea are
the two pieces of outside work we would use; the filter would be a clean reimplementation from
the published description, or its MPL file kept as its own file with credit.

## What fits this fork

Constraints: tens of km per world, tiles streamed by Terrain3D regions, same result on every
machine, no frame cost at runtime unless it buys visible detail, path tracing must keep every
bit of detail raster shows.

Determinism. GPU float results can differ between vendors and drivers, and order-dependent
methods (droplets, atomics) differ between runs. So the bake that produces collision-relevant
heights runs on CPU with fixed iteration order (threaded per tile, tiles independent within a
pass), or on GPU at edit time with the result saved. Runtime generation is only allowed for the
local, pure-function stages, where a vendor difference changes nothing a player can collide with.

## Plan

`TerrainGenerator3D` (new node in `modules/terrain_deform`, or its own `modules/terrain_gen`),
a fixed stack of stages, each a resource with a seed and parameters. It bakes once in the
editor or from a script, then writes heights by region into Terrain3D (by name) or the
deformer's `HeightMapShape3D`. `TerrainDeformer3D` brushes (rivers, roads, plots) stay on top:
the generator writes the base heights the deformer composes over.

**Stage 1, macro shape.** Uplift map from masks (painted image, splines for ranges, or ridged
multifractal noise) plus base elevation. Coarse grid, for example 32 m cells: 64 km is 2048².

**Stage 2, global erosion: uplift plus stream power** (Cordonnier 2016, Braun-Willett 2013
solver; Schott 2023 parameters and hillslope diffusion; Tzathas 2024 as the fast equilibrium
option). Runs on the whole world at coarse resolution, so drainage is connected across every
tile: no seams by construction. CPU implicit solver: O(n) per step, a few hundred steps,
seconds to a minute at 2048². Outputs coarse height, drainage area, river network.

**Stage 3, local amplification** per tile at full resolution (1 m or the region's spacing):
bicubic upsample of stage 2, then the erosion filter (Johansen 2026) and derivative-damped fBm
driven by stage 2's slope and drainage. Pure function of world position, so tiles match to the
bit. The same function can run in the terrain material as displacement and normal detail below
the grid spacing, which is how gullies thinner than a vertex stay visible, raster and path
traced alike (displaced shaders work under path tracing since #182).

**Stage 4, fine simulation per tile, optional** (Mei 2007 virtual pipes with Št'ava 2008
layers, thermal talus). Each tile is simulated with an apron of k cells read from its
neighbours' stage 3 result, and only the interior is kept; seams are bounded by the apron and
cross-faded over a few cells (the Wildlands and World Machine scheme). This adds sediment fans
and talus at the 1 to 50 m scale that the filter cannot.

**Stage 5, maps.** Sediment depth, exposed bedrock, talus, drainage area and a river mask,
written as Terrain3D control or colour hints and as new `TerrainDataMap3D` channels, so the
vegetation module and texturing read them (the Star Engine and Far Cry 5 pattern). Rivers:
export main channels above a drainage threshold as `Path3D` for `PhysXRiver3D`.

Costs: all of it is bake time, zero runtime frame cost, except stage 3 in the material, which is
a few ALU-heavy noise octaves per terrain pixel and is optional per material (to be timed on
the PC under raster and path tracing before merging). Bake times measured in the cloud on CPU.

Verification per step: deterministic hash of each tile across runs and thread counts; seam
error across tile borders (must be 0 for stages 1 to 3, under a set bound for stage 4); a
generic test scene rendered raster and path traced at 100% crop.

Build order proposed: stages 1 and 2 with the bake into Terrain3D and `HeightMapShape3D`, then
stage 3 (bake and material), then stage 5 maps, then stage 4.

## Stages 1 and 2: what was built

Picked 10 Nov: mountain ranges first. `TerrainGenerator3D` in `modules/terrain_deform`:

- **Uplift** (stage 1): `base_uplift` everywhere, `uplift_map` (red channel over the rectangle), Path3D `ranges`
  (raised-cosine band of `range_width`, per-path `width` and `uplift` metadata), all multiplied by `uplift_noise`.
  Rate in mm/yr (`uplift`).
- **Solver** (stage 2), `terrain_stream_power.h`, plain C++ on caller-owned buffers, single-threaded in a fixed
  order: D8 receivers, Braun-Willett stack, implicit stream power (n = 1, `area_exponent` m), critical slope along
  the receiver (`critical_slope`, threshold hillslopes), explicit hillslope diffusion with stable sub-steps,
  Priority-Flood + epsilon only on steps that leave a closed basin (fill kept as `sediment`), and drainage area by
  multiple flow directions (`flow_exponent`, cells visited highest first by an O(n) radix sort) so valleys do not
  follow the grid. Outlet edges sit at `sea_level`. The seed only sets the starting roughness.
- **Write**: bicubic (Catmull-Rom) onto Terrain3D, by name (missing regions created with `add_region_blank`,
  heights written per region image, then the deformer's `commit_grid` path: edited flags, `update_maps`, collision)
  or a `HeightMapShape3D` (+ ground mesh). A `deformer` is cleared before and re-applied after, so rivers, roads and
  plots follow the new ground. `max_height` rescales relief; prefer `uplift` and `range_width`, since scaling
  steepens slopes past the critical angle.
- Inspector Generate button; `get_height`, `get_drainage_area`, `get_sediment_depth` and their images for the
  next stages.

Tuning (prototype, 4 km): `uplift` / `erodibility` sets equilibrium steepness (2 mm/yr over 2e-5 gives about 350 m
of relief on an 800 m half-width range; 6 mm/yr and 1300 m about 1 km). Without `critical_slope` single cells
grow into spikes; without diffusion ridges facet along the grid; `hillslope_diffusion` 0.01 m²/yr at 16 m is the
balance. D8 alone leaves straight 45 degree valleys, hence MFD by default.

Bench `docs/bench/terrain_gen` (`--test`, `--time`, `--map`, `--out`): 15 checks pass in the cloud (determinism by
hash, another seed differs, relief, outlets, no pits, trunk river drains 21% of the world, slopes under the critical
angle, `max_height`, HeightMapShape3D matches `get_height` at every vertex, deformer brush re-applied and its base
height is the new ground). Generation time on the cloud CPU, 300 steps: 257² 5.3 s, 513² 26 s, 1025² 169 s. All
of it is bake time; nothing runs per frame. The Terrain3D path is code-reviewed against Terrain3D 1.1 but not run
in the cloud (`--test --terrain3d` once the addon is in the bench project, on the PC).

Next after stages 1 and 2: stage 3, then stage 5 maps, then stage 4.

## Stage 3: what was built

`terrain_erosion_filter.h`, written from Johansen's description (not his MPL code): per octave a jittered grid one
wavelength wide; stripes through each pivot run down the steering slope (cos height, sin slope), blended over the
5x5 cells around the point with a compact weight (radius 1.5 cells, so no pivot enters or leaves at a cell border;
the first 4x4 version left straight seams at 0.5 m crops); partial normalization (lengths over 1/2 to 1); fade
approach with an altitude fade target; stacked fading (each octave's result is the next fade target, its creases and
ridges multiply into the mask with the `detail_reach` power); straight gullies (the next octave steers by the sign of
this one's slope); gully weight. The input gradient comes from a 3x3 stencil one coarse cell wide, and its four
quarter gradients give the slope's coherence (mean gradient length over mean length of the quarters; Kass and Witkin
1987, Bigun and Granlund 1987): 1 on an even slope, near 0 where opposite slopes meet. The filter's first mask is
multiplied by it, so gullies stop short of crests and valley floors instead of cutting through them.

`TerrainGenerator3D` applies it in `get_height()`, and the terrain write fills rows in parallel through
`WorkerThreadPool` from the same function, so written ground equals `get_height()` at every vertex and tiles join
exactly. Defaults: 128 m, 4 octaves, depth 0.08, gully weight 0.5. Cost: about 1.8 microseconds a vertex on one
cloud core (66k vertices written in 47 ms on 4 threads). Bake only; no frame cost.

Look: raster and path traced close views at 100% crop show the same gullies and ridges; the path tracer keeps all
the detail (the frame is noisier at 16 samples on the CPU, nothing missing).

Dented crests (fixed after the first merge): without the coherence fade, the gullies of both flanks met on the crest
and notched it into a regular comb of teeth a few metres tall, plain on summit silhouettes. Fading them fully made
crests and spurs straight, which lost character. Now the gullies keep 40% on crests (`crest_keep`; along crests
the offset bends about half as much as without the fade, bench check), and a ridged gradient-noise fBm (Musgrave's
ridged multifractal family) takes over where they fade, on convex ground only, so crests get irregular pinnacles
and cols (`detail_crest`, amplitude twice the gully amplitude). Valley floors (concave) get no crest noise. The
jagged colour borders in the first shots were the bench's colouring, per vertex on a 4 m mesh; the bench now
colours per pixel and builds the close view at 2 m through `sample_heights()`. Still open: the same function in the terrain material for detail under the vertex
spacing (needs a GPU timing), and a ridge map output for texturing. Speed-ups when worlds grow: threading the per-cell passes, and FastFlow-style GPU
routing (Jain et al. 2024).

## Stage 5: what was built

`terrain_geology.h` (plain C++, run at the end of `generate()` over the final grid, its MFD drainage area, basin
fill, and the D8 receivers and stack of the last routing):

- Soil: the exponential soil production function (Heimsath, Dietrich, Nishiizumi and Finkel 1997). Bare rock weathers
  at P0 (`soil_production`, 0.5 mm/yr), slowing as e^(-h/h0) (`soil_depth_scale`), so soil settles at
  h = h0 ln(P0 / E) where E is the local erosion rate of the same laws the run used: K A^m S plus D times the
  convexity. Slopes near the critical slope shed it. Ranges rising at mm a year come out bare; plains and gentle hills
  covered. Basin fill adds on top.
- Alluvium: channels gentle for their drainage area are alluvial. Their channel steepness index k_s = S A^m (Wobus et
  al. 2006) falls below a fifth of U_max / K (the steady-state k_s of the fastest-rising ground), the bedrock against
  alluvial channel split of Montgomery et al. 1996. They get a full cover.
- Rock exposure: where the loose cover is under about 0.3 m.
- Talus: scree shed by bare rock steeper than `talus_angle`, carried down the receivers and laid where the slope eases
  below repose (above about 7 degrees), thinning over `talus_reach`; none on channels; a 3x3 spread.
- Rivers: a D8 area along the receivers (an MFD area can fall downstream where flow splits, which broke widths);
  each river runs upstream from an outlet or junction along the largest donor (Hack's main stem), other donors start
  tributaries, so trunks come first and every tributary names the river it joins. Widths from hydraulic geometry
  (w = 7 Q^0.5, Leopold and Maddock 1953) at `river_runoff`; each river's own discharge is its mouth's less its
  tributaries', so PhysXRiver3D adds them back at the joins.

Output: `get_soil_depth()`, `get_rock_exposure()`, `get_talus()`, `get_geology_at()`, `get_geology_image()` (RGBA
float), a texture published as shader globals `terrain_geology_map` / `terrain_geology_rect` with
`get_geology_shader_code()` giving `terrain_geology(world_pos)`, `get_rivers(min_area)` and
`build_river_paths(parent, min_area)` (PhysXRiver3D by name when present, else Path3D). TerrainDataMap3D is left as
it is: its `height_source` can point at the generator, and placement can read the geology straight from it. Bench: 34
checks, including rock on steep ground against cover on gentle ground, scree below repose, rivers downhill, widening,
joined, and discharges adding up. Cost: one pass over the grid, about 100 ms at 1025 squared on one core (the erosion run takes minutes), so generation time is unchanged within noise (257 squared: 5.1 s).

## Stage 4: what was built

`terrain_hydraulic.h` (plain C++, single thread per tile, fixed order), run at the end of `generate()` when
`tile_erosion_enabled` is on, over a fine grid (`tile_erosion_spacing`, 2 m) of the final ground (simulated plus
detail):

- Water: droplet erosion as Beyer describes it (2015, after Mei et al. 2007's capacity law): each droplet runs
  downhill with inertia over the bilinear surface, carries up to max(drop, min slope) x speed x water x Kc, erodes
  over a small disc below capacity and deposits above it, and evaporates. The virtual pipe model (O'Brien and Hodgins
  1995; Mei, Decaudin and Hu 2007) was tried first: on mountain slopes near 1 it cut grooves along the grid axes, and
  its sheet flow wore whole facets down evenly instead of cutting rills. Droplets gave branching rills and valley fills.
- Layers (Stava et al. 2008, two of them): loose cover (soil and scree from stage 5, plus what settles) erodes first,
  bedrock at `tile_erosion_rock_hardness`. Erosion slows to nothing as a cut nears `tile_erosion_max_depth`; without
  that, channels running out of a tile deepened by hundreds of metres.
- Thermal: loose material steeper than the repose angle slides to its lower neighbours in proportion to the excess
  (Musgrave, Kolb and Mace 1989; eight neighbours as Olsen 2004), eight passes.
- Tiles: cores of `tile_erosion_tile_size` with an apron of `tile_erosion_apron` on every side, each seeded by its
  index and eroded on its own on the worker threads, then blended with weights 1 over the core falling to 0 halfway
  out into the apron (the Wildlands approach: overlap and cross-fade, so droplets cut short at a tile's edge never
  show). The result does not depend on the thread count. Outputs: the height change (added in `get_height()`), droplet
  flow (`get_tile_flow()`, a stream and wetness measure) and deposits (`get_tile_deposit()`).

Bench (4 checks added, 38 in all): deterministic, bounded change (mean 4.8 m at 4 m spacing), no step at tile
borders larger than inside tiles, droplet flow over 5 times higher in valleys than on ridges. Cost: 4096 m square at
4 m spacing 2.2 s, at 2 m about 14 s (cloud, 4 threads); bake only, no frame cost. Look: rills branching down every
flank and flat alluvial floors in the valleys, raster and path traced alike.

## Detail in the material: what was built

`TerrainGenerator3D.get_detail_shader_code()` emits the detail filter's next octaves (below the ones in the geometry)
as shader code that returns a normal, not a height: `terrain_detail_normal(world_pos, world_normal, octaves,
footprint)`. Its slope is the filter differentiated analytically: the blended stripes (weights, phase and the
partial normalization), carried through the stacked fade and the stacked mask. Its C++ twin
`TerrainErosionFilter::gradient()` matches finite differences of `offset()` with correlation 0.99 over 20,000 points
(0.79 with the sine term alone, 0.85 with the stripes differentiated, 0.99 once the mask's slope is carried too).
Octaves shorter than about four pixels a wavelength fade out by the pixel footprint, so it does not shimmer at
distance. The bench view uses it with `--mdetail=4` (8, 4, 2 and 1 m over a 4 m mesh): fine gullies on every flank,
and the path traced frame keeps all of it (skylight shows it in the shadowed slopes the raster leaves dark). Cost:
25 cells and one sine and cosine each per octave per pixel; GPU timing is queued for the PC (cloud has no GPU).

## Coast and lakes: research

Asked for by the showcase scene, which had to cut its coast and lakes by hand.

How others do it:
- Landscape evolution models put the sea in as base level: every node at or below sea level is an outlet, so rivers
  end at the coast and the land behind grades to it (Braun and Willett 2013; Cordonnier et al. 2016 keep the coast
  fixed and let uplift raise the land from it; Yuan et al. 2019 add marine deposition offshore).
- Sub-sea shape: the equilibrium beach profile h = A x^(2/3) (Bruun 1954, Dean 1991; A about 0.1 m^(1/3) for fine
  sand, 0.2 for coarse) for the shoreface, then a gentle shelf. World Machine's and Gaea's coast tools and Houdini's
  heightfield erode all lower land below a sea level and smooth a beach band; none model waves.
- Lakes: closed basins either fill to their spill point and overflow, or stop where evaporation over the lake
  matches the water coming in (endorheic lakes). Fill-Spill-Merge (Barnes, Callaghan and Wickert 2021) solves this
  over a depression hierarchy; Priority-Flood (Barnes 2014) gives the spill levels. Lake floors are flat sediment
  with steeper margins; a depth growing as the square root of distance from shore is a common stand-in.
- Games: Horizon and Far Cry place lakes and the sea as authored water bodies over the terrain; Star Citizen's
  Planet Tech v5 fills its ocean to a planet-wide level and reads depth for its shore. Our ocean surface already
  reads the seabed through physics and needs a real depth for shore waves (water.md Step 5b).

Plan:
1. Sea: an `ocean_edges` mask (like `outlet_edges`) and an optional `sea_map` texture sink the land there below
   `sea_level` (negative uplift ramping in over `coast_width`). In the run, every node at or below sea level is an
   outlet. After it, the shoreface follows Dean's profile out to `shelf_depth`, and a beach band blends the land
   down to it.
2. Lakes: the run's basin fill marks closed basins. Each keeps water instead of sediment down to `lake_depth`
   (deepest at the centre, square root of distance from shore), with its level at the spill point, lowered when
   evaporation over the lake exceeds runoff from its catchment (the Fill-Spill-Merge balance for one basin).
3. Water: `get_lakes()` gives level, extent and outflow point; `build_water(parent)` makes a PhysXWaterSurface3D
   (by name) for the sea at sea level with shore waves, and one per lake at its level. Rivers stop at a lake or the
   sea (mouth_target set) and outflow rivers start at the spill point.

## Coast and lakes: what was built

`terrain_coast.h` (plain C++) and TerrainGenerator3D's Coast and Lakes groups.

- Sea: `ocean_edges` (like `outlet_edges`) puts sea within `coast_distance` of those edges, the coastline wandering
  by `coast_roughness` (fBm of `coast_wavelength`), corners rounded by a soft minimum of the edge distances;
  `sea_map` adds sea where it is white. Sea cells are the run's outlets, held at `sea_level`, so the land drains to
  the coast and rises from it. After the run a chamfer distance transform (Borgefors 1986) gives the distance
  offshore and inland. Below the sea: Dean's profile `seabed_profile * x^(2/3)`, plus `shelf_slope` past 300 m, with
  20% fBm relief, down to `max_sea_depth`. Above it: beaches along some stretches (`beach_width`, varying with
  noise from none, a cliff, to twice it) at `beach_slope`, blended into the land over 60 m behind. Land never
  lies at sea level: a coastal plain the run graded flat to its outlets is lifted to a beach floor (0.15 m plus
  `beach_slope` over `beach_width`, then 2 m per km), the detail fades in over the first 6 m above the sea, and a
  gully cut deeper than a quarter of the ground's height above the sea is compressed to at most half of it.
- Lakes: the plan was to keep the run's filled basins as lakes. In the bench they turned out to be sloping valley
  fills (20 to 100 m of relief in one fill), not basins: a stream power landscape drains completely. So lakes come
  from what holds most mountain lakes, a dam across a valley (moraine, landslide or rock bar; Costa and Schuster
  1988, Korup 2002). At a dam site the generator raises a dam `lake_depth` high across the valley (crest 3 m above
  the spillway, faces 0.35 and 0.2, half width ten times its height), cuts a spillway down the old channel, and
  floods the valley above in height order (Priority-Flood) up to the crest or the lowest col of the basin's rim,
  whichever is lower (then the col is the outlet). If runoff over the catchment (`river_runoff`) is less than
  `lake_evaporation` over the lake, the level drops to where they match and the lake has no outflow (the
  Fill-Spill-Merge balance for one basin). Sites: `lakes` markers first, then `lake_count` automatic ones: valley
  cells with at least `lake_catchment` above them, `lake_depth / 2` above the sea and away from it, each probed
  (built and taken back) and the largest lakes built first, `lake_spacing` apart.
- Around a lake (three cells) the detail and tile erosion cannot take ground that was above the water below it, so
  the shores and the dam hold the surface. Under water (sea and lakes) both fade out, so beds stay smooth.
- Rivers: routed again with the water (Kahn order): lake cells drain to their outlet, the sea ends everything.
  `get_rivers()` ends rivers at the first water cell, on the water (`ends_in` sea, lake, river or edge; `lake`;
  `from_lake`), and an outflow river starts at each lake's outlet carrying its whole catchment. Where a lake spills over a col
  rather than its dam, the outflow follows the flood's own path from the lake to the spill cell, so it never stalls
  on flat ground or loops back.
- Water: `build_water(parent)` makes PhysXWaterSurface3D by name: the sea at sea level over the generated
  rectangle (`seabed_from_floor`, `shore_waves`, `open_sea_beyond`, `follow_weather`, three `wave_cascades`, its open-sea `depth` the seabed's mean depth along the edge,
  `render_extent` out to 20 km or more), and one per lake at its level with `surface_mesh` set to the lake's outline
  (grid quads with a corner under water and none on lower ground outside it), `follow_weather` on. Without PhysX, plain mesh sheets. Call
  it before `build_river_paths()`, which then sets each river's `mouth_target` to the sea or lake it ends in.
- Queries: `get_lake_results()` (level, bed, depth, area, catchment, endorheic, center, rect, dam, outlet),
  `get_water_level()`, `get_water_depth()`, `get_water_image()`, `has_sea_result()`.

Cost (cloud CPU, one thread): coast, lakes (with up to 512 dams probed) and routing take 28 ms of a 3.9 s
generate() at 257^2 cells and 199 ms of 19.4 s at 513^2. Bench: `--test` (54 checks, 16 for coast and lakes),
`--coast --geology=map.png` (island map, lakes listed), `--out=... --coast [--focus=lake|coast]`. Media in
fork/terrain/coast/ (raster and path traced, same frames).

Limits: lakes in steep fluvial valleys are small (1 to 2 ha with 20 to 35 m dams in the bench); larger ones want a
marker on a flatter reach or a deeper dam. No glacial overdeepening, lagoons or deltas yet. The sea surface covers
the generated rectangle; its shore waves are baked on that grid (16 m cells for a 4 km world).

## Star Citizen Planet Tech v5 (Genesis): what it does that we do not

Paulo named Nyx I's planet tech as a bar (10 Nov). What CIG has shown (CitizenCon 2024 and 2025, monthly reports;
summarised on the Star Citizen wiki), against TerrainGenerator3D:

- Data pools over the whole planet: temperature and humidity (from v4), geology, soil type, soil depth, nutrients;
  sun exposure and aspect derived from them. Ours: soil depth, bare rock, scree, sediment, drainage, rivers, water.
  Missing: climate (temperature, precipitation or humidity, insolation), soil type and nutrients.
- Erosion simulated offline, then medium tiles (base tiles 4 km) refine height plus soil and bedrock by slope, soil
  and erosion. Ours matches: global stream power baked once, tile erosion, geology from it.
- Rocks and cliffs follow local geology, in families, so boulders match the cliff above them; debris collects at
  cliff bases from the erosion sim. Ours has the talus map, but nothing places rocks from it yet.
- Biomes are emergent: each species has needs and competes for water, nutrients, light and space, with growth
  stages and health. That is the vegetation thread's side (placement on our geology already started).
- Terrain material layers bedrock, soil, litter, snow; moisture changes the look and wet ground dries slowly. Distant
  terrain colour is drawn from the data, including vegetation that is not loaded, so far land matches near land.
- A virtual terrain cache at several scales; scatter generated on the GPU.
- Starchitect: searches the planet data for sites that meet a designer's rules (a farm needs workable soil and a
  mild climate), then assembles locations from modular tiles.
- Not shown in public: how they do rivers, lakes, coasts or overhangs, and any resolutions or costs.

Candidates for us, generator side: a climate pass (temperature by lapse rate, insolation from slope and aspect,
orographic precipitation by the linear model of Smith and Barstad 2004, giving humidity and snow lines); a site
query over our maps (`find_sites()` with required and preferred ranges); rock and debris placement from talus and
cliffs; a far-field colour map baked from geology and vegetation.

## Climate maps (Paulo picked 10 Nov 19:14)

One bake on `TerrainDataMap3D`: `bake_climate()` with a `ClimateRecipe` resource. It reads any height source the map
reads (TerrainGenerator3D, TerrainDeformer3D, Terrain3D by name, colliders), and water from `water_sources` or the
generator's sea and lakes, so it works on hand-made ground too. It has its own arrays and rectangle (the map's square
plus `context_margin`); `bake()` never touches them and `has_climate()` says whether they exist.

Methods, one per field:

- **Temperature.** A seasonal harmonic between January and July sea-level means, minus a lapse rate that itself moves
  between a winter and a summer value: surface lapse rates in the Alps run about 4 to 5 K/km in winter and 6 to 7 in
  summer (Rolland 2003), not the 6.5 standard atmosphere all year. Air over water uses the water surface. Mean,
  January and July are stored; any day comes from the harmonic.
- **Precipitation, linear theory of orographic precipitation (Smith and Barstad 2004).** Terrain in Fourier space
  times the transfer function `Cw i sigma h / ((1 - i m Hw)(1 + i sigma tau_c)(1 + i sigma tau_f))`, sigma = U k + V l,
  m the vertical wavenumber with Coriolis (real, signed by sigma, below the moist buoyancy frequency; decaying
  above it), Hw and Cw from the reference temperature (Clausius-Clapeyron vapour density, moist adiabat), tau_c =
  tau_f = 1000 s, N_m 0.005 /s. Background rate added, negatives truncated, as in the paper. Climatology: summed over
  a wind rose of wet-day winds (16 directions, Gaussian around the prevailing direction) times the wet hours a
  year, so a range gets a wet windward side, a rain shadow, and more on its crests. Solved on a coarse grid
  (`precipitation_cell`, 64 m) with tapered padding so the map edge is not a cliff, then upsampled.
- **Snow fraction.** The share of each month's precipitation falling as snow: daily temperatures spread around the
  month's mean (normal, 3 K), snow below the 1.0 C rain-snow threshold Jennings et al. (2018) found on average.
- **Aridity index (UNEP 1997).** Precipitation over potential evapotranspiration, PET by Thornthwaite (1948) from
  the twelve monthly temperatures and day length at the latitude (above 26.5 C, Willmott et al. 1985). Under 0.2
  arid, 0.5 semi-arid, 0.65 dry sub-humid, above humid. This stands in for "humidity" on the vegetation side.
- **Shortwave, annual and winter.** Sun path per latitude (Cooper 1969 declination, hour angle) on 24 days and half
  hours; horizon angles in 16 directions marched out to `horizon_distance` (Dozier and Frew 1990), so valleys lose
  the low sun; beam on the slope by the incidence angle, clear sky by Meinel with Kasten and Young (1989) air mass,
  diffuse by Liu and Jordan (1960) times the sky view factor (Dozier and Frew 1990), ground reflection, clouds by
  Kasten and Czeplak (1980). Stored as mean W/m2 over the year and over the three winter months.
- **Sky view factor.** From the same horizons; also useful for ambient occlusion of far terrain and cold pools.
- **Wind exposure, Winstral Sx (Winstral, Elder and Davis 2002).** For 16 wind directions: the largest upward angle
  to the terrain within `sx_distance` (100 m) upwind, averaged over a 30 degree window. The same measure SnowCover3D
  computes per wind; snow can read these instead of recomputing.
- **Aspect** is the existing `CHANNEL_ASPECT`; it is not repeated.

Channel layout (`TerrainDataMap3D.ClimateChannel`, its own enum, not `Channel` 0 to 10), one grid at `sun_cell`
(16 m default):

| channel | unit |
| --- | --- |
| CLIMATE_TEMPERATURE_MEAN, _JANUARY, _JULY | C at the surface |
| CLIMATE_PRECIPITATION | mm a year |
| CLIMATE_SNOW_FRACTION | 0 to 1 of the year's precipitation |
| CLIMATE_ARIDITY | P / PET |
| CLIMATE_SHORTWAVE, CLIMATE_SHORTWAVE_WINTER | W/m2, mean over the year / winter |
| CLIMATE_SKY_VIEW | 0 to 1 |
| Sx, 16 directions (separate) | degrees, wind from 0, 22.5, ... clockwise from north |

Queries: `get_temperature_at(pos, day_of_year = -1)`, `get_precipitation_at`, `get_snow_fraction_at`,
`get_aridity_at`, `get_shortwave_at`, `get_winter_shortwave_at`, `get_sky_view_at`, `get_sx_at(pos, wind_from)`,
`get_climate_at(pos)` (a Dictionary of all), `get_climate_image(channel)`. Shader globals `terrain_climate_map`
(RGBA half: mean temperature C, precipitation m a year, shortwave kW/m2, snow fraction) and `terrain_climate_rect`.

Snow keeps its own WGEN weather and snow state. Not modelled: cold-air pooling and inversions, continentality
within a map (maps are far smaller than the hundreds of km it acts over), seasonality of precipitation, fog.

### Climate maps: what was built (10 Nov)

As planned above: `ClimateRecipe`, `TerrainDataMap3D.bake_climate()` and its queries, `terrain_climate.h` (plain
C++, threaded rows, a small radix-2 FFT). Differences from the plan:

- **Rain shadow floor.** On an Alps-sized range (2000 m, sigma 15 km) the linear model gives 3 times the foot's
  precipitation on the windward slope (Saentis against Zurich is about 2.5) but leaves the lee at 10 to 20% of the
  background, drier than inner Alpine valleys (Sion about 60%): it knows only flow over the terrain, not showers
  or fronts. `precipitation_floor` (0.4) keeps that share of the background on every wet hour, as the
  `precip_min` of fastscape's implementation does.
- **Scale.** Rain drifts about wind speed times (tau_c + tau_f), 20 km by default, so on maps under about 20 km it
  falls past the crests (the model's spillover) and no rain shadow shows; precipitation then follows the range as
  one broad patch. Temperature, snow share, sun and Sx carry the detail on small maps. Linear theory also wants
  N h / U at or below 1; much taller, slower cases come out too dry over the crest.
- **Clouds.** The clear-sky global is Meinel beam plus the Liu and Jordan diffuse share; clouds scale it by Kasten
  and Czeplak and raise the diffuse share to 0.3 + 0.7 n^2. One mean cover all year.

Cost (cloud CPU, threaded): the 4 km bench world at 16 m (256 x 256, precipitation at 64 m) 0.43 to 0.49 s; a
24 km square at 64 m (375 x 375) 0.47 s. Horizons dominate; `horizon_distance` 0 drops them. Memory: 25 floats a
cell (9 maps, 16 Sx). Nothing renders differently: the `terrain_climate_map` globals only exist once baked, and no
material reads them yet.

Checks (`--test`, 15 of 68): lapse-rate temperatures at the top of a hill (January -3.75 C, July 12.25 C); south
face gets twice the north face's winter sun; flat ground 188 W/m2 a year (46 N, half cloud); open sky on flat
ground; Sx +17 degrees in the lee, -16 windward; snow share 0.35 on top against 0.05 below; on the range, windward
6000 mm against 1900 at the foot, lee 540, wettest point 12 km upwind of the crest, aridity lower in the lee;
`bake()` keeps the climate; the generated world bakes. Maps: `--climate=map.png [--view] [--wind=270]`
(temperature, precipitation, winter sun, snow share). Media fork/terrain/climate/.

## Faster generation: coarse to fine (Paulo picked 10 Nov 20:11)

Profile first (cloud CPU, one thread): a step costs 11 ms at 257^2 and 84 ms at 513^2, spread over receivers, the
height sort for multiple-flow drainage, the area pass, diffusion and the occasional Priority-Flood; no one part
dominates, so a faster step alone would gain little. The outlet test was cached per cell (it ran hundreds of
millions of times; about 4%).

What others do: Tzathas, Gailleton, Steer and Cordonnier (Physically-based analytical erosion for fast terrain
generation, CGF 43(2) 2024) solve the steady state of the same stream power law along the flow tree, and to make
it converge fast they cycle through resolutions: inputs averaged 2x2 down to a small grid, a few iterations
there, bilinear upsampling (nodes jittered by up to a quarter cell) and more iterations at each finer level, "the
simplest V-cycle". Their table: 512^2 in 1.8 s against 7.9 s for the fixed point and 56 s for a time-stepping
simulation; 2048^2 in 34 s against 4800 s. We keep our time-stepping solver (it carries the critical slope,
hillslope diffusion, multiple flow directions and the sediment fill the analytical form approximates) and borrow
the cycle:

- `coarse_levels` (-1 auto: halve while the coarse grid keeps 129 nodes a side): `steps` run on the coarsest grid,
  where relief and drainage form; uplift restricted by full weighting, sea and outlets by injection (node grids,
  so coarse node j is fine node 2j).
- Each finer level starts from the bilinear upsample of heights and sediment plus the initial roughness again (in
  place of Tzathas's jitter, so flats break) and runs `refine_steps` (60) to grow the valleys, hillslopes and sharp
  crests that resolution allows. Deterministic as before.

Result against single-level runs of the bench range (same seed, compare with
`--compare=prefix [--cell=..] [--refine=..]`):

| grid | single | coarse to fine | heights p50 / p99 / max | mean slope | crest cells | drainage cells |
| --- | --- | --- | --- | --- | --- | --- |
| 257^2 (16 m) | 5.2 s | 2.5 s | within 0.4% | 1.000 | 0.98 | 0.98 |
| 513^2 (8 m) | 26 s | 8.4 s | within 1.2% | 1.001 | | 1.00 |
| 1025^2 (4 m) | 155 s | 42 s | within 1.4% | 0.997 | | 0.98 |

(2.1, 3.1 and 3.7 times faster; times include the detail, geology, coast and lake passes, which do not change, so
the erosion alone speeds up more. Crest cells are only counted reliably at 16 m.)
Heights blurred over 250 m correlate 0.993 to 0.997 with the single-level result. With 40 refine steps it is 2.4,
3.9 and 5 times faster, but crests come out about 8% rounder at 16 m, so 60 is the default (the floor rule).
`coarse_levels = 0` gives the old result exactly.

Found on the way: with a sea, the finest level kept the coarse outlet mask (a bug the checks caught as a
run-to-run difference; fixed before merging). Rivers ending in a lake could stop 0.35 m above its water where the shore's detail lifts the
ground in the shallowest margin; a mouth now sits on the water.

## Soil on mountainsides (fix, 10 Nov)

The trees thread found every mountainside of the bench range bare: soil 0 and rock 0.9 to 1 on all slopes over
about 30 degrees. Two defects in the stage 5 soil: the erosion rate fed to the soil production function counted
stream incision K A^m S on every cell, so hillslope cells read their own small area as a stream (0.3 to 0.5 mm a
year against 0.5 mm of production); and soil faded out from 60% of the critical gradient (27 degrees). Against
the literature: soil-mantled hillslopes hold up to near the critical gradient (Roering, Kirchner and Dietrich
1999), and threshold hillslopes of fast-eroding ranges keep thin forested soils (Heimsath et al. 2012; Larsen et
al. 2014), with rock on sharp crests and cliffs. Now:

- incision counts on channels only (20 to 200 cells of drainage, above the channel heads of Montgomery and
  Dietrich 1988 hillslopes lower by creep and landslides);
- creep is nonlinear (Roering et al. 1999, transport over 1 - (S/Sc)^2), so steep sharp crests lower fast and
  go bare while rounded hilltops keep soil; curvature over a 3x3 window (the one-cell zigzag of threshold slopes
  checkered the soil);
- soil thins from 85% of the critical gradient, a third left at it, none past 110% (cliffs).

Hillslope cells of the bench range (`--soil [--uplift=6]`; drainage under 1e5 m2, 40 m above the lowest ground):

| slope | before: soil / rock / share with 0.2 m | after |
| --- | --- | --- |
| 20-25 | 0.13 m / 0.46 / 41% | 0.94 m / 0.17 / 77% |
| 25-30 | 0.07 / 0.63 / 7% | 0.86 / 0.19 / 71% |
| 30-35 | 0.02 / 0.67 / 0% | 0.75 / 0.30 / 56% |
| 35-40 | 0.00 / 0.72 / 0% | 0.68 / 0.36 / 54% |
| 40-45 | 0.00 / 0.96 / 0% | 0.59 / 0.20 / 77% |

(uplift 2; with uplift 6, 30-35 degrees goes from 9% to 87% of cells with soil, 40-45 from 0% to 65%). The relief
is unchanged; only the geology maps move. Map fork/terrain/soil_before_after.png. The slope distribution itself
(40% of hillslope cells at 35 to 45 degrees, piled at the 40 degree `critical_slope`) is a threshold landscape;
real ones sit nearer 30 to 35 degrees (Montgomery and Brandon 2002), which `critical_slope = 35` gives.

## Waterfalls from rock layers (Paulo picked 10 Nov 21:47)

The showcase scene cuts its waterfall by hand. Real falls in uplifting ranges sit where a river crosses a band of
hard rock over soft (Niagara, the Yorkshire Dales forces, the Tatra and Alpine trough steps): the river cannot
grade the hard band as fast as the soft rock below, and where the soft rock under the band is exposed in the face
it is undercut, the band breaks off in blocks and the face stays vertical as it retreats upstream (Gilbert 1907).

Research:

- Forte, Yanites and Whipple 2016 run stream power over horizontal hard and soft layers: knickpoints form at every
  contact, channel steepness switches with the rock, and divides migrate. Plain stream power makes steep hard
  reaches, not vertical steps: with n = 1 a band of K/6 only grades six times steeper (for a big river, 0.02 to
  0.12), a rapid hundreds of metres long.
- Haviv et al. 2010 model vertical knickpoints with a resistant caprock: the face retreats by collapse of the
  caprock once the weak rock under it is eroded, at a rate set by the discharge, not the slope; the fall keeps its
  height (the caprock thickness plus the weak rock under it) as it retreats.
- Lamb and Dietrich 2009: falls persist where the rock fails by toppling of blocks once undercut; Scheingross and
  Lamb 2017: plunge pool erosion at the foot drives the undercutting.
- Perne et al. 2017: in steady state over dipping layers the channel steepness matches each layer's erodibility,
  with transient steps where contacts cross the channel.
- Whipple and Tucker 1999: with n = 1 a knickpoint travels upstream at K A^m, independent of its height. So a face
  that erodes as soft rock retreats at the soft rock's celerity and keeps its form.

What is built (stream power, terrain_stream_power.h, per cell; `strata_enabled`, off by default):

- strata: bands `strata_spacing` (90 m) apart, `strata_hard_fraction` (0.35) of each hard. The rock at a node is the
  band at its stratigraphic height q = h - U t + tan(dip) (p . dip direction) + wobble fbm (the rock rose with the
  uplift); contacts about 2 m thick.
- hard rock erodes `strata_hardness` (100) times slower everywhere. Erosion goes as tensile strength^-2 (Sklar and
  Dietrich 2001), so 100 is a 10x strength contrast, mudstone against sandstone or quartzite. Below about 20 the
  rivers grade every band into a rapid hundreds of metres long (plain stream power, as Forte 2016 found).
- undercutting: a hard node whose receiver lies below the bottom of its band by more than a soft slope at the
  critical gradient stands on worn-back soft rock. Its block breaks off whole (the node drops to the band's base)
  and it erodes on as soft rock. The face retreats a cell per undercutting, at the pace the foot incises (Haviv
  2010), and keeps the band's height as a fall.
- only channels (drainage over `strata_channel_area`, 2e4 m2, about where channels begin) stand at
  `strata_cliff_angle` (75 degrees) in hard rock and stop creeping; the soft cell right under a hard lip stands as
  steep (the plunge pool's undercut, Scheingross and Lamb 2017). A first version gave every hard cell cliffs and no
  creep: hillslopes became fields of rock pinnacles. Hillslopes in hard rock now erode slowly but fail at
  `critical_slope` like soft ones.
- `strata_dip` 1 degree by default: at 3 degrees (near the rivers' own gradient) rivers run along one band for
  hundreds of metres and grade it.
- `get_rock_hardness(pos)` (the rock as it crops out, hillslopes too) and `get_waterfalls(min_area, min_height)`:
  runs of a river's cells steeper than 45 degrees in the eroded heights (not the smoothed river points, whose
  bilinear samples cut corners). Soft rock never stands past `critical_slope`, so only hard bands make them.

Bench (4 km range, 16 m cells, `--strata`): 6 falls of 8 to 30 m on the rivers draining over 1e5 m2, four over 15
m, every one in a hard band; long profiles step down between flat reaches (fork/terrain/waterfalls/
waterfalls_before_after.png). Ground over 60 degrees stays under 0.5% of the land (no pinnacles). The range
stands higher than in one rock: the hard bands hold the rivers up (relief rises with the mean rock strength, Forte
2016); lower the uplift or raise `erodibility` to keep a height. Cost: generation 2.51 s to 2.98 s (+19%; the
receivers pass and per-face creep). Off, the output is byte-identical (bench check). Not done: the falls' water
(the showcase scene builds it from get_waterfalls), overhangs (a height field has none), plunge pools.

## Rain and snowmelt feed the rivers (Paulo picked 10 Nov 22:49)

Today every river carries `river_runoff` (1000 mm a year) times its drainage area: the same depth of water off a
glacier, a dry lee slope and a lake, all year. The climate maps already know temperature by month, precipitation
(with the rain shadow), snow share and sunshine per cell, so the flow can come from them.

Research (what hydrology models do at this scale):

- Monthly water balance models: Thornthwaite and Mather 1957 (soil water bucket, withdrawal ST = ST0
  exp(-deficit/STC)) and the USGS monthly water balance model of McCabe and Markstrom 2007 (OFR 2007-1088): rain
  and snow split by temperature, a snowpack store, direct runoff 5% of rain, soil capacity about 150 mm, and half
  the surplus running off each month (the rest carried over: a linear groundwater reservoir). Used for continental
  runoff maps; works on monthly means only, which is what the climate maps give.
- Snowmelt by temperature index (Hock 2003): melt = DDF x positive degree days, DDF 2.5 to 6 mm/day/K for snow;
  Hock 1999 adds the sun (melt = (MF + a I) T), so south slopes melt out first. Positive degree days of a month
  from its mean and the daily spread, analytically (Calov and Greve 2005), the same normal spread the climate maps
  already use for the snow share.
- Rain or snow per day below the rain-snow threshold (Jennings et al. 2018, 1 C), as the climate maps do.
- Open water: a lake loses its evaporation (`lake_evaporation`, spread over the months by PET).
- Routing: flow accumulation weighted by runoff along the river receivers (as TopoToolbox and global models like
  WBM do). A month is longer than travel times on a few tens of kilometres, so no channel lag; the groundwater
  reservoir gives the delay (a single linear reservoir, the first stage of a Nash 1957 cascade).
- Engines: UE5 Water, HDRP and Houdini Terrain take river flow as an artist input or a uniform rain; World Machine
  and Gaea's erosion use uniform rain; none of their docs tie river flow to a climate.

Plan:

1. `TerrainClimate::thornthwaite_pet` returns the months too; a new terrain_runoff.h runs the monthly balance per
   climate cell: rain and snow split, Hock 1999 melt with that month's sunshine, Thornthwaite-Mather soil bucket,
   direct runoff and the groundwater reservoir, three years of spin-up. Out: twelve months of runoff and snowpack
   (snow water equivalent) per cell. Precipitation by month: the year's total on a harmonic with new
   ClimateRecipe settings `precipitation_seasonality` (0 = even) and `precipitation_peak_month`.
2. TerrainGenerator3D gets `climate_recipe`. With one, generate() bakes the climate on its own heights (sun cell,
   lakes and sea as water) and accumulates each month's runoff along the river routing, storing monthly
   discharge for channel cells only (a compact index, not twelve full grids). Without, nothing changes.
3. Rivers, waterfalls and lakes use it: `get_rivers` gives `discharge` (annual mean) and `monthly_discharge`;
   `river_month` (-1 the annual mean) picks the month build_river_paths uses, so a spring flood can be shown;
   the lake balance uses the map's mean climate runoff. New: `get_runoff_at(pos, month)`,
   `get_discharge_at(pos, month)`, `get_snow_water_at(pos, month)` (the snow thread can start its cover from it).
4. Bench: water balance closes (runoff + evapotranspiration + change in storage = precipitation), snowmelt peak
   later with height, wet windward rivers carry more per km2, lake-less river flow equals summed runoff, cost.

### Rain and snowmelt: what was built (10 Nov)

As planned, in terrain_runoff.h (`TerrainRunoff::balance`) and TerrainGenerator3D (`climate_recipe`,
`river_month`, `get_runoff_at`, `get_snow_water_at`, `get_discharge_at`, `get_mean_runoff`). The climate is baked
on the generator's own result at the recipe's `sun_cell` (TerrainDataMap3D and the generator now share
`TerrainClimate::precipitation_on`). Channel cells (8 cells of drainage or more) keep 12 monthly discharges.
New ClimateRecipe settings: `precipitation_seasonality`, `precipitation_peak_month`, `melt_factor` 1.8,
`radiation_melt_factor` 0.015, `soil_capacity` 150, `direct_runoff` 0.05, `groundwater_release` 0.5.

Bench (`--runoff[=csv]`, 4 km range, cold climate -4 / 15 C, 1100 mm): mean runoff 711 mm a year; the trunk river
peaks in April at 0.23 m3/s (snowmelt), 9x its August low; snow lies to the end of March on the crest, February at
the foot. A mild climate with even rain runs high in winter (February); summer rain (seasonality 0.9, peak July)
moves the high water to September, after the soils refill. Lakes balance on the map's mean runoff and lose their
open-water evaporation month by month. Cost: 230 ms of 2.7 s generation at 16 m cells (8%); none without a
recipe, and the output is then unchanged. Media fork/terrain/runoff/. Not done: glaciers (cells that never melt
out keep their snow; no ice flow), channel travel time within a month, rain shadow on maps smaller than the
rain's drift (about 20 km, see Climate maps).

Found on the lake shots: the ring that keeps detail from cutting below a lake's level (3 cells, nearest node) left
flat terraces with walls stepping along the cell lines on the dam flanks. The hold is now full within one cell of
the water and fades out over the next two (bilinear weight), so the detail runs on beside the lake
(fork/terrain/runoff/lake_rim_before_after.png).

## Valley floors: rivers that deposit (Paulo picked 11 Nov 10:33)

The showcase wants a wide, flat, grassy valley floor with the river from a high lake running through it to the sea.
The stream power model only cuts, so every valley is a V and every low area is hills.

Research:

- **Yuan, Braun, Guerit et al. 2019** (JGR Earth Surface, "A new efficient method to solve the stream power law
  model taking into account sediment deposition"): adds `(G / A) Qs` to `dh/dt = U - K A^m S`, Qs the sediment
  flux from upstream, G dimensionless. It stays implicit and O(n) per pass; Qs depends on the new heights
  upstream, so the solve is repeated (Gauss-Seidel) until it settles. FastScape and goSPL use it. Where all the
  ground rises, steady-state slopes grow by 1 + G (more relief); where ground does not rise below the mountains,
  rivers drop their load and build alluvial plains and fans. Chosen: same solver, same cost order.
- **Davy and Lague 2009** (the xi-q model): erosion-deposition with a transport length; Yuan's G is its efficient
  form for n = 1.
- **Transport-limited and undercapacity models** (Whipple and Tucker 2002): explicit, need small time steps;
  far too slow at our 20,000-year steps.
- **Flood filling to a level** (non-physical): flat but dead, rivers cross in straight lines; no.
- **Glacial erosion** (Cordonnier et al. 2023, U valleys, cirque lakes): deferred by Paulo.

Built (`deposition` on TerrainGenerator3D, the solver in terrain_stream_power.h):

- The first pass is the old incision-only solve. Then each pass sums the eroded volume down the receiver tree
  and solves `h_i (1 + F + G a/A_i) = h*_i (1 + G a/A_i) + F h_r + G Qs_up / A_i` from the outlets up.
- A is the drainage area along the same single receiver tree as the flux. The multiple-flow area can be far
  smaller on a cell the tree funnels into, and dividing by it piled sediment there and the passes never settled.
- The caprock undercut is decided once per step, on the first pass; a block breaking off and back in turn made
  passes swing by its whole thickness.
- Passes are under-relaxed by half: the full update swings between two states when a step moves a lot of
  sediment. The change then halves each pass; it stops under 5 cm or after 12 passes.
- Alluvium (ground raised above the uplifted rock, worn first when the river cuts back) goes into the sediment
  map, so `get_sediment_depth` and the geology's alluvium follow it. With deposition 0 nothing changes, byte for
  byte.

Results:

- Showcase valley layout (`docs/bench/landscape --layout=valley --map=... --deposition=G --base-uplift=0
  --floor-uplift=0 --profile`). With 0 the floor is flat at 2.5 m and rivers run in grid lines. With 0.5 an
  alluvial plain about 40 m high slopes gently to the sea, flat across, with winding rivers. At 1 and above the
  fans climb the range front.
- The scene's floor was hills mainly because `base_uplift` 0.1 lifts it: a floor needs ground that does not
  rise. Deposition then shapes it.
- terrain_gen bench: with the whole world rising, the highest point goes from 343 to 480 m at 0.5, as Yuan
  predicts. With `base_uplift` 0, 45% of the land ends up on over 1 m of alluvium.
- Cost: 2.6 to 3.5 s (+35%) on the rising world, and 1.9 to 3.4 s on still ground, where most passes run to 12.
  Since the direct solve (below): deposition itself costs about 0.4 s of a 3 s generate.

Cheaper deposition solve (11 Nov, Paulo's pick): the passes were most of deposition's cost (about 7 a step,
2,638 on the bench's rising world at G 0.5; each a flux sweep and a solve sweep). Tried first, all on the passes:
starting each step from the last step's flux (more passes, 4,385: receivers move between steps), a relaxation
weight per node that halves where the change flips sign (2,846), and Aitken's dynamic relaxation (Irons and Tuck
1969, as in partitioned fluid-structure coupling; 2,146). None fixes the cause: Gauss-Seidel solves a system that
needs no iteration. With n = 1, Yuan's equation is linear in the heights along the receiver tree: the flux into a
node is what its donors pass on, each affine in the node's own height once their subtrees are. One sweep from the
sources down writes every node as `h = alpha + beta h_receiver` (and its outflow likewise); one sweep from the
outlets up gives the exact heights. Only the critical slope (a min) is not linear: nodes the incision-only solve
left at it are taken to stay there (`h = h_r + S_c d`), the sweep up takes the lower of the two, and the old
passes then settle any wrong guess. Measured (`terrain_gen -- --deposition-time`): 903 passes against 2,638
(rising world, G 0.5) and 1,141 against 3,555 (G 1); with no clamps the first check pass moves under 2 cm. The
deposition part of a generate drops from about 0.73 to 0.40 s; per-node terms that do not change over a step's
passes are also computed once (byte for byte the same result). The terrain changes as a long simulation does
when its steps differ by centimetres: on the showcase valley the plain, fans and winding rivers keep their
character, but rivers sit in other places (one capture moves the high lake east).

A lake above a waterfall (fix, 11 Nov): the showcase put a lake marker 120 to 300 m up a river with a 34 m fall,
and the fall vanished. Marker dams are landslide dams (Costa and Schuster 1988) with a 0.35 downstream face; on a
steep reach that apron runs hundreds of metres down the valley and buries the fall's lip. Lakes above falls in
nature sit behind a bedrock lip (cirque and hanging-valley sills). Markers now take `rock_lip` metadata (a face of 3,
about 72 degrees) or `face_slope`. On the valley layout, a 60 m marker dam 250 m above two falls (31 and 26 m)
buried both; with `rock_lip` both stay and the lake spills over its own 70 m lip.

Detail on alluvium (fix, 11 Nov): the showcase valley showed trenches along the floor's rivers and a comb of
gullies on the fan toe. The erosion detail faded over the first 2 m of alluvium, but rivers keep their own cells
nearly bare (the alluvium sits beside them) and the toe's deposit thins to decimetres, so the detail cut 5 to 20 m
there. An aggrading surface buries gullies, so the fade now saturates at 0.5 m, holds 32 m past the deposit (a
channel and its banks count as the deposit) and thins out over 3/8 of `detail_wavelength` (two chamfer sweeps,
Borgefors 1986). The material's detail normal used to ignore every fade (alluvium, water, the coastal strip) and
drew a ripple pattern over the plain; the generator now publishes the share of detail each node keeps
(`terrain_detail_keep_map`) and the detail normal scales by it. Still open: short one-cell slots in the base
heights at the fan front, from the solver itself (rivers re-cutting the toe each step); more sediment diffusion
(Fastscape's separate sediment diffusivity) removed them but left a jagged scarp, so it needs more work.

Fan slots (11 Nov): fans backfill the channels a river abandons (avulsion; Bryant, Falk and Paola 1995; Jerolmack and
Paola 2007), but a single-direction solver leaves each abandoned one-cell trench open. `fan_backfill` (on) fills,
after the last step and before the final routing, alluvium cells (at least 0.3 m of sediment) that carry less than a
river's area (0.1 km2) and sit at least 0.5 m below six or more of their eight neighbours, to the median of the
neighbours (an edge-keeping filter, so terrace steps and scarps stay), two passes. On the valley layout it fills 9
cells by 1.5 to 8 m (a three-cell slot across the plain at (-400, 512)); lakes, falls and rivers are unchanged.
Active channels still move between runs with the step count: that is the river migrating, not a trench.

## One main river: inflow from beyond the map, and caprock lips (11 Nov, Paulo's pick)

The valley's rivers carried about 0.1 m3/s at most: the 16 km2 tile holds catchments of 2.6 km2 at best, so no fall
can be a curtain and no river dominates the plain. Real valleys are fed from far upstream.

Research:

- Landscape models with open boundaries take water and sediment in at boundary nodes: Landlab's flow routing takes a
  per-node water input (`water__unit_flux_in`), FastScape and goSPL a precipitation or inflow field. Drainage area
  is the stream power law's proxy for discharge, so an inflow of land is extra area at a node.
- Large rivers are graded (Mackin 1948): with 60 km2 coming in at the map edge the river cut a gorge through the range
  to sea level and its falls went to the side streams. Falls on large rivers sit where a hard caprock lip holds them
  (Niagara; Gilbert 1907; Haviv et al. 2010), which the strata model already makes.
- A free-falling jet needs a face it clears: a vertical or undercut lip. The jet leaves the brink near critical depth
  (Rouse 1936), at v = (g q)^(1/3) per unit width, and travels v sqrt(2H/g) out over a drop H. A steep face that
  recedes further than that is a cascade (horsetail), not a plunge.

Built:

- `inflows` (markers, metadata `area` in km2, default `inflow_area` 25): land beyond the map draining in at the
  marker's node (the nearest node off an outlet edge or the sea). It counts as drainage area in every erosion step
  and level, in deposition, in the rivers' routing and in the monthly runoff (at the climate of the node it enters
  at), so discharge and the water balance stay one. `get_inflow_cells()` lists them. No sediment comes in with it.
- `fall_lips` (default on): falls of 4 m or more over rock at least half hard keep a vertical face. The grid holds a
  drop over a whole 16 m cell at best (25 m drop: about 58 degrees), so the ground holds the lip's height to the
  middle of the drop and the foot's after it, the step taking 3 m (a 1 m step oblique to the 1 m grid aliased into vertical ribs), its line wandering by up to 2 m, across the river's width plus 12 m fading into the
  slope. `get_waterfalls()` now gives `face_run`, `jet_throw` and `free_fall` by the rule above.
- Showcase valley (`--layout=valley --inflow=60 --inflow-at=lake`): 60 km2 into the high lake gives its outflow 2.2
  m3/s (3.0 at the spring peak); it falls 63 m over a caprock lip (jet 4.6 m out, face 1 m) and the plain's streams
  join it as one main river to the sea. At the north edge instead (`--inflow-at=edge`) it cuts a graded gorge with no
  fall. The river module tests clearance against its sections' bed (4 m ground body in the bench, sections apart),
  so it still sees the face as a ramp; sampling the face by raycast as the lip already is would make it agree.

## Glacial erosion: cirques, troughs, hanging valleys (research, 11 Nov; waits on Paulo's pick)

Why: the valley wants a high lake above a big fall on its main river. Fluvial erosion alone cannot hold it: a river
of 2-3 m3/s grades its bed down (the inflow tests above), so a pinned sill gives 250-380 m falls or is bypassed. In
mountains, high lakes above big falls are mostly glacial: cirque basins (overdeepened at the glacier's head, Barr
and Spagnolo 2015) and hanging valleys (a tributary glacier, thinner and slower, erodes less than the trunk; its
valley is left above the trough when the ice goes, and its stream falls off the lip, as in Yosemite).

Established techniques:

- Cordonnier, Jouvet, Peytavie, Braun, Cani, Benes, Galin, Guerin, Gain 2023, "Forming Terrains by Glacial
  Erosion" (ACM TOG 42(4), SIGGRAPH): ice flow from a learned emulator of the Stokes equations (IGM, Jouvet et al.
  2022), erosion rate proportional to sliding speed squared, alternated with fluvial erosion over glacial cycles;
  makes U valleys, cirques, hanging valleys and lakes on a heightfield. The reference for us.
- Shallow ice approximation (SIA: Mahaffy 1976; mass-conserving MUSCL scheme, Jarosch, Schoof and Anslow 2013):
  ice flux from thickness and surface slope, explicit, cheap on a grid; the usual stand-in for Stokes flow in
  landscape models (Braun, Zwartz and Tomkin 1999; Herman and Braun 2008; Egholm et al. 2009).
- Erosion law: abrasion and quarrying at rate K_g |u_b|^l with l about 1 to 2 (Humphrey and Raymond 1994; Herman et
  al. 2015), u_b the basal sliding speed.
- Mass balance from the climate: accumulation above the equilibrium line (ELA), melt below, linear in height (the
  climate recipe already has temperature by height and snowfall).

Plan (if picked): an ice stage in terrain_stream_power's loop: SIA ice thickness on the same grid (MUSCL fluxes,
adaptive time step), sliding speed as a share of the deformation speed, glacial erosion added to the stream power
solve for the glacial part of each cycle, then fluvial steps on the ice-free ground; parameters `glacial_ela`,
`glacial_erosion`, `glacial_cycles`. Expected cost: SIA needs small time steps (CFL on diffusivity); on the 257 grid
a few hundred ice steps per cycle, about a second; coarse levels first as for the rivers. Checks: a cirque basin
holds a lake by itself (no marker), a tributary hangs above the trunk, U cross-sections (width over depth).

### Glacial erosion: what was built (11 Nov, Paulo picked the stage at 14:55 and the high bowl at 15:59)

- `TerrainGlacial` (terrain_glacial.h): shallow ice on a 64 m grid, flux q = -Gamma H^5 |grad s|^2 grad s with the
  thickness upwinded on each face; steady state by pseudo-time Jacobi with a local step min(0.03 dx^2/D, 20 yr)
  (6000 iterations on the first solve, 200 warm after). Mass balance from `glacial_ela` and a gradient of 0.006 per
  year, capped at 2 m a year.
- Sliding speed from the flux (|q| / max(H, 10 m), blurred over 3 ice cells); the H^4 S^3 deformation speed with
  the valley walls' slopes ran away. Erosion K_g u^l with K_g 2.7e-7 and l 2.02 (Herman et al. 2015), none on
  adverse slopes steeper than 1.5 times the ice surface (Alley et al. 2003), in sub-steps of at most 25 m.
- Interleaved: every 8 river steps the ice wears 8 dt `glacial_share` years, then a last glaciation of
  `glacial_final` years. A rockfall clamp after it keeps slopes under `strata_cliff_angle` (75 degrees) by raising
  the cell below, one sweep from the top down (Schmidt and Montgomery 1995 threshold hillslopes).
- Hollows the ice dug stay unfilled after routing and become lakes (depth at least 1 m, area at least
  `lake_min_area`); other hollows still fill.
- Sill markers with a level are a hard band (rock at most 1 m over the level does not wear). A range path with
  `basin` metadata lowers uplift under it (raised cosine), so a bowl can sit behind a bar.
- Result on the valley layout (bowl at (350, -1850), bar at 450 m, ELA 450 m): a lake at 450 m, 43 m deep, 1.9 ha;
  the main river (3.6 m3/s) leaves it over the bar and falls 333 m, free of the face. Ice costs about 1.3 s.
- Not yet: the lake is small (the bowl is narrow), the bar runs straight, and lake banks are bare rock.

### Ledges on cliff faces (11 Nov)

A heightfield cliff is a smooth curtain. Bedded rock breaks into steps: the hard bed stands as a riser, the soft
bed under it weathers back into a ledge (the terrace filters Gaea and World Machine use do the same). `cliff_ledge_spacing` (default 0, off) and `cliff_ledge_strength` terrace slopes between 40 and 55
degrees into beds of that spacing, the bed line wobbling with value noise, each bed a narrow ledge (20 to 35 percent of the bed,
rising a quarter of its height) under a riser; bed thickness varies by a monotonic warp of height. Wide ledges (60
percent of each bed, the first try) read as rice terraces close up. Alluvium no longer fades detail on slopes steeper than repose.

### Fall data for the water (contract agreed with the water thread, 11 Nov)

Each entry of `get_waterfalls()` now also has: `id` (river and distance down it, stable for a seed), `direction`,
`lip_points` (8 across the brink) and `lip_share` (Manning: unit discharge as depth^(5/3) under the critical-depth
surface), `lip_width`, `overhang` (0: a heightfield cannot undercut), `caprock` (hard bed thickness),
`face_profiles` (3 lines of 16 samples), `tiers` and `ledges`, `type` (plunge, tiered, horsetail, cascade, after the
World Waterfall Database), `power` (rho g Q H), `breakup_length` (Horeni 1956: 6 q^0.32), `impact_speed` (drag
limited: past break-up the drops close on about 9 m/s terminal speed over v_t^2/g, Gunn and Kinzer 1949),
`impact_speed_free` and `discharge_months`.

## Plunge pools (research, 11 Nov; ordered by the water contract)

Why: a fall lands in a pool, and the water code needs one to put its foam, mist and churn in. Today the foot is the
ground the river left, flat or sloping.

Established techniques:

- Mason and Arumugam 1985 (free jet scour below dams, J. Hydraulic Engineering 111(2)): from 47 model and prototype
  data sets, scour plus tailwater depth D + Y = 3.27 q^0.60 H^0.05 h^0.15 / (g^0.30 d^0.10), q the unit discharge,
  H the drop, h the tailwater depth, d the bed grain size. The usual engineering estimate of ultimate scour.
- Pagliara, Hager and Minor 2006 (plane plunge pool scour, J. Hydraulic Engineering 132): the pool's length is
  several times its depth and its shape similar across flows, upstream wall steep, downstream ridge gentle.
- Bollaert and Schleiss 2003 (comprehensive scour method): rock breaks by jet pressure fluctuations in joints;
  scour stops once the jet has diffused through the pool depth (core gone after 4 to 6 jet widths).
- Scheingross and Lamb 2016, 2017 (JGR Earth Surface): bedrock pools deepen by sediment the jet swirls against the
  floor, and widen as the fall retreats; pool depth grows with discharge and drop, and stops where the jet can no
  longer move the bed grains out.

Plan: for every fall the rivers make (drop at least 4 m), bake a pool at the jet's impact point (the brink plus
the jet throw, at least past the face), depth from Mason and Arumugam with d = 0.3 m cobbles and the tailwater the
river's normal depth, clamped to 1 to 15 m, radius max(lip width / 2 + 2 D, 3 D) after Pagliara's shape. The 1 m
ground takes min(ground, foot - D (1 - (r/R)^2)), so the pool holds water to its lowest rim. New keys:
`pool_center`, `pool_radius`, `pool_depth`, `pool_level` (the lowest rim). Off with `plunge_pools = false`.

## Spray potential (11 Nov; the static half of the water thread's spray wetting)

No published model gives the spray field of a natural fall. Measurements around plunging jets below dams (flood
discharge atomization) show spray intensity falling off roughly exponentially with distance from the impact, and
the reach growing with the jet's energy. We bake that as a static map: `terrain_spray_potential` (L8, `spray_cell`
4 m over the result, rect in `terrain_spray_rect`). Each fall of 4 m or more sprays from its path in plan (lip to
impact) with weight 0.35 to 1 growing down the drop, peak 0.25 to 1 by log10(1 + Q H), falloff exp(-2 d / R) with
R = 3 (Q H)^0.4 m (at least 4 m; about 50 m for the 3.6 m3/s, 333 m fall, about 300 m for the Victoria Falls,
matching the width of its spray forest), fading with height above the lip. Falls combine as 1 - prod(1 - p). The
water thread's live mist, drifting with the wind, multiplies with it at run time.

## Gorges: falls that wore back upstream (research and build, 11 Nov)

A fall over a hard bed does not stay where the river first steepened: the plunge pool undercuts the soft rock
below the caprock, the lip collapses block by block, and the fall walks upstream leaving a slot gorge as wide as the
river (Niagara, about 1 m a year at 5700 m3/s, Gilbert 1907; the caprock mechanism, Haviv et al. 2010). Landscape
models write the knickpoint's speed as a celerity C A^p or C Q^p with p about 0.5 to 1 (Crosby and Whipple 2006;
Berlin and Anderson 2007); in massive, unjointed rock falls barely move (Lamb and Dietrich 2009). Hayakawa and
Matsukura 2003 fit Japanese falls' recession to discharge, fall size and rock strength the same way.

Built: `gorge_age` (years since the falls began wearing back, 0 off; the bench uses 12000, since the ice left) and
`gorge_rate` (0.013 m a year per sqrt(m3/s), Niagara's rate). Each lipped fall's lip moves up its river by
age x rate x sqrt(Q), a twentieth of that where the lip is a sill (a riegel of massive rock). It stops at a lake
(the fall would drain it), the fall above, and three cells from the map's edge (an inflow's reach is not on the
map). Below the new lip, `_ground` cuts a slot along the river's cells: the river's width plus 2 m, walls rising
over 2 m, its floor falling 1 percent to the old foot. The fall's record moves to the gorge's head, and its pool
sits at the slot's head. On the valley layout three side falls wore back 35 to 40 m into slots 8 m wide and 12 m
deep; the main fall stays on its bar (the lake's outlet).

## Rocks below cliffs (11 Nov; a leftover from the Planet Tech comparison)

Cliffs shed blocks, and the blocks come to rest on the talus below them, the biggest furthest out. Established
practice: rockfall run-out by the shadow angle (the line from the top of the talus to the furthest block dips about
27.5 degrees at the least, Evans and Hungr 1993) or the travel angle from the release point (the "Fahrboeschung",
Heim 1932; about 32 degrees for small rockfalls, Corominas 1996); fall sorting, larger blocks rolling further
(Kirkby and Statham 1975); block sizes by a power law (Dussauge-Peisser et al. 2002). Engines place such debris as
procedural scatter from slope and exposure rules (UE's PCG, Horizon Zero Dawn's GPU placement, Star Citizen's
"debris from cliffs").

Built: `get_boulders(rect, density)` returns (x, ground y, z, radius) for every block resting in the rect. Each grid
cell steeper than 45 degrees with at least 30 percent bare rock sheds about one block per 150 m2 of face times its
exposure; each block's radius is Pareto (exponent 2.5) from 0.25 to 3 m; it rolls down the steepest way over the
grid in 3 m steps, bouncing up to 35 degrees off the fall line each step, and rests once its travel angle from the release point drops below 27.5 to 33.5 degrees (2 degrees
less per metre of radius) and the ground has eased; blocks that reach water are dropped. Deterministic for a seed;
about 21 000 blocks and 0.3 s for the 4 km valley layout. Placement nodes (vegetation's scatter, Tripo rock props)
read the list; the bench shows grey stand-ins (`--boulders`).

## Automatic high lakes: riegels, overdeepenings and hanging valleys (research, 11 Nov; Paulo picked at 19:41)

Why: the showcase's high lake needs a hand-drawn bowl and bar. An open world (mortal-plane) needs the generator to
make them itself.

What makes them in nature:

- Overdeepenings and riegels: a glacier erodes at a rate set by its sliding speed and by the rock. Where the rock is
  weaker (more joints, softer beds) or where ice converges (confluences, a narrowing), it digs a basin; where the
  rock is stronger it leaves a bar, the riegel. Chains of them hold paternoster lakes down a trough (Cook and Swift
  2012, "Subglacial basins: their origin and importance", Earth-Science Reviews 115; Hooke 1991 on the feedback
  that deepens a basin once it starts: GSA Bulletin 103). Jointing, more than hardness alone, sets quarrying rates
  (Dühnforth, Anderson, Ward and Stock 2010, Geology 38; Krabbendam and Glasser 2011).
- Hanging valleys: a tributary glacier carries less ice than the trunk, slides slower and erodes less, so its
  floor is left above the trunk's and its stream falls off the lip. The hang height grows with the ice-discharge
  contrast (Amundson and Iverson 2006, JGR 111, F01020), which our erosion law (K_g u^l) already gives once the
  trunk carries much more ice than its tributaries.

Plan: the ice stage's erosion gets a rock-quality field (`glacial_rock_variation`, the spread, and
`glacial_rock_scale`, its wavelength, about 500 m), a smooth noise of joint density that multiplies K_g by
exp(variation x noise); where strata are on, hard bands also slow it. Soft patches become basins, hard ones bars,
and the existing basin-lake rule makes lakes of them with no markers. Test: the valley layout without `--bar` and
`--bowl` makes a lake at 300 m or higher with a fall below it on a river, and hanging side valleys show steps at
their mouths.

Built (11 Nov):

- The rock-quality multiplier acts on the ice grid's erosion rate (64 m nodes), not per bed cell. Per bed cell, a
  soft cell between ice nodes was invisible to the step limit and to the supercooling stop (both on the ice grid)
  and dug pits 500 to 2700 m deep. On the ice grid the stop holds and each basin is as wide as the ice samples.
- `--layout=valley --auto-lake` (no bar, bowl or level markers; ELA 400 m, erosion x15, variation 1.0): seven
  tarns and paternoster lakes form by themselves at 140 to 310 m in the side valleys, 1.5 to 2.5 ha, each with a
  fall of 20 to 40 m below its outlet. Erosion stays under 80 m; the ice stage takes 2.5 s.
- Variation 1.4 to 2.0 makes lakes 100 to 250 m deep in 2 to 3 ha basins: deeper than real cirque lakes (depth
  about a tenth of the width), so the bench keeps 1.0.
- Not yet: the big fall on the main river. Its glacier comes from beyond the map (the inflow carries water, not
  ice), so the trunk is never glaciated at the map edge and no riegel forms on it. That needs ice inflow at the
  edge, an ice flux boundary at the inflow cell; the showcase keeps its marked bar until then.

Ice inflow at the map edge (built 11 Nov, Paulo picked at 20:23):

- How regional ice models do it: a model that covers only part of an ice mass takes the ice crossing its boundary
  as a prescribed flux or thickness there and lets no ice leave through that strip (PISM's regional mode holds the
  boundary strip fixed; flowline models feed the upstream flux in as a source). The flux through a cross-section of
  a steady glacier is the net balance summed over the area above it.
- Built: `glacial_inflow_balance` (m/yr, 0 = off) times each inflow's off-map area is the ice it brings, added as
  accumulation on the ice grid in a Gaussian of 128 m sigma round the entry (a single node took 25 to 100 m of ice
  a pseudo-step and the solve did not settle). The edge beside the entry takes the surface of the node inside it,
  so no ice crosses there, and it wears as that node does, so the trough runs on past the map with no step at the
  boundary. Copying the thickness instead of the surface let ice pour in from the higher edge without limit (5 km
  thick, residual 1e14).
- Result (`--layout=valley --auto-lake`, 0.03 m/yr over 60 km², erosion x5): the main valley gets a trunk glacier
  up to 230 m thick, a trough and a chain of lakes down it (104 m, 56 m at 5.8 ha, 32 m) with steps between them;
  the side valleys keep their tarns (348, 159, 124 m). The solve settles (residual 15 m/yr). Erosion x15 with ice
  inflow wears the trough 550 m deep and the solve does not settle: x15 suits cirque glaciers, not a trunk.
- Why no lake at 450 m on the main river: without the hand-made bowl the main valley floor enters the map at about
  160 m, so its lakes sit below that. A high lake on the trunk needs high ground there to begin with (the bowl's
  uplift), not just ice.
- Cost: the ice stage takes 7 to 8 s with an inflow glacier against 2.5 s without (a larger ice area means shorter
  erosion sub-steps). Only when the setting is on.

## Moraines (research, 11 Nov night; proposed to Paulo, recommended)

Why: a glaciated valley without moraines reads as eroded by water; the sharp ridges along the old ice edges and
the loop across the valley at its furthest reach are the landform people know glacial valleys by, and the loop
dams lakes (Lake Garda, Lago Maggiore, most piedmont lakes).

What makes them:

- Debris: rockfall from the walls above the ice lands on the glacier (supraglacial debris), is carried down with the
  ice along flow lines (lateral and medial bands), and is dropped where the ice melts: the margins. A margin that
  holds still for centuries builds a ridge; the furthest stand leaves the terminal loop (Benn and Evans, Glaciers and
  Glaciation, ch. 11; Anderson, Anderson et al. 2012 on how debris cover lets glaciers reach far and builds large
  moraines; Rowan, Egholm and Clark 2015 for a debris-transport ice model, Earth and Planetary Science Letters 430).
- Shape: crests sharp, flanks near the angle of repose (30 to 38 degrees) when young, the inner flank steeper;
  lateral ridges tens of metres high in Alpine valleys, terminal loops 10 to 50 m.

Design (one pass after the last glaciation, no new solve):

1. Supply: ice-free cells steeper than 40 degrees next to the ice shed headwall retreat (about 1 mm/yr) times their
   area onto the nearest ice cell.
2. Transport: route the debris down the ice surface (steepest descent on bed + ice, as a flow accumulation weighted
   by supply) to the first cell where the ice ends.
3. Deposit: each margin cell gets its debris volume over a stand time (`moraine_age`, about 2000 years, the last
   stillstand) as a ridge: a cone at 34 degrees round it, maximum with the ground. The basin-lake rule finds the
   lakes behind the loops by itself.
4. Material: `get_soil_at`-style channel for till (grey, bouldery) so the vegetation and the ground shader can tell
   moraine from alluvium; boulders scattered on crests from get_boulders.

Cost: one routing pass on the fine grid, well under the ice stage's time.

Built (11 Nov night, Paulo picked all four proposals at 21:25):

- `moraine_age` (years, 0 = off). Supply: bare walls over 40 degrees retreat 1 mm/yr onto the ice, plus half the
  basal wear of the stand (the rest leaves in the meltwater). Transport: multiple flow directions down the ice
  surface (Freeman 1991); single-direction routing gathered everything on a few flow lines and dropped it at 47
  points. Deposit: at the first cell where the ice ends, spread along the margin, laid as 34 degree cones merged by
  maximum, added to the sediment (till carries soil).
- The edge of the glacier is where the ice, smoothed on the fine grid, is 10 m thick: the 64 m ice grid's
  bilinear edge drew staircases, and the solve's thin skirt over the slopes put the margin on every hillside.
- A hollow in the ice surface keeps its load (englacial, out with the meltwater); dropping it there left scattered
  bumps under the ice.
- Valley with no markers, 3000 years: terminal loops round each lobe at the mountain foot and lateral ridges round
  the ice-free peaks, up to about 30 m; the lakes at the foot grow behind them (20 to 23 ha, 8.6 to 10.6 ha).
  About 4e5 m3 of debris at 3000 years (the bench default with `--auto-lake`). Cost under 0.1 s.

## Scree (built 11 Nov night, Paulo picked it with the others)

What makes it: rock walls retreat by rockfall, about 0.1 to 1 mm/yr since the ice left (Sass 2005; Hales and Roering
2005, sensitive to frost cracking). The blocks fall down the wall, gather in its gullies and stop where the ground
flattens below it: cones under gullies, aprons under plain faces, straight at the angle of repose with a concave toe
where the large blocks run further (Statham 1976; Luckman 2013, Treatise on Geomorphology 7.15). Streams undercut
their toes and carry them off. Graphics papers place blocks on such slopes (Paris et al. 2020) but shape the ground by
thermal erosion only; World Creator and Gaea have debris layers of the same kind.

Built: `scree_age` (years, 0 = off). Supply: walls over 50 degrees (a 45 degree threshold took the sharp crests
between two slopes as walls and studded the slopes with cones) retreat 0.5 mm/yr. Transport: multiple flow
directions by slope^4 (single direction gathered a whole wall onto a few cells: one 90 m cone buried the pool lake
under the 338 m fall). Deposit: where the ground falls below 38 degrees, a half cone against the wall at
`talus_angle` with its last third eased out, merged by maximum, then two 3x3 passes on the scree only. No scree in
standing water (priority-flood basins, Barnes et al. 2014), on streams of 5 ha or more, or within 48 m of rivers of
1 km2 (they work their floor). No soil added: it stays bare.

Result, showcase valley at 10000 years: 2.2e6 m3, aprons fill the hollows at the feet of the walls; every lake and
fall stays. Off by default and in the bench (`--scree-age=10000` to see it): the difference is modest at 16 m cells,
and it reads as rock until the ground shader gets a scree material (next step: mark it in the talus channel).

Materials (12 Nov, Paulo picked "scree and till materials" at 23:04): the geology image's talus channel (blue) now
carries what loose cover lies on the ground. Scree from `scree_age` is 0..1 (smoothstep of its depth over 1 m), so
the cones and aprons read as angular blocks with no soil; moraine till from `moraine_age` is written negative
(0..-1, its depth over 2 m) where it is deeper than the scree. TerrainMaterial3D draws till as soil with stones
through it (the scree layer at 0.45 of the till weight, the height blend lifts the stones out of the soil), the way
fresh moraine looks: a sandy, silty matrix full of rounded boulders (Benn and Evans ch. 11), unlike scree's bare
sorted blocks. Old maps keep working: nothing negative was ever written there.

## Faster ice stage (11 Nov night)

Measured (showcase valley): the steady-ice solve is 96% of the ice stage, 1.26 of 1.30 s, in 128 solves (6000
pseudo-steps for the first, 200 warm-started for each after) on a 65 x 65 ice grid; a pseudo-step costs about 9 ns a
node, so the loop itself is already lean. The solve never settles: the largest steady-state imbalance stays at 15 to
37 m/yr after each warm start, so the terrain depends on the iteration path, and any change to the iterations
changes the result.

Tried:

- Accelerated pseudo-transient (a damped second-order update, Räss et al. 2022, GMD 15): diverges on the shallow-ice
  equation's H^5 diffusivity at damping 0.9 and 0.95, even with the stable step.
- Larger pseudo steps (0.1 instead of 0.03 d^2/D): slower and worse (oscillation, imbalance doubled).
- Coarse-to-fine in time: the ice ages between river steps on a 128 m ice grid, the last glaciation on 64 m
  (`glacial_coarse_cell`, 0 = off). The ice stage takes a third of the time (1.29 to 0.42 s; 8.1 to 2.2 s with an
  inflow glacier). The terrain is similar but not the same: the showcase lake shrinks from 3.0 to 2.3 ha and the
  fall moves; on the automatic valley the side-valley tarns do not form (the coarse ice wears the cirques
  differently). Kept as an option for previews and very large worlds, off by default.

The real fix is a converged steady-ice solver: the steady shallow-ice sheet as an obstacle problem solved by Newton
with a multigrid inner solve (Jouvet and Bueler 2012, SIAM J. Appl. Math. 72; Bueler 2016, J. Glaciology 62), which
settles in tens of iterations instead of thousands and makes the result independent of the iteration path. It
changes the terrain once (to the converged answer), so it needs a before/after look.

## Ice-worn rock (built 11 Nov night)

What it is: rock the ice slid over is abraded smooth on the faces that meet the flow (stoss) and quarried into
steps on the faces it leaves (lee): roches moutonnees and whalebacks, streamlined along the flow, scratched by
striations parallel to it (Glasser and Bennett 2004; Hallet 1979 and 1996 on abrasion and quarrying; Krabbendam and
Glasser 2011 on whalebacks vs roches moutonnees). At our scale it lives in the detail normal, not in the geometry.

Built: `ice_worn_detail` (off by default). The generator publishes `terrain_ice_flow` (RGBA8 on the geology grid:
rg the last glaciers' flow direction down their surface, b the wear, full at 20 m of glacial erosion) and
`terrain_ice_flow_rect` (zero: off). terrain_detail_normal() then scales the detail's slope along the flow: faces
rising into the flow by 1 - 0.7 wear, faces falling away by 1 + 0.8 wear, and adds striations 0.35 m apart across
the flow (faded by the pixel footprint). Showcase cirque from the floor: the slopes the ice ran down lose their soft
gullies on one side and keep sharp steps on the other; the effect is subtle at a distance, as it is in nature.

## Meandering rivers (built 12 Nov, Paulo picked it with the others at 23:04)

Rivers on flat alluvial floors wander: the outer bank of a bend erodes, the inner one builds a point bar, the bends
grow and travel downstream until a loop closes on itself and is cut off, leaving an oxbow. Engines and terrain tools
mostly draw rivers as splines (World Machine, Gaea, UE's water plugin); the established way to *grow* a meander belt
is the Howard and Knutson (1984) migration model, as in Ikeda, Parker and Sawai (1981) and meanderpy (Sylvester et al.
2019). Built that way:

- **Which reaches.** Down the solver's own drainage, stretches under Leopold and Wolman's (1957) braiding threshold
  S = 0.012 Q^-0.44, on loose fill, not ponded, above the tidal reach (1.5 m over sea level), at least 12 cells; slope
  and fill are 9-cell means along the river and gaps of up to 4 cells are bridged. Rivers narrower than half a cell
  (the grid cannot hold their bends) are left alone. Steeper sediment-rich reaches are braided instead (next item).
- **Migration.** R1 = -R0 + 2.5 x the upstream-weighted mean curvature (decay 2 Cf / depth, Cf 0.02, depth w/33 as in
  meanderpy). Banks move kl R1 toward the outer bank, kl 0.04 w per year: about 2% of the width a year at R1 0.5, the
  median measured rate (Hooke 2003; Constantine et al. 2014). meanderpy's 0.3 w per year is for big rivers in fast
  forward; at that rate a 14 m river cut off a loop every few years. The valley wall stops a bank (3 m above the
  reach's floodplain). Migration fades out over ten widths at each end, where the reach meets the fixed course.
- **Seeding bends.** The model only grows bends that exist, and they travel downstream, so a reach whose upstream end
  feeds none goes straight behind them. The start gets a wander a width high at the meander wavelength (11 w,
  Leopold and Wolman 1960). Banks of uneven strength (Motta et al. 2012) scale the rate and push the bank a little to
  one side, in a pattern that changes every few centuries. A new cutoff is left slightly rough.
- **Cutoffs.** Neck cutoffs (two points four widths apart along the river closer than 0.9 w) and chute cutoffs within
  three widths when the course is five times the way across (Howard 1996). Curvature is smoothed over a few points so
  the resampling's kinks do not grow into loops.
- **Oxbows** silt up after the cut: half the channel depth at first, an e-fold every 400 years, left out under 0.15 m.
- **Carving.** The old course is filled to the floodplain. The new channel is cut at full resolution: a parabolic
  bed of depth w/12, banks easing over another half width, eased in over three widths at the reach's head. At its
  foot the bed may not rise (the river's surface never climbs, so the water behind such a step vanished downstream):
  once the ground is final, the floodplain runs evenly between the ground at the reach's two ends and the course
  below is cut down to the channel's bed, falling a millimetre per metre, until it meets the ground.
  The 16 m grid could not hold a 14 m river: cut there alone, the cleanup filled it and the water stayed on the
  old cells. The grid keeps a coarse copy so the drainage follows. The water's points follow the channel's own
  curve at 0.6 of the bankfull depth, and the cut follows what the coast and the lakes later do to the ground.

Showcase valley at 3000 years: the clearing reach reaches sinuosity 2.0 (pressed against its walls) and the plain above
the beach 1.7, with 28 cut-off loops of which the youngest still show as hollows. Generation takes about 1.5 s
longer. Off by default; with it off the result is unchanged.

## Braided outwash (research, 12 Nov; Paulo picked it with the others at 23:04)

Why: below a glacier, or where one stood, the meltwater carries more gravel than one channel can move. It spreads
over a wide flat gravel floor (a sandur or valley train) and splits into many shallow channels round bars that shift
every flood. Alpine and Icelandic valleys and New Zealand's braided rivers read this way; a single meandering or
straight channel there looks wrong.

What decides it: the Leopold and Wolman (1957) line, slope above about 0.012 Q^-0.44 at bankfull discharge Q, with
high bedload and weak (unvegetated, gravel) banks; the same threshold this generator already uses to keep meanders
below it. Width grows with discharge (Parker 1976; braidplain width tens of times the channel width), the channel
count with stream power (Ashmore 1991; Egozi and Ashmore 2008 count 2 to 6 active channels at typical powers).

Established models:
- Murray and Paola (1994, Nature 371): the cellular braided river. Water is routed cell to cell down the slope,
  shared among the three downstream neighbours by slope^n, and moves sediment as a nonlinear function of the
  discharge; bars and channels braid on their own. Cheap, grid based, the standard reduced-complexity model.
- Thomas and Nicholas (2002), Nicholas (2013, Earth Surface Processes and Landforms): the same with a shallow-water
  flow solve, for the shapes of bars; heavier than a terrain generator needs.
- Engines: World Creator and Gaea draw river networks but no braids; Star Citizen's planets have none seen.

Design: `outwash_age` (years, 0 = off). Find the reaches above the Leopold-Wolman line with plenty of sediment that
lie below the last glaciation's margins (meltwater supply) or carry its moraine and scree; grade a braidplain across
them, flat across and as wide as Parker's width; then run a Murray-Paola pass on a fine grid (4 m) over that plain
for the years given, carving the active channels and leaving the bars. The ground there is bare gravel (alluvium,
no soil), the water follows the deepest channel with the side channels shallow, and the plants stay off the active
plain (bars older than a few decades take willows and grass in the real thing, a later step).

Built (off by default; on main without the water): reaches below the last ice stand (or on fans) whose 5-cell slope
is at or above 0.012 qb^-0.44 (qb five times the mean flow when the river is ice-fed) and under 0.05 are graded to a
flat plain clamp(6w, 3 cells, 400 m) wide and run through Murray-Paola for clamp(age/5, 60, 900) steps: water shared
to the three cells below by slope^0.5, sediment flux (Q S)^2.5, lateral bank collapse 0.3, adaptive time step. The
relief is detrended per row, scaled so the deepest 3% sit a thread depth (0.06 w, 0.5 to 2 m) down, and faded at the
edges; the plain is bare gravel. `get_braidplains()` gives the water its contract: centre line, half widths, stage per
point (30th percentile of the relief per row, made non-increasing downstream by isotonic regression), discharge at
the head, head and foot positions and the head cell (for the feeding river). Dry, a plain reads as flat pale gravel
with half a metre of relief: the braids show once the water sheet (Valence, Peytavie et al. 2019) lies over it.

## Tall falls keep their face (12 Nov, bug from the showcase)

The lip model builds a step: the ground over the fall's run is held at the lip's height to mid-run and at the
foot's after it. Right for a ledge of a few metres, wrong for the 338 m fall off the bar: it cut a straight-sided box
into the face with a flat pad at the bottom. Real falls off hanging-valley lips and riegels (Yosemite, Staubbach,
Lauterbrunnen) leave the wall as it stands and notch only the brink, the cleft narrowing and shallowing down the
face, with rockfall and a scour basin at the foot (Crosby and Whipple 2006 on hanging valleys; Lamb et al. 2007 on
falls cutting back by undercutting and block toppling). So falls over 25 m (NOTCH_DROP) keep the ground and get a
notch: up to 0.4 of the river's width deep at the brink (2 to 10 m), fading in over 8 m upstream and gone a third of
the way down, a rounded V narrowing downwards with walls wandering by a quarter of its width; the plunge pool sits
at the foot of the run (as for falls without a lip). get_waterfalls() keeps its keys; the tall falls' face profiles
and type now follow the real face.

## Cliff relief in the geometry (12 Nov; Paulo's "then displaced cliffs")

Why: the shader detail cannot change a silhouette or cast a ledge's shadow, and a heightfield has no vertices on a
wall: a 30 m face at 1 m spacing is one strip of stretched triangles. Established ways: displacement along the normal
on tessellated terrain (REDengine 3, Gollent 2014; Frostbite, Widmark 2012; UE4 landscape tessellation, UE5 Nanite
displacement), or placed cliff meshes (Far Cry 5, Horizon). The second needs assets; the first needs vertices where
the wall is.

Built: Landscape3D `cliff_relief` (m, 0 = off). On the finest level, where the relief spans at least half the
pixel error (near the camera), triangles whose edges climb more than the grid spacing are refined by red-green
refinement driven by each edge's own end points (both sides of an edge agree, so no cracks, also across chunks), and
every vertex on faces past about 50 degrees is pushed along the face's horizontal normal: warped beds 3 m thick
standing proud or set back (differential erosion; ledges), their steps rounded over a metre, staggered joint blocks
4.5 m apart, roughness. Normals come from the displaced surface around each point (positions only, so seamless).
Sides next to a chunk without relief keep their vertices and edges, as sides next to a coarser chunk do. Raster and
path traced frames at the bar face match (the BLAS holds the ledges). Cost on the bench at 0.6 m: a view close to the
338 m wall goes from 2.6 to 5.5 M triangles; a wide view of it is unchanged (relief beyond the radius is subpixel and
left to the shader). Next: the far strata in the shader, matched to these beds.

## Rock detail on steep faces and scree (12 Nov; Paulo picked shader detail first, then displaced cliffs)

Why: the generator's erosion detail is a heightfield (offsets in y over x z), so faces near vertical get none: gorge
walls and cliffs read as smooth stretched slabs, and scree as flat paint, wherever the rock textures are missing or
too coarse (the showcase found it on the gorge walls; the scree and till docs pairs did not read for the same reason).

Established ways: triplanar or biplanar texture projection (Quilez 2020 for biplanar; UE and HDRP cliff materials)
needs textures; procedural solid noise in world space needs none and cannot stretch (Perlin 1985, Worley 1996), and
Mikkelsen's (2010) surface gradient turns any 3D height into a normal with no tangent frame. Real walls break along
bedding planes and two joint sets near right angles to them (Priest 1993; spacings of decimetres to metres), with
the blocks of one bed offset from the next; scree is loose blocks of 0.1 to 1 m.

Built: TerrainMaterial3D `rock_detail_enabled` (off by default), `rock_detail_scale`. Faces of rock steeper than
about 35 degrees: warped beds 1.2 m thick, joints 2.5 and 3.5 m apart at a strike that turns slowly over the map,
staggered bed to bed, each block a little proud, recessed or tilted, cracks shaded, two octaves of roughness. Scree
(and the stones in till): rounded stones about 0.35 m. The cracks never get thinner than 2.5 pixels and the detail
fades out before a pixel covers a tenth of a bed or a quarter of a stone, so nothing aliases into lines. Next:
displaced geometry on faces over 60 degrees for close views.

Fix (12 Nov, from the showcase): from about 150 m and at grazing angles the first build covered slopes in dark
hairlines that beat into moire, and darkened the frame, because the fade went by distance only (a face seen edge-on
spans metres in one pixel at any distance) and the cracks were widened to stay 2.5 pixels wide, so their share of
the face grew with distance. The established cure is band limiting the pattern itself: the cracks are a box-filtered
pulse train (Apodaca and Gritz 2000, Advanced RenderMan ch. 11), integrated over each pixel's footprint along each
set's normal from the pixel's footprint (a ray cone's ellipse, Akenine-Moller et al. 2019, so the path tracer, which has
no screen derivatives, filters as the raster does), so a pixel shows the cracks' area, not a line; every
other feature (block steps and tones, crack relief, the roughness octaves, stones) is frequency clamped (Norton,
Rockwood and Skolmoski 1982) to its mean as a pixel spans a quarter to half its size; the normal's difference step
grows with the pixel. Faces are lifted by the cracks' mean darkening and stones divided by their measured mean
(0.958), so the shade averages 1 over any area: the mean brightness on and off matched to 0.01 of 255 at 150 m and
at grazing angles. Cracks shade to 0.82 (was 0.72) and the faces get the detail past 45 degrees (full past 57), so
gentle rock keeps the generator's own detail.

## Sources

- Yuan, Braun, Guerit, Rouby, Cordonnier 2019, "A new efficient method to solve the stream power law model taking into account sediment deposition", JGR Earth Surface 124, 1346-1365.
- Davy, Lague 2009, "Fluvial erosion/transport equation of landscape evolution models revisited", JGR Earth Surface 114.

- McCabe, Markstrom, A monthly water-balance model driven by a graphical user interface, USGS Open-File Report
  2007-1088, 2007.
- Thornthwaite, Mather, Instructions and tables for computing potential evapotranspiration and the water balance,
  Publications in Climatology 10(3), 1957.
- Hock, Temperature index melt modelling in mountain areas, Journal of Hydrology 282, 2003.
- Hock, A distributed temperature-index ice- and snowmelt model including potential direct solar radiation, Journal
  of Glaciology 45, 1999.
- Calov, Greve, A semi-analytical solution for the positive degree-day model with stochastic temperature
  variations, Journal of Glaciology 51, 2005.
- Nash, The form of the instantaneous unit hydrograph, IAHS Publication 45, 1957.
- Gilbert, Rate of recession of Niagara Falls, USGS Bulletin 306, 1907.
- Haviv, Enzel, Whipple et al., Evolution of vertical knickpoints (waterfalls) with resistant caprock: insights from
  numerical modeling, JGR 115, F03028, 2010.
- Forte, Yanites, Whipple, Complexities of landscape evolution during incision through layered stratigraphy with
  contrasts in rock strength, ESPL 41, 2016.
- Mason, Arumugam, Free jet scour below dams and flip buckets, J. Hydraulic Engineering 111(2), 1985.
- Pagliara, Hager, Minor, Hydraulics of plane plunge pool scour, J. Hydraulic Engineering 132, 2006.
- Bollaert, Schleiss, Scour of rock due to the impact of plunging high velocity jets, J. Hydraulic Research 41, 2003.
- Scheingross, Lamb, Sediment transport through self-adjusting, bedrock-walled waterfall plunge pools, JGR Earth
  Surface 121, 2016.
- Horeni, Disintegration of a free jet of water in air, Sesit 93, Prague, 1956.
- Gunn, Kinzer, The terminal velocity of fall for water droplets in stagnant air, J. Meteorology 6, 1949.
- Herman, Beyssac, Brughelli et al., Erosion by an Alpine glacier, Science 350, 2015.
- Alley, Lawson, Larson, Evenson, Baker, Stabilizing feedbacks in glacier-bed erosion, Nature 424, 2003.
- Schmidt, Montgomery, Limits to relief, Science 270, 1995.
- Cordonnier et al., Forming terrains by glacial erosion, ACM TOG 42(4), 2023.
- Crosby, Whipple, Knickpoint initiation and distribution within fluvial networks: 236 waterfalls in the Waipaoa
  River, North Island, New Zealand, Geomorphology 82, 2006.
- Berlin, Anderson, Modeling of knickpoint retreat on the Roan Plateau, western Colorado, JGR 112, F03S06, 2007.
- Hayakawa, Matsukura, Recession rates of waterfalls in Boso Peninsula, Japan, and a predictive equation, ESPL 28,
  2003.
- Evans, Hungr, The assessment of rockfall hazard at the base of talus slopes, Canadian Geotechnical Journal 30,
  1993.
- Kirkby, Statham, Surface stone movement and scree formation, Journal of Geology 83, 1975.
- Corominas, The angle of reach as a mobility index for small and large landslides, Canadian Geotechnical Journal
  33, 1996.
- Dussauge-Peisser, Helmstetter, Grasso et al., Probabilistic approach to rock fall hazard assessment: potential of
  historical data analysis, Natural Hazards and Earth System Sciences 2, 2002.
- Bryant, Falk, Paola, Experimental study of avulsion frequency and rate of deposition, Geology 23, 1995.
- Jerolmack, Paola, Complexity in a cellular model of river avulsion, Geomorphology 91, 2007.
- Lamb, Dietrich, The persistence of waterfalls in fractured rock, GSA Bulletin 121, 2009.
- Scheingross, Lamb, A mechanistic model of waterfall plunge pool erosion into bedrock, JGR Earth Surface 122, 2017.
- Perne, Covington, Thaler, Myre, Steady state, erosional continuity, and the topography of landscapes developed in
  layered rocks, Earth Surface Dynamics 5, 2017.
- Whipple, Tucker, Dynamics of the stream-power river incision model, JGR 104, 1999.
- Sklar, Dietrich, Sediment and rock strength controls on river incision into bedrock, Geology 29, 2001.
- Roering, Kirchner, Dietrich, Evidence for nonlinear, diffusive sediment transport on hillslopes, Water Resources
  Research 35, 1999.
- Heimsath, DiBiase, Whipple, Soil production limits and the transition to bedrock-dominated landscapes, Nature
  Geoscience 5, 2012.
- Larsen, Almond, Eger et al., Rapid soil production and weathering in the Southern Alps, Science 343, 2014.
- Montgomery, Dietrich, Where do channels begin?, Nature 336, 1988.
- Montgomery, Brandon, Topographic controls on erosion rates in tectonically active mountain ranges, EPSL 201, 2002.
- Tzathas, Gailleton, Steer, Cordonnier, Physically-based analytical erosion for fast terrain generation, Computer
  Graphics Forum 43(2), 2024.
- Smith, Barstad, A linear theory of orographic precipitation, Journal of the Atmospheric Sciences 61, 2004.
- Rolland, Spatial and seasonal variations of air temperature lapse rates in Alpine regions, Journal of Climate 16, 2003.
- Jennings, Winchell, Livneh, Molotch, Spatial variation of the rain-snow temperature threshold across the Northern
  Hemisphere, Nature Communications 9, 2018.
- Thornthwaite, An approach toward a rational classification of climate, Geographical Review 38, 1948.
- Willmott, Rowe, Mintz, Climatology of the terrestrial seasonal water cycle, Journal of Climatology 5, 1985.
- UNEP, World Atlas of Desertification, 2nd ed., 1997 (aridity index).
- Dozier, Frew, Rapid calculation of terrain parameters for radiation modeling from digital elevation data, IEEE
  TGRS 28, 1990.
- Cooper, The absorption of radiation in solar stills, Solar Energy 12, 1969.
- Kasten, Young, Revised optical air mass tables and approximation formula, Applied Optics 28, 1989.
- Liu, Jordan, The interrelationship and characteristic distribution of direct, diffuse and total solar radiation,
  Solar Energy 4, 1960.
- Kasten, Czeplak, Solar and terrestrial radiation dependent on the amount and type of cloud, Solar Energy 24, 1980.
- Winstral, Elder, Davis, Spatial snow modeling of wind-redistributed snow using terrain-based parameters, Journal of
  Hydrometeorology 3, 2002.
- fastscape-lem/orographic-precipitation (MIT), read for the transfer function's sign conventions; not copied.
- Star Citizen wiki, Planet Tech v5: https://starcitizen.tools/Planet_Tech_v5
- Borgefors, Distance transformations in digital images, Computer Vision, Graphics and Image Processing 1986.
- Costa, Schuster, The formation and failure of natural dams, GSA Bulletin 1988.
- Korup, Recent research on landslide dams: a literature review with special attention to New Zealand, Progress in
  Physical Geography 2002.
- Galin, Guérin, Peytavie et al., A Review of Digital Terrain Modeling, CGF 38(2), 2019.
- Musgrave, Kolb, Mace, The synthesis and rendering of eroded fractal terrains, SIGGRAPH 1989.
- Ebert, Musgrave, Peachey, Perlin, Worley, Texturing and Modeling: A Procedural Approach.
- Quilez, noise derivatives and domain warping: https://iquilezles.org/articles/
- Bigun and Granlund, Optimal orientation detection of linear symmetry, ICCV 1987.
- Bruun, Coast erosion and the development of beach profiles, Beach Erosion Board TM 44, 1954.
- Dean, Equilibrium beach profiles: characteristics and applications, Journal of Coastal Research 7(1), 1991.
- Barnes, Callaghan, Wickert, Computing water flow through complex landscapes, part 3: Fill-Spill-Merge, Earth
  Surface Dynamics 9, 2021.
- Yuan, Braun, Guerit et al., A new efficient method to solve the stream power law model taking into account
  sediment deposition, JGR Earth Surface 124, 2019.
- Beyer, Implementation of a method for hydraulic erosion, bachelor's thesis, TU Munich, 2015.
- Mei, Decaudin, Hu, Fast hydraulic erosion simulation and visualization on GPU, Pacific Graphics 2007.
- O'Brien and Hodgins, Dynamic simulation of splashing fluids, Computer Animation 1995 (virtual pipes).
- Stava, Benes, Brisbin, Krivanek, Interactive terrain modeling using hydraulic erosion, SCA 2008.
- Olsen, Realtime procedural terrain generation, 2004.
- Heimsath, Dietrich, Nishiizumi, Finkel, The soil production function and landscape equilibrium, Nature 388, 1997.
- Wobus, Whipple, Kirby et al., Tectonics from topography: procedures, promise, and pitfalls, GSA Special Paper 398, 2006.
- Montgomery, Abbe, Buffington et al., Distribution of bedrock and alluvial channels in forested mountain drainage
  basins, Nature 381, 1996.
- Leopold and Maddock, The hydraulic geometry of stream channels and some physiographic implications, USGS PP 252, 1953.
- Hack, Studies of longitudinal stream profiles in Virginia and Maryland, USGS PP 294-B, 1957.
- Kass and Witkin, Analyzing oriented patterns, CVGIP 37, 1987.
- Johansen, Fast and Gorgeous Erosion Filter, 2026:
  https://blog.runevision.com/2026/03/fast-and-gorgeous-erosion-filter.html
- Mei, Decaudin, Hu, Fast Hydraulic Erosion Simulation and Visualization on GPU, PG 2007.
- Št'ava, Beneš, Brisbin, Křivánek, Interactive Terrain Modeling Using Hydraulic Erosion, SCA 2008.
- Olsen, Realtime Procedural Terrain Generation, 2004.
- Jákó, Tóth, Fast Hydraulic and Thermal Erosion on the GPU, CESCG 2011.
- Beyer, Implementation of a method for hydraulic erosion, TU München 2015.
- Cordonnier et al., Large Scale Terrain Generation from Tectonic Uplift and Fluvial Erosion,
  CGF 35(2), 2016: https://diglib.eg.org/handle/10.1111/cgf12820
- Braun, Willett, A very efficient O(n), implicit and parallel method to solve the stream power
  equation, Geomorphology 180-181, 2013.
- Schott et al., Large-scale Terrain Authoring through Interactive Erosion Simulation, ACM TOG
  2023: https://h-schott.github.io/p/uplift ; code https://github.com/H-Schott/StreamPowerErosion (MIT)
- Schott et al., Terrain Amplification using Multi-Scale Erosion, ACM TOG 2024:
  https://h-schott.github.io/p/mserosion
- Jain et al., FastFlow: GPU Acceleration of Flow and Depression Routing for Landscape
  Simulation, CGF 2024: https://diglib.eg.org/handle/10.1111/cgf15243
- Jain et al., Efficient Debris-flow Simulation for Steep Terrain Erosion, SIGGRAPH 2024.
- Tzathas et al., Physically-based Analytical Erosion for fast Terrain Generation, CGF 43(2), 2024:
  https://diglib.eg.org/handle/10.1111/cgf15033
- Cordonnier et al., Forming Terrains by Glacial Erosion, SIGGRAPH 2023.
- SideFX, Houdini heightfield erosion: https://www.sidefx.com/docs/houdini/heightfields/erosion.html
- QuadSpinner, Gaea tiled builds: https://docs.quadspinner.com/Guide/Build/Tiled.html
- World Machine: https://world-machine.com/
- Ghost Recon Wildlands pipeline interview:
  https://80.lv/articles/procedural-technology-in-ghost-recon-wildlands/
- Carrier, Procedural World Generation of Far Cry 5, GDC 2018:
  https://gdcvault.com/play/1025557/Procedural-World-Generation-of-Far
- Star Citizen Planet Tech v5 (Genesis, Starchitect): https://starcitizen.tools/Planet_Tech_v5
- Frontier, Elite Dangerous Odyssey planet tech:
  https://www.pcgamer.com/heres-how-frontier-rebuilt-a-galaxys-worth-of-planets-for-elite-dangerous-odyssey
- Terrain3D https://github.com/TokisanGames/Terrain3D ; HTerrain
  https://github.com/Zylann/godot_heightmap_plugin
- Howard, A. D. and Knutson, T. R. (1984). Sufficient conditions for river meandering: a simulation approach. Water Resources Research 20(11).
- Howard, A. D. (1996). Modelling channel evolution and floodplain morphology. In Floodplain Processes, Wiley.
- Ikeda, S., Parker, G. and Sawai, K. (1981). Bend theory of river meanders. Journal of Fluid Mechanics 112.
- Sylvester, Z., Durkin, P. and Covault, J. A. (2019). High curvatures drive river meandering. Geology 47(3). meanderpy.
- Leopold, L. B. and Wolman, M. G. (1957). River channel patterns: braided, meandering and straight. USGS Professional Paper 282-B.
- Hooke, J. M. (2003). River meander behaviour and instability. Transactions of the Institute of British Geographers 28.
- Constantine, J. A. et al. (2014). Sediment supply as a driver of river meandering and floodplain evolution in the Amazon Basin. Nature Geoscience 7.
- Motta, D., Abad, J. D., Langendoen, E. J. and Garcia, M. H. (2012). The effects of floodplain soil heterogeneity on meander planform shape. Water Resources Research 48.
- Leopold, L. B. and Wolman, M. G. (1960). River meanders. GSA Bulletin 71.
- Constantine, J. A., Dunne, T., Piégay, H. and Kondolf, G. M. (2010). Controls on the alluviation of oxbow lakes by bed-material load. Sedimentology 57.
