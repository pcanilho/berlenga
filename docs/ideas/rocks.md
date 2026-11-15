# Rocks and stones

Research note, 15 Nov 2026, written before any code. Rocks as real geometry at every scale, from
sand-grain shingle to house-sized blocks, shaped by how they broke and how far they travelled, and
placed by the systems that already know where they should be (terrain, rivers, shore, vegetation).

## What exists today

- `TerrainGenerator3D.get_boulders(rect, density)`: rockfall blocks. Each bare cell steeper than
  45 degrees sheds blocks by exposed face area; each rolls down to the shadow angle (Evans and Hungr
  1993) with fall sorting (Kirkby and Statham 1975); radius Pareto 0.25 to 3 m. Returns
  `(x, y, z, radius)` only. **No mesh**: the landscape bench draws flattened 7-segment spheres.
- Scree and till (`scree_age`, moraines): cones and aprons in the heightfield plus
  `scree_cover` / `till_cover` maps for the material. The stones themselves are
  `TerrainMaterial3D.rock_detail_enabled`: a Worley-cell normal (0.2 to 0.6 m "stones") with no
  geometry. Under path tracing they have no silhouette, no contact shadow, no parallax.
- Cliffs: `cliff_relief`, `get_cliff_point`, strata (`strata_enabled`, `hardness` map), rock
  detail joints and beds as a shader normal.
- Rivers: `PhysXRiver3D` already flows round rocks (`obstacle_collision_mask`) but nothing
  makes river rocks.
- Vegetation: `VegetationObstacles` discs and `rock_foot` (plants gather and lean at boulder
  feet). Trees: lichen, moss and fungi code (`_lichen_at`, `lichen_cover`, `fungi_cover`).
- Water: contact wetness, shore run-up; weather: shared snow and wetness shader code.
- Open world: RT LOD plan and the open-world bench (docs/ideas/open-world-geometry.md,
  persistent-instances.md); "400 rocks: 6 BLAS builds a frame instead of 400" is the case for a
  small shared shape library.

terrain-generation.md already lists the gap: "nothing places rocks from [the talus map] yet".

## Bar

- Kingdom Come: Deliverance II, Crysis (CryEngine): scanned rock sets, a few dozen meshes per
  biome, sunk into the ground and blended into the terrain material at the contact; moss on top
  faces. The rocks match the cliffs above them.
- Star Engine / Nyx I (Planet Tech v4/v5, Genesis): rocks scattered by biome rules in clusters
  (big, medium, debris), object-terrain blending, rock families matching the planet's geology.
- Real geometry over texture tricks: a stone that reads at 2 m must be a mesh.

## Science: how rocks get their shape

- **Fragmentation.** Domokos, Jerolmack, Kun, Török, "Plato's cube and the natural geometry of
  fragmentation", PNAS 2020: natural fragments average about 6 faces and 8 vertices (cuboid)
  whether from joints, frost or impact; 2D cracks give mean 4-gons. Joint sets (bedding plus two
  joint directions) cut blocks; Priest 1993 for spacing. Paris, Peytavie, Guérin, Dischler, Galin,
  "Modeling Rocky Scenery using Implicit Blocks", The Visual Computer 2020: fracture networks
  define tiling blocks as implicit primitives, blended on cliffs following strata (code MIT,
  aparis69/Rock-fracturing, reference only).
- **Abrasion by transport.** Bloore 1977: surface moves inward at speed 1 + c·kappa (curvature).
  Domokos and Gibbons 2012 (box equations), Domokos, Jerolmack, Sipos, Török, "How river rocks
  round: resolving the shape-size paradox", PLOS ONE 2014: rounding comes first with little mass
  loss (corners go, about 40% of mass), then slow shrinking; Novák-Szabó et al., Science Advances
  2018: bed-load chipping follows the same curve everywhere. Szabó, Domokos, Grotzinger,
  Jerolmack, Nature Communications 2015: roundness gives transport distance (the inverse is what
  we need: distance gives roundness). Havasi-Tóth and Fehér 2023 (arXiv 2310.06932): polygon ODE
  approximation of Bloore within 10%.
- **Downstream fining.** Sternberg's law D = D0·exp(-a·x): river bed grain size falls with
  distance; rivers sort (coarse in riffles and outer bends, fine in pools and bars).
- **Beaches.** Shingle is sorted along and across the shore by wave energy; beach pebbles flatten
  into discs (swash sliding), unlike river cobbles; cusps and storm ridges.
- **Weathering.** Spheroidal weathering of jointed granite and basalt: corners attack from three
  sides, blocks round in place into corestones (tors, Dartmoor). Dorsey et al., "Modeling and
  Rendering of Weathered Stone", SIGGRAPH 1999 (surface slab, mineral dissolution). Sedimentary
  blocks keep flat bedding faces; columnar basalt makes 5 to 7-sided columns.
- **Piles.** Peytavie, Galin, Grosjean, Mérillou, "Procedural Generation of Rock Piles using
  Aperiodic Tiling", CGF 2009: packed piles from tiled Voronoi cells, eroded. For us: settle with
  PhysX rigid bodies offline and freeze, as scree and walls are really stacked.

## Engines and tools

- Unreal: Electric Dreams uses PCG to scatter hand-made Nanite rock meshes and "hero rock
  assemblies" (Flat Area Detector, hierarchy points); no procedural shapes. UE 5.7 Nanite foliage
  and assemblies are about rendering many instances, which our RT LOD and MultiMesh path covers.
- Houdini: Voronoi fracture plus noise and smoothing (SideFX "rock generator", Labs), the
  standard pipeline; Far Cry 5 (GDC 2018, Carrier) generated cliffs and debris from terrain
  slope and flow maps in Houdini at world scale.
- Horizon Zero Dawn (GDC 2017, van Muijden): GPU procedural placement by density maps per
  object, rocks included. Frostbite (Battlefront GDC 2016): photogrammetry rocks.
- Unity HDRP, CryEngine, Godot: nothing built in. Godot addons (Gaea, terrain tools) are 2D or
  heightfield; nothing worth taking.
- itch.io: ROCKGEN 2024 (Unity, noise on low poly, LODs, clusters); rocks-bitch (ProcJam 2014,
  point cloud plus Poisson reconstruction); Blender geometry-node rock makers (noise plus cracks).
  All are noise on a blob: no fracture, no transport, no geology. The lesson is only that LODs and
  clusters matter; nothing to build on.

What I would build on: Plato's cube statistics and jointed fracture for the break, Bloore
abrasion for the transport, Paris 2020 for cliff blocks, Sternberg and shore sorting for where,
PhysX for piles. No addon is needed.

## Proposal

A `RockGenerator` (new module, beside `TreeGenerator`) that makes a **small shape library per
geology**, then placement layers that feed it from the systems that already compute where rocks
are. Not one unique mesh per rock.

1. **Shapes.** Per family: fracture a block with the joint sets of the rock type (bedding
   thickness, joint spacing, angles; Voronoi cells for massive rock), into a signed distance
   field; abrade with Bloore flow for a set transport distance (0 = fresh rockfall, metres for
   talus, kilometres for river cobbles, flattened for beach shingle); spheroidal rounding for
   granite; chips, pits and bedding grooves as SDF detail at cm scale; mesh by surface nets;
   meshoptimizer LODs (in tree); xatlas UVs where a texture is needed. Per-vertex data: cavity,
   up-facing, exposure for moss, lichen, wetness and snow. PhysX convex hull per shape (and a
   Blast asset for the ones that break). 16 to 32 variants per family, a few MB in all.
2. **Families.** Angular rockfall blocks and scree; glacial boulders and erratics
   (subrounded, striated); river cobbles and gravel (rounded by distance); beach pebbles and
   shingle (flat, sorted); cliff blocks and outcrops (Paris 2020, on the cliff faces the
   terrain marks); later sea stacks and tors.
3. **Placement layers.** `get_boulders` gets real meshes. Scree and till maps become instanced
   stones near the camera, with today's rock-detail normal kept for the far field (it already
   matches). River beds: size by Sternberg along each river and sorting by bend and depth, laid
   in the channel, read by `PhysXRiver3D` as obstacles. Shore: shingle and pebble bands by wave
   energy and shoreline distance. Piles: PhysX settle offline, frozen to transforms.
4. **Ties.** Vegetation obstacles and `rock_foot` read the same list; moss and lichen from the
   tree code by up-facing and wetness; contact wetness and run-up wet the shore stones; the shared
   snow code covers tops; the contact line blends into the terrain material and the rock is sunk
   by a third, so no floating stones.

## Scale (open world)

- Instances in MultiMesh per streaming cell; one BLAS per (shape, LOD), shared by every instance
  (open-world-geometry.md step 1, Valence's numbers). Pebbles under about 5 cm stay in the
  terrain normal; a minimum projected size drops tiny far stones from the TLAS.
- Placement is deterministic per cell from the seed, so nothing is stored; only the shape library
  is kept in memory.
- Path tracing: geometry carries the silhouette and contact; the near-field cm detail must be
  geometry too or a displacement that PT also hits (displaced-hit-varyings.md), never a normal
  only. Check raster against PT at 100% crop before merging.

## Cost to measure before merging

Library build time on load, memory per family, instance counts per km2 for scree and river beds,
TLAS and trace ms on the open-world bench at 10k and 100k stones.

## Built (15 Nov)

`modules/rocks`: `RockGenerator` (shapes) and `RockField3D` (placement), bench in `docs/bench/rocks`.

- **Shapes.** Five rock types (granite, limestone, sandstone, basalt, slate) by six families (rockfall,
  scree, glacial, cobble, pebble, cliff block). Joint planes with jitter per lithology (basalt as 5 to
  7-sided prisms), corner cuts, chip scoops and part cracks into an SDF; Bloore flow (1 + c·kappa) on a
  coarse grid for the transport distance, with flattening for beach pebbles; then grain, lumps, karst
  pits or vesicles, laminae, cleavage, tafoni and striations at grid scale; surface nets; cavity and
  fresh-break masks in COLOR; meshoptimizer LODs; 48-point convex hull. Rest pose: long axis x, short
  y, b axis 2 units on z. 12 to 245 ms a rock on one cloud core.
- **Top LOD collapse** (`simplify`, on): meshoptimizer with normals as attributes at 3% of a grid cell.
  Close-ups at 100% crop match the full mesh (mean pixel difference 0.3 to 0.5 of 255); shape memory
  52.6 MB to 26.4 MB. A 20%-of-a-cell collapse cut it to 6.6 MB but softened granite relief and
  sandstone grooves, so it was not kept.
- **Placement, one layer each.** Rockfall from `get_boulders`. Scree packed on talus by sequential
  deposition (largest first, fall sorted), no deep overlaps. Glacial on till or old ice ground, 8%
  erratics of `erratic_rock_type`. River beds sized by Shields competence on the channel slope with
  Leopold-Maddock depth, rounded by distance from the head (three roundness bins per lithology),
  imbricated against the flow, with a water film. Shores: band from 0.6 m below to 2.5 m above water,
  grain size from beach slope (Bascom), blended between 2 m sub-cell corners so the cover fades with the
  slope, boulders under rock. Cliffs: blocks in 1.4 m courses along the
  beds, staggered joints, set 60 to 80% of their depth into the face (Paris et al. 2020).
  `use_strata` swaps in `soft_rock_type` on soft beds. Cliff blocks only go where the heightfield holds the
  face (50 to 76 degrees, near planar across a terrain cell, centre inside the rock): steeper heightfield
  steps are stretched in the mesh and blocks set by the bilinear height floated in front of them.
- **Ties.** `get_obstacles(rect)` gives `(x, ground y, z, footprint)`, the format
  `VegetationField3D.obstacles` and `GrassField3D.obstacles` take; `get_stones` adds basis, scale,
  water film, family, layer and `top` for crevice plants and moss. Shader: lichen and moss covers,
  ground contact band, the shared weather wetness and snow code. Optional convex hull collision per cell
  near the target.
- **Numbers on the 2 km bench world.** Stones over 4 km2: rockfall 4.6k, glacial 178, river 14.9k,
  shore 8.5k, cliff 28.7k; scree 7.2k in the fullest 200 m square. A 320 m square round scree: 399 cells,
  123k stones, 54 shapes (26.4 MB), 7.6 MB of instances, 11.9k collision hulls, built in 1.8 s.

Review fixes (15 Nov evening): fresh rockfall and scree no longer get spheroidal rounding and are cut by
two to four through-fractures at any angle; glacial boulders get one or two ground facets; periodic marks
(laminae, cleavage, striae) have a period of at least five grid cells, as finer ones aliased into combs and
notches; cracks are at least 1.6 cells wide, wander, and taper shut at their tips.

Open: GPU frame and trace cost on the open-world bench (PC queue); river stones here never pass the first
roundness bin because the bench rivers are short; vegetation reading the stone list is the vegetation
thread's pick.

## Sources

Domokos et al. PNAS 2020 (arXiv 1912.04628); Paris et al. Visual Computer 2020 (hal-02926218);
Domokos et al. PLOS ONE 2014 (PMC3922984); Novák-Szabó et al. Sci. Adv. 2018; Szabó et al.
Nat. Commun. 2015; Havasi-Tóth and Fehér arXiv 2310.06932; Peytavie et al. CGF 2009; Dorsey et
al. SIGGRAPH 1999; Evans and Hungr 1993; Kirkby and Statham 1975; Priest 1993; Epic "PCG in
Electric Dreams"; SideFX rock generator tutorials; GDC 2018 Far Cry 5 world generation; GDC 2017
Horizon Zero Dawn GPU placement; itch.io ROCKGEN 2024, rocks-bitch, mooham3d rockgen.
