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

Next: stage 3 (local amplification: erosion filter and damped fBm, in the bake and optionally in the material),
then stage 5 maps, then stage 4. Speed-ups when worlds grow: threading the per-cell passes, and FastFlow-style GPU
routing (Jain et al. 2024).

## Sources

- Galin, Guérin, Peytavie et al., A Review of Digital Terrain Modeling, CGF 38(2), 2019.
- Musgrave, Kolb, Mace, The synthesis and rendering of eroded fractal terrains, SIGGRAPH 1989.
- Ebert, Musgrave, Peachey, Perlin, Worley, Texturing and Modeling: A Procedural Approach.
- Quilez, noise derivatives and domain warping: https://iquilezles.org/articles/
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
