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

## Round 2 (15 Nov, evening)

Close-up detail as geometry: `RockGenerator.close_up` builds the same stone (same flow on the base coarse grid,
same centimetre marks sized by the base cell) on a grid of about 3 mm cells at the family's typical size (capped
at 300 cells), with the rock's millimetre fabric carved into the distance field: granite crystals (Worley cells
of 2 cm, feldspar proud, quartz lower, etched boundaries, grus), sandstone laminae of 1.2 cm, limestone
rillenkarren running down the upper faces and micropits, basalt vesicles, slate cleavage steps, conchoidal
ripples on fresh breaks, glacial striae at least eight cells apart and discontinuous. Features under three cells
fade out instead of aliasing. `RockField3D.close_up_*`: stones within 8 m of the follow target swap to the
close-up shape (built on worker threads, two at a time, base instance collapsed meanwhile; at most 12 shapes
kept, the stale ones dropped after 30 s; no LODs; not for cobbles and pebbles). Bench: 6 shapes, 400k
triangles, 9.8 MB. Path traced and raster agree at 100% crop: the relief is geometry, so the BLAS has it.
Shader specks are cellular grains (Worley 1996), irregular polygons, with a glassier middle third (quartz).

Silhouettes: the two cross joint sets meet at 60 to 90 degrees (more spread on fresh fragments), 35% of fresh
and wall-block faces carry an en echelon step mid-face, corner cuts are three times as many on rockfall and
scree. The grid is sized by probing the planes' intersection, as acute corners reached past the old a, c, 1 box
and the mesh was cut open there (a glacial boulder rendered as an open shell).

`get_stones()["top"]` is the hull's support straight up (the box of the axes floated moss on tilted stones).
`get_stone_caps(layer, rect, tolerance = 0.01)` adds `cap_point`, `cap_normal`, `cap_radius` from surface
samples of the library mesh (one per 0.06 units): the up-facing samples round the highest one, and the radius
out to which the up-facing surface stays within the tolerance of that plane. 1282 scree stones in 65 ms.

`get_obstacles` keeps each placement cell's obstacles (16 bytes a stone) until `rebuild()`: 12.7k stones in
463 ms, then 1 ms. Big blocks (footprint over 25 cm) seat to the mean ground height round the footprint, so on
crests and knolls they sit in the ground instead of perching on the centre point (at most a third of their
height deeper).

Review pass (15 Nov, late): the close-up marks are now laid out in the stone's own frame (fixed from the field's
zero crossings before carving, shared by the base and close-up shapes, so they also match in scale and the swap
does not pop) and in metres at the family's typical size, which UV2.y carries to the shader. Granite: a crystal
map of 1.8 cm cells (quartz 30% proud, feldspar lower, each crystal a tilted facet, etched boundaries, cleavage
glints) and a biotite map of 1.3 cm clots weathered into pits, both read by the shader with the same integer hash
(rk_crystal), so colour and relief agree; flakes spalled from the rind (6 cm plates), grus pits, hairline cracks on
a 9 cm map, short hackle steps on fresh breaks. Sandstone laminae of 1.9 cm as ledges by hardness (shader colour
from the same lamina hash), limestone runnels with knife-edge crests, basalt vesicles as spherical cavities.
Fresh rockfall and scree fragments spread over the Zingg classes (32% slabs, 23% rods, 20% wedges, the rest
blocks); the scree toe gets a few 2 to 6 m blocks. Caps: the largest up-facing patch, 3 cm tolerance, on the base
shape (1196 of 1282 scree stones with a cap of 6 cm or more). Debris skirts of chips round rockfall and glacial
blocks over 0.8 m; moss favours pole-facing faces and lichen the lit ones (`pole_direction`, `aspect_strength`).

Tors and coast (15 Nov, night): `FAMILY_CORESTONE` (joint blocks between sheeting joints, at least half rounded,
flatter than wall blocks, a weathering pan on most tops: Twidale and Corbin 1963) and `LAYER_TOR`: on each 128 m
cell the most convex point of thin soil (6-point ring at 15 m, convexity of 0.6 m or more, slope under about 30
degrees) gets a tor of two to five tiers, base blocks 3 to 7 m set into the bedrock, by rock (granite 0.8, sandstone
0.5, basalt 0.3), and other crests low one-tier outcrops (Linton 1955 two-stage model). `LAYER_COAST`: sea points 0.8
to 12 m deep with land 8 m above the water within 50 m get a stack (wall blocks stood on end, sunk into each other
so the joints read as grooves, a broader foot, fallen blocks round it) or an arch (two piers and a lintel), and a
jointed rock pavement on the cliff foot with tops just above the water line (Trenhaile 1987; Sunamura 1992).

Breaking (16 Nov): Blast's own Voronoi cut gives flat cell walls (its noise option only covers slicing and cutout)
and pieces drawn with flat normals and no UVs, which reads as glass, not rock. Instead RockGenerator splits the stone
itself (`fracture_pieces`): Voronoi cells of sites spread by best-candidate sampling (Mitchell 1991), taken in
domain-warped space so the walls wander like a real fracture surface (the same idea as the noise on cuts in Houdini's
RBD Material Fracture and Chaos), stretched 1.8 to 2.6 times across the bedding of sandstone, limestone and slate so
they split into slabs. Each piece is the stone's field cut by its cell (both cells read the same wall, so they fit
exactly), meshed by surface nets like the stone, so the old surface keeps its marks and the walls are fresh breaks;
bonds come from the grid edges crossing each wall. `PhysXBlastAuthoring.build_prefractured` turns these into a Blast
asset (root plus support leaves, our bonds) and keeps the meshes (`PhysXBlastAsset.chunk_meshes`), so
`PhysXDestructible3D` draws pieces with their normals, colours and UVs. `RockField3D.break_stone(position, damage,
radius)` swaps the nearest stone for a destructible copy (its own material copy with the stone's instance data in
`rock_instance`, so it shades the same), turns its hull off and keeps it broken across rebuilds; shapes near the target
are fractured ahead on a worker (`break_prepare_radius`). Granite rockfall: 10 pieces filling the stone to 1%, 29
bonds, 88k triangles, 0.3 s per shape.

Docs review of the break (16 Nov): the break faces showed the crystal map blown up to 5 to 10 cm, because the
marks were laid out at the family's typical size and the stone (a scree-family block of 1.3 m) was four times that.
Now the shader lays mineral maps out at the stone's real size in whole octaves from its family's size, and close-up
and fracture shapes are made per octave (`marks_scale`), so relief and colour still share one map (crystals within
41% of true size; granites vary more). Close-ups keep to stones within an octave of their family's size. Breaks
are near planar walls (gentle warp) with rk_relief on them as fresh faces (granite facets, hackle risers of 3 to 6
mm, conchoidal ripples) on a grid up to 1.6 times finer than the stone's; crystals get their own facet normal in the
shader (Chermain et al. 2020 is the next step for glints). Biotite share 7%, its pits half as deep; flakes 3 to 7
mm. Cost: fractured granite shape 138k triangles, 2.2 s on a worker; close-ups near the bench target 16 shapes,
1.6M triangles, 47 MB (8 MB before, when every size shared one shape).

## Sources

Domokos et al. PNAS 2020 (arXiv 1912.04628); Mitchell SIGGRAPH 1991 (best-candidate sampling); NVIDIA Blast SDK docs (NvBlastCreateAsset, bonds); SideFX RBD Material Fracture docs; Paris et al. Visual Computer 2020 (hal-02926218);
Domokos et al. PLOS ONE 2014 (PMC3922984); Novák-Szabó et al. Sci. Adv. 2018; Szabó et al.
Nat. Commun. 2015; Havasi-Tóth and Fehér arXiv 2310.06932; Peytavie et al. CGF 2009; Dorsey et
al. SIGGRAPH 1999; Evans and Hungr 1993; Kirkby and Statham 1975; Priest 1993; Epic "PCG in
Electric Dreams"; SideFX rock generator tutorials; GDC 2018 Far Cry 5 world generation; GDC 2017
Horizon Zero Dawn GPU placement; Worley SIGGRAPH 1996; Ford and Williams 2007 (karren); Benn and Evans 2010 (striae, faceted clasts); Migoń 2006 (granite landforms); Kulander et al. 1990 (fracture surfaces); Linton 1955 and Twidale 1982 (tors); Twidale and Corbin 1963 (gnammas); Trenhaile 1987, Sunamura 1992 (rock coasts); itch.io ROCKGEN 2024, rocks-bitch, mooham3d rockgen.

### Docs review fixes (16 Nov)

- Pieces were 1.5x the stone: the breakable copy was added before its transform, so it spawned at scale 1. Now
  the transform is set first and hulls are scaled; the bench test checks pieces sum to the stone's volume and fit
  its box (0.893 of 0.896 m3).
- Pieces sunk in the ground stay put (`buried_below_y`), and the shatter impulse is 2.5 m/s, so a block breaks
  into a pile where it stood instead of a spray.
- Glossy crystals (quartz and a third of the feldspars, the cleavage faces) tilt up to about 45 degrees with
  roughness 0.12: under a low sun a scatter of them flash, raster and path traced alike. A true glint BRDF
  (Chermain et al. 2020) is the next round.
- Granite reads less like a mosaic: the crystal map is looked up through a small warp (ragged grain boundaries),
  the quartz class fades over the last tenth of a cell, and each crystal carries its own cloud (altered feldspar
  blotchy, quartz glassy and even).
- Sea rock carries the shore zones (Stephenson and Stephenson 1949; Lewis 1964): wet barnacle crust and weed
  below high water, the black Verrucaria band through the splash zone (shader `sea_level`, `tide_range`; the field
  sets the sea level from its generator). Stacks are one mass (tiers sunk deep, barely shifted); the wave-cut
  platform's blocks touch across wandering joints and step down seaward in low ledges.
- Bench: `--sea-res=N` sets the sea's simulated grid; at the default 16 m cells the water left the stack's foot dry.
- A prepared stone breaks in about 12 ms (it was 87): the Blast asset is authored on the worker with the fracture,
  each piece gets a small collision hull there (its convex hull thinned to 64 points by farthest-point sampling,
  `PhysXBlastAsset.chunk_hulls`), and PhysXDestructible3D keeps indexed chunk meshes indexed instead of unrolling
  them to a soup. Bench test: break, restore, break the same shape again.
- Close-up memory: 46.8 MB for the 16 shapes near the bench target. A second tier at 3x cells beyond 4 m saved only
  7 MB (most of those shapes have a stone within 4 m), so it was not kept. Left: compressed vertex attributes
  (about half), to measure with the GPU.
- Shore zones only on stones the sea reaches (coast, and shore stones on the sea's own water): flagged in the
  instance film (+2); by height alone they darkened inland stones near sea level.

### Coast forms as bedrock (16 Nov, docs review)

Docs declined stacks built of stood-up blocks, an arch of two piers and a lintel, and a platform of flagstones. Real
forms (Trenhaile 1987; Sunamura 1992; Kennedy 2014 on shore platforms) are one mass each: a stack is a jointed
column banded by harder and softer beds, undercut by the wave-cut notch at the waterline, opening down its joints
from the top; an arch is a headland wall the waves cut through along a joint; a platform is a bench of bedrock cut
flush at the cliff foot, crossed by joint sets opened into grooves, stepping down seaward, with pools. Paris et al.
2020 (Modeling rocky scenery using implicit blocks) build such scenery the same way: a jointed volume with implicit
cuts. Here three RockGenerator families do it on the field grid after the rounding:
- `FAMILY_SEA_STACK`: big off-axis cuts (an irregular polygon in plan), beds of uneven thickness each weathered back by
  its own amount with a sharp step between them, the notch a fifth up the solid (the field stands it with the notch at
  sea level), a taper, low-frequency relief, one or two open joints from the top;
- `FAMILY_SEA_ARCH`: the same wall with an elliptic opening from the bed through most of its height;
- `FAMILY_SEA_PLATFORM`: a thin bench with vertical cuts for an irregular edge, two joint sets (the cross set stops and
  starts), seaward ledges and 3 to 6 pools; set 0.6 m above mean sea level, its landward edge sunk into the cliff foot.
Coast forms get trimesh collision (an arch's opening stays open) and are not breakable or close-up shapes.

#### Second review (16 Nov): crisp beds, open joints, shore bands

Docs declined the stack and arch again: rounded "melted cushions" banded at even heights, a groove for a joint, no
tidal bands, a pale plinth at the waterline. Causes and fixes:
- The beds were a field offset blended over ~3 cells per bed (0.5 m beds at 0.5 m cells), plus 0.14c fbm bulges: all
  rounded. Now bed boundaries fall at random intervals (0.3 to 1.9 x a mean of ~1.5 m), half the beds soft (recessed
  0.12 to 0.24c, never two soft in a row) and the rest hard, the step between beds eased over 1.5 cells only, so a
  hard bed's lip overhangs the soft one below with an arris. Hard lips break back where a block fell out. The fbm
  bulges are gone; faces get quantised block relief (facets meeting in edges) instead. Stack resolution 160 -> 256,
  arch 180 -> 224. No spheroidal rounding on coast forms.
- One or two of the big side cuts are recent falls, flagged fresh (pale scar face, beds hardly recessed).
- Joints are planar slots (a slab removed down to 0.55c from the surface, tapering shut at the tip) instead of a
  smooth groove: a dark gap with flat walls.
- Notch: sloping floor up to a sharp visor; below it the foot stays worn back, so clear water does not show a wider
  plinth under the surface (the pale rim was that, plus the pale barnacle colour).
- Arch: taller (span/height ~1.6), legs wider than the opening, a round-headed opening whose ceiling steps in blocks a
  fifth of the thickness high (roof blocks fall one by one), walls upright below the spring.
- Shore bands never showed on coast forms: the sea flag tested the stone's ground against the sea, and a coast form's
  ground is 50 m below it. Coast forms are now always flagged when there is a sea. The barnacle band is a dull
  grey-brown crust specked with pale shells, weed below, and the black splash band reaches up to 3x the tide range.
- Fallen blocks at a stack's foot are rockfall fragments (slabs, wedges, rods), not cliff blocks.
- Bench: `--wind=m/s` (sea wind and some swell), `--seaward` (camera on the land side looking out).
Water does not yet see the forms (rock collision is off with --water, or the sea's seabed probe takes a stack for the
bed), so no foam rings a stack's foot: a water-side hook would be needed.

## Ground blending (round 3, 16 Nov)

A stone dropped on terrain shows a hard seam: its rind meets the soil with no transition, and on a slope the
contact band (one ground height per stone) sits wrong on both sides. What engines do:
- **CryEngine / KCD2 "terrain layer blend"**: objects (rocks especially) blend toward the terrain layer under them
  over a height band, with a noise and height-map term, and take the terrain normal there.
- **Unreal Runtime Virtual Texturing** (Fortnite, Valley of the Ancient): the landscape renders its final albedo,
  normal and roughness into a virtual texture; rock materials sample it near the ground and blend by height above the
  landscape (also read from the RVT) and by a noise mask.
- **Far Cry 5** (McAuley, GDC 2018 "Terrain Rendering in Far Cry 5"): objects read the terrain's height and albedo
  and blend over a depth band; the blend follows the terrain's real height under each pixel.
- **Frostbite / Battlefront** (Andersson; DICE terrain blending): a mesh blend band with height-based (Mishkinis
  2013) layering, so hollows fill with ground first and the proud parts of the stone stay clean.
- **Unity HDRP / MicroSplat "terrain blending"**: same idea with a distance-to-terrain from a height map and a
  per-object blend height.
Common pieces: (1) the terrain's height under each pixel, (2) the terrain's surface (albedo, roughness, normal) at
that point, (3) a band blend modulated by the stone's own relief (height blend) and noise, (4) the normal pulled to
the terrain's in the band so lighting agrees across the seam.

Here, without a virtual texture: (1) a ground plane per stone (height at its foot and the slope there, packed in
INSTANCE_CUSTOM.w with the random), so the band follows a slope; (2) the generator's geology map (global
terrain_geology_map) gives rock, scree, soil, alluvium and sand per pixel, coloured from the landscape's
TerrainMaterial3D layers (colour times the mean of its albedo texture), which RockField3D.ground_material hands to the
rock material; (3) a band of `ground_blend_height` (scaled by the stone's size), cavities first (Mishkinis height
blend on the stone's cavity map), with noise, upward faces more; (4) the normal eased to the ground plane's.

#### Third review (16 Nov): natural silhouettes

Docs: crisp, but both forms read as masonry (a ruined chimney of rectangular courses; a doorway with a lintel). The
references (Old Harry Rocks and Durdle Door in chalk and limestone, the Twelve Apostles in limestone) differ by
outline more than by surface:
- Stack: now built from 6 to 9 joint faces at uneven azimuths and distances (an irregular polygon in plan, not a
  box), each a few degrees off upright, the whole leaning 2 to 7 degrees one way and narrowing upward; one to three
  dominant steep joint faces at any azimuth cut into one side, deeper toward the top; the top two or three oblique
  breaks. Beds vary much more in thickness (0.2 to 2.6 x the mean, most thin) and their recess varies around the
  stack (0.3 to 1.7 x), so ledges run part way round and stop. An open joint only on about two stacks in three,
  off-centre and a little oblique. Below the notch's roof the beds are planed back alike (the lips there read as a
  plinth); the notch is shallower (0.18c).
- Arch: the opening is widest at the waterline (flare 0.45 to 0.85 x its half-width) and closes to a rounded head
  that leans to one side; above the springing its outline steps where roof blocks fell; its edges are ragged. The
  deck sags over the span (a quarter to a half of the rock above the opening, never thinner than 0.4c) and dips
  along the ridge.

## Mineral glints (round 3, 16 Nov)

The close-up crystals (granite's quartz and feldspar cleavage, tilted up to ~45 degrees, rough 0.12) flash only while
a pixel is finer than a crystal (footprint under ~3 cm): past that the crystal map fades to the mean and the sparkle
is gone, though a sunlit granite boulder at 10 to 50 m visibly glitters. Published models:
- **Jakob et al. 2014** (discrete stochastic microfacets) and **Yan et al. 2014/2016** (position-normal
  distributions): the references; offline.
- **Zirr and Kaplanyan 2016** (real-time glints): counts facets per pixel footprint with a binomial law per cell of
  a multiscale grid.
- **Chermain et al. 2020** (Procedural physically based BRDF for real-time rendering of glints, Pacific Graphics):
  multiscale procedural microfacet slopes from a dictionary of 1D distributions, converging to the smooth GGX as the
  footprint grows; energy-conserving, filtered over the footprint.
- **Deliot and Belcour 2023** (Real-time rendering of glinty appearances using distributed binomial laws on
  anisotropic grids, HPG): the same convergence with no dictionary, cheaper; Unity's take.
The engine already has the reduced Deliot and Belcour counting for snow (snow_glint: one facet per footprint cell,
weight share / chance / sqrt(share n), a coat lobe in raster) and an exact facet per grain under the path tracer
(SPECULAR_NORMAL). Minerals use the same: Chermain's convergence argument with Deliot and Belcour's counting, so it
is one technique engine-wide.
- Raster, past the resolved crystals: rk_glint() counts crystals (1.8 cm on granite, share 0.45 glossy: quartz plus a
  third of feldspars; 2 x speck size, share 0.12 on the other rocks: quartz and mica grains) per footprint cell up
  to 32 crystals a cell, the facet GGX-tilted (alpha 0.6), drawn in the clear-coat lobe (F0 0.04, minerals about
  0.045) with NORMAL_MAP solved back onto the stone's normal so its own shading is unchanged.
- Path tracer: the facet of the crystal each sample hits, in the base specular only (SPECULAR_NORMAL), rough 0.12;
  its samples count the footprint's crystals themselves.
- Covered by lichen, moss, water film, snow and the ground's dust; dulled to 35% on the weathered rind.
- `RockField3D.mineral_glints` (off until its GPU cost is timed on the PC; on in the bench); bench `--no-glints`. `ground_blend` likewise.

## Landforms (round 3, 16 Nov): plan

Bedrock forms bigger than a boulder but smaller than the terrain grid, each tied to a rock and a setting:
- **Hoodoos** (Bryce Canyon; Twidale and Campbell 2005; Schumm's badlands): columns 2 to 40 m of soft, bedded rock
  under a harder caprock that shields them; frost wedging along joints separates fins into columns (Bryce sees 200+
  freeze-thaw cycles a year). Shape: a jointed column, beds recessed unevenly (soft beds deeply, so the column
  "beads"), narrowing to a neck below a wider caprock that overhangs. Setting: soft bedded rock (sandstone,
  limestone, mudstone) on steep, bare, gullied slopes (high rock exposure, 20 to 45 degrees, high drainage density),
  in rows along the joints, at amphitheatre heads.
- **Goblins** (Goblin Valley, Entrada sandstone): 1 to 6 m knobs, rounded by spheroidal weathering and sand blast,
  mushroom heads on narrower necks, scattered on a flat, bare valley floor between mesa walls.
- **Inland arches** (Arches NP; Ostanin et al. 2017, Nature Geoscience: stress shapes arches, the load arching round
  the weak bed): sandstone fins 3 to 10 m thick in parallel rows along one joint set; a fin's weaker lower bed
  weathers through (alcove first, then a window) and the opening's roof follows the stress arch (a smooth catenary
  head, not stepped). Setting: sandstone, gentle bare bedrock (slope under 15 degrees).
- **Clints and grykes** (limestone pavement; Williams 1966, Goldie 2005): glacially scoured flat limestone dissolved
  along two joint sets into blocks (clints, 0.5 to 3 m) between slots (grykes, 0.1 to 0.5 m wide, 0.5 to 2 m deep),
  clint tops fluted with runnels (rinnenkarren) and pans (kamenitza). Setting: limestone, flat (under 8 degrees),
  bare or under thin soil, where ice scoured (the generator's ice flow map, else high exposure on flat ground).
- **Potholes** (Richardson and Carling 2005, "A typology of sculpted forms in open bedrock channels"): round,
  steep-walled hollows 0.2 to 3 m wide drilled by grinders (cobbles whirled by the flow) into bedrock river beds at
  steps and constrictions, often in lines, sometimes breached into each other; a grinder stone left inside.
  Setting: bedrock channels (rock exposure, steep reach, the river layer's beds).
- **Cave mouths** (Ford and Williams 2007): in limestone cliffs, along a bedding plane or a joint intersection; an
  arched or bedding-flat roof, breakdown blocks on the floor at the entrance, a dark interior. Setting: limestone
  cliff faces (the generator's cliff points), at the foot or along a bed.
Build: RockGenerator families (hoodoo, goblin, fin, pavement, pothole slab, cave mouth) on the same jointed implicit
field as the coast forms (Paris et al. 2020), and a RockField3D landform layer placing them from the geology, slope,
rock type and drainage, as the coast layer does. Order: pavement and hoodoos first (the most common), then fins and
arches, goblins, potholes, cave mouths.
