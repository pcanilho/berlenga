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
map of 1 cm cells (quartz 30% proud, feldspar lower, each crystal a tilted facet, etched boundaries, cleavage
glints) and a biotite map of 6 mm clots weathered into pits, both read by the shader with the same integer hash
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
- Raster, past the resolved crystals: rk_glint() counts crystals (1 cm on granite, share 0.45 glossy: quartz plus a
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

### Landforms step 1: built (16 Nov)

- `RockField3D.LAYER_LANDFORM` (radius 900 m), families `FAMILY_HOODOO` and `FAMILY_PAVEMENT`.
- Hoodoos (Bryce, Drumheller): a round, irregular plan of 11 to 15 faces; a few smooth swells and necks up the
  column (knots 0.45 to 0.95 units apart, soft ones 0.12 to 0.26c back, smoothstep between them, not steps); faint
  hard ribs every 0.07 to 0.09; rills down the sides; a thin cap (top 6 to 13%) with a domed, uneven top over a neck.
  Placed in rows of 3 to 9 on steep (20 to 45 degree) bare soft beds, only in one placement cell in four (badland
  heads), 4 to 20 m tall, the foot sunk under the lowest ground round it.
- Limestone pavement (Malham, the Burren): two joint sets, the cross set 1.4 to 2.8 times wider and intermittent, each
  joint wandering and the spacing swinging by half, so clints differ; grykes most of the slab deep; rounded clint
  edges, runnels and pans on the tops. Placed on bare limestone up to 20 degrees, only where the ground within 8 m
  stays within 0.8 m of a plane (benches, not knolls) and 6 m above the sea; 8 to 16 m patches, top 15 cm proud.
- Upright forms (hoodoos, sea stacks) now carry their beds across the long axis: the laminae relief and the shader's
  colour bands (vertex COLOR.b) were running up the column like wood grain.
- Bench: `--view=landform`, `--form=pavement`, `--only-layer`; `--test` checks both kinds are placed.

### Landforms step 2a: fins and inland arches (16 Nov)

- `FAMILY_FIN`: a wall between two joints (x along, z up, y 0.22 to 0.35 of the height thick). Whaleback crest that
  falls 15 to 50% toward the ends and wanders along the fin; ends rounded in plan; beds etched across z as smooth
  swells; an alcove on one face at a weak bed near the foot. About half carry an arch: walls at +-w (one leaning,
  both ragged), the head a cosh curve (k 1.2 to 2.7: round to tall), floor 4 to 24% up, the opening flaring where it
  meets the faces so the rim reads rounded, not drilled (Ostanin et al. 2017: the roof follows the stress arch).
- Placement: bare sandstone (a little on limestone) under about 18 degrees, in one cell in three; rows of 3 to 7 fins
  6 to 14 m apart along one joint set, each 12 to 35 m long, on the lowest ground along its foot.
- Bedrock forms (coast and landforms) now cook their triangle collision once per shape and share it between
  instances: a fin field went from 14 s to 0.2 s to build.
- Bench: `--rock=sandstone,fin --stand` (a fin, hoodoo or stack stood on end), `--form=fin`. Pavement on the steep test
  world is 0 to 2 patches, so the test counts it rather than requiring it.
- Seen, not from this work: the test's cliff layer logs "Vector3 cannot be normalized" a few times.

### Landforms step 2b: river potholes (16 Nov)

- `FAMILY_POTHOLE`: a slab of stream bedrock (x down the flow) with 1 to 3 round, undercut, round-floored potholes in a
  line, some breached into the next, spiral grinder grooves on the walls; the top fluted along the flow, the upstream
  edge rounded over and the whole top rolling down into the bed so no cut edge shows. Mesh meta `rock_potholes`
  (floor x, y, z, r) lets the field drop a rounded cobble (the grinder) into about 60% of the holes.
- Placement (landform layer, from the river paths): reaches of slope 0.03 to 0.4, bedrock exposed (or slope over 0.08),
  above the delta, one slab per 50 to 90 m, only where the bed within 40% of the slab stays within 0.5 m of its
  plane and is level across (normal y over 0.92); the top at the highest ground under it, so it never sinks.
- Bedrock that is the ground (pavement, pothole slabs) keeps the ground-blend band below its top, at the buried
  edges: flush with the soil, the whole top was being painted as grass.
- The bench's 8 m terrain grid turns most stream beds into V-gullies, so it places 0 to 2 slabs; finer terrain places
  more.

### Landforms step 2c: goblins (16 Nov)

- `FAMILY_GOBLIN` (Goblin Valley, Entrada sandstone): an upright knob 1.5 to 5 m tall, the round-plan joint block
  pulled toward a round section (spheroidal weathering), a bulbous head (Gaussian swell at 72 to 86% of the height)
  on a pinched neck over a flared foot, faint beds. Grid 128 cells (35 to 70 thousand triangles).
- Placement: flat bare sandstone (normal y over 0.97), one cell in ten or so; clusters of 6 to 20 within 25 m, each on
  the floor (local normal y over 0.9), sunk 6% of its height below the lowest ground round it.

### Landforms step 2d: cave mouths (16 Nov), off by default

- `FAMILY_CAVE_MOUTH`: a limestone outcrop (x along the cliff, z up, front +y) with a passage along a bed or a joint:
  roof bedding-flat or a cosh arch, the opening leaning along its joint, the passage narrowing by a third and swinging
  sideways as it goes in to a round blind end 1.6c deep, its floor rising to a choke; the lip rounded; the block's
  sides and top rounded back and bulging at the scale of metres, a few thick beds standing out.
- Placement (`RockField3D.cave_mouths`, default off; the bench turns it on): bare limestone wall (normal y 0.23 to
  0.64) whose foot 10 m down the fall line is level over 8 m, with the wall rising at least 8 m within 12 m behind;
  one per cell in four such cells; the portal 6 to 12 m tall standing on the foot, its back to the wall, 3 to 7
  fallen blocks at the entrance.
- Why off: a heightfield cannot open a hole into its own slope, so the portal stands in front of the wall as its own
  block and reads as an outcrop with a hole, not a cave in the cliff. A proper cave mouth needs the terrain to cut a
  hole (or a mesh tile) behind it: a follow-up with the terrain thread.

## Live rockfall (round 3, 16 Nov): research

What sets rocks off, and when:
- **Freeze-thaw.** Frost cracking works hardest between about -3 and -8 C (the frost-cracking window; Hales and
  Roering 2007, Walder and Hallet 1985). Ice holds the cracked blocks in place until it melts, so most falls come
  in the thaw that follows: rockfall peaks in spring and on mornings after a freeze (Matsuoka and Sakai 1999;
  D'Amato et al. 2016 measured falls a few hours after the temperature crosses 0 C).
- **Rain.** Rockfall rates jump during and just after heavy rain, from water pressure in the joints (Krautblatter
  and Moser 2009 in the Reintal; Delonca et al. 2014: rain above roughly 10 to 20 mm/h, and rain falling on
  thawing ground, add up).
- **Base rate.** Alpine walls shed small blocks all year round, rarely but steadily. Big falls are rarer by a
  power law of volume (Hungr et al. 1999).

How a fall moves:
- Engineering rockfall codes treat the block as a rigid body bouncing down the slope. Rockyfor3D (Dorren 2012)
  uses restitution coefficients; RAMMS::ROCK (Leine et al. 2014) keeps the real shape with non-smooth contact,
  and its shape is what makes a block roll, slide or jump. A PhysX rigid body with the stone's own hull is the
  same model.
- Blocks break up when they hit hard. The share that fragments grows with impact energy (Ruiz-Carulla et al.
  2015/2017, fractal fragmentation). PhysX Blast impact damage does this: impact_strength and
  impact_damage_scale on PhysXDestructible3D.

Plan (`RockField3D.live_rockfall`, off by default until its frame cost is measured):
- **Hazard.** Rate = `rockfall_rate` (falls per game hour in the `rockfall_radius` round the follow target) times
  a trigger factor from WeatherMap3D: 1 as the base, plus 3 in the frost-cracking window, plus 6 for two game
  hours after the air crosses 0 C going up, plus the rain rate at the target divided by 5 mm/h. Falls are drawn
  as a Poisson process per frame (game time from the weather's time_scale).
- **Which block.** A random intact cliff-wall block (LAYER_CLIFF) 20 m to `rockfall_radius` from the target. Its
  fracture is started on a worker, and the fall is spawned when the fracture is ready, so there is no hitch.
- **The fall.** The block is hidden (and its collision dropped). A PhysXDestructible3D takes its place with no
  anchor, moved out of the wall along the face normal by the depth it was set in, so it does not start inside the
  heightfield. It breaks on hard impacts (impact damage). Its pieces freeze when asleep and stay as debris.
  `rockfall_max` live falls are kept and the oldest is freed.
- **Hooks.** The signal `rockfall(position, mass)` is for sound and camera shake. `trigger_rockfall(position,
  radius)` forces a fall (for gameplay and the bench). It is synchronous, so use it sparingly.

Built (16 Nov): `live_rockfall` (off), `live_rockfall_rate` 4 per game hour, `live_rockfall_radius` 150 m,
`live_rockfall_max` 16, `get_rockfall_trigger()`, `trigger_rockfall(position, radius)`, signal `rockfall`.
- The air at the target's height takes the standard lapse (6.5 C per km) from WeatherMap3D's air temperature.
- A fall is a dynamic PhysXDestructible3D (`dynamic`, no anchor) set out of the face by the block's reach.
  Impact break: `impact_strength` = mass x 3 m/s, `impact_damage_scale` = 1 / mass, `impact_radius` = 1.5 x
  reach. Measured in the bench (Godot Physics contacts): a sliding 21 t block's single contacts take a median
  of about 1 t m/s and up to about 6 t m/s per tonne on hard knocks, so 3 m/s per tonne breaks it at the first
  real knock (11 m down a 60 degree face, into 6, then 10 pieces) and not while it slides. 2 m/s broke it at the
  first touch; 6 m/s never broke it in a 200 m fall.
- Bench: `--world --view=cliff --fall[=ticks] --fall-cam=d`; the test looses the biggest cliff block and runs
  the live path at a very high rate. The Landscape3D collision follows the camera (192 m), so a fall outside it
  drops through the ground until `kill_y` (400 m under it): keep `live_rockfall_radius` under the collision
  radius.
- A synchronous trigger on an unprepared shape costs 0.6 to 1.4 s for a big block (the fracture); the live path
  builds it on a worker first.

## Cave mouths into the terrain: holes (round 3, 16 Nov): research

Paulo (16 Nov): cave mouths wanted (they make scenes with water: sea caves), with real holes, on Landscape3D and
on Terrain3D.

How engines do it:
- **Unreal Landscape** (Visibility tool): a painted visibility layer and a Landscape Hole Material (opacity mask)
  clip the surface; collision drops the quads under the painted vertices. Lower LODs read a mip of the mask, so
  holes go blocky with distance; Epic's own guidance for caves is a hole plus a static mesh entrance that hides the
  hole's quad edges (UE docs "Landscape Visibility Tool"; forum reports of collision offset by half a quad).
- **Unity HDRP/URP terrain holes** (2019.3+): a hole texture at heightmap resolution, clipped in the pixel shader
  (so it holds at every LOD) and honoured by the TerrainCollider; the jagged edge is again hidden by meshes.
- **Terrain3D**: bit 2 of its control map is a hole (`Terrain3DData.set_control_hole(global_position, enable)`,
  then `update_maps(TYPE_CONTROL)`; `get_surface_height` and `get_mesh_vertex` answer NaN there); resolution is the
  vertex spacing. Collision follows `Terrain3DCollision.update()`.
- **PhysX heightfields** take holes as `PxHeightFieldMaterial::eHOLE` per triangle; our PhysX backend already
  turns NaN heights in a HeightMapShape3D into holes (Jolt's rule).

What fits us: the hole is per height sample (every triangle touching a hole sample is dropped, as Terrain3D and
UE do), and the cave mouth block covers its edges, as UE and Unity both recommend. Clip-in-shader holes do not suit path tracing (geometry that is not there must not be in the BLAS), so the triangles are left out of the mesh,
and a chunk holding a hole keeps its finest level so the hole does not grow or close with distance (one chunk
of 64 quads at full detail costs little).

Plan:
1. **TerrainGenerator3D holes**: `add_hole(transform, size)` (an oriented box), `clear_holes()`,
   `is_hole(position)` (the ground at that x, z lies inside a hole box), signal `holes_changed(rect)`. Holes live
   with the generator so every reader of its ground (Landscape3D, vegetation, water, rocks) can ask.
2. **Landscape3D**: hole samples from the source's holes (or `set_hole(position, enabled)` for an image source);
   triangles touching one are left out at level 0; such chunks stay at level 0; collision gets NaN heights there
   (min/max exclude them); `holes_changed` rebuilds only the chunks it covers.
3. **Cave mouths**: the portal block is set into the slope (its front at the face) instead of standing in front
   of it; the generator exports the passage's box as mesh meta, and RockField3D adds it as a hole, so the
   terrain inside the passage goes and the block's rock hides the rest. Vegetation skips holes.
4. **Terrain3D**: optional and by name (never bundled, compiles without it): RockField3D `terrain3d` node path;
   each cave's hole is written with `set_control_hole` over the vertices inside its box, then
   `update_maps(TYPE_CONTROL)` and the collision update.
5. Cave mouths on by default once the holes hold in a raster and a PT frame. Water in sea caves goes to the water
   thread through the coordinator.

## Real caves behind the mouths (16 Nov): research

Paulo (16 Nov, card): build real caves behind the cave mouths: tunnels and chambers under the terrain, joined to
the holes, including sea caves that water can fill later.

The terrain is a heightfield, so nothing can sit under it. Every engine that keeps a heightfield does caves the
same way: the cave is separate geometry under the ground, and the ground gets a hole where they meet.
- **Unreal / Unity**: landscape hole plus a cave mesh (hand-made, or from Voxel Plugin's SDF tools). Red Dead
  Redemption 2, Horizon and Far Cry all use authored cave meshes behind terrain holes.
- **Full voxel terrain** (Voxel Plugin, Zylann's Voxel Tools for Godot, No Man's Sky, Astroneer) would replace
  Landscape3D, Terrain3D support and every system that reads `get_height`. Too large a change for caves alone.
  Rejected.

What to build (the published method): **Paris, Peytavie, Guérin, Collon, Galin, "Synthesizing Geologically
Coherent Cave Networks", Computer Graphics Forum 40(7), Pacific Graphics 2021**.
- **Key points** are the entrances and exits. For us: the cave mouths (springs at cliff feet, already holes), sea
  cave mouths, and sinks (shafts) on limestone benches.
- **Points** are Poisson samples in the bedrock, denser on the inception horizons and the water table. The graph
  is each point's 40 nearest neighbours.
- **Paths** come from A* over an anisotropic cost:
  - c_L = smoothstep(|z − horizon| / r) makes bedding planes cheap. Our horizons are the strata contacts of
    TerrainGenerator3D (`strata_spacing`, `strata_dip`, `strata_wobble`).
  - c_F = Σ w_k (1 − (n_k · dir)²) makes travel along joint planes cheap. Our joints: two regional joint directions per area, the
    same ones the fins and cliff blocks follow.
  - c_π is permeability.
- **Skeleton**: the γ-skeleton keeps an edge (a, b) only if no key point k has d(a,k)^γ + d(k,b)^γ < d(a,b)^γ.
  γ = 2 gives branchwork caves; γ below 1 gives mazes. Extra random dead-end and interior points then add
  side passages. Tortuosity is midpoint displacement of up to 4 m (θ).
- **Passage types** depend on height against the water table:
  - above it (vadose): canyons, keyholes and shafts;
  - below it (phreatic): tubes and bedding-plane passages;
  - breakdown chambers where the rock is weakest.
  - Radius is 0.5 to 4 m (epikarst 0.5 m).
- **Geometry** is a signed distance field. Each passage is a profile swept along a segment:
  d = √(s² + d_C²), with s the distance past the segment's ends and d_C the 2D distance to the profile. Curved
  passages use unions of spheres. The cave is subtracted from the rock.
- **Cost in the paper**: skeleton under 1 s, field 3 to 8 s, and polygonising 1 × 1 × 0.4 km at 0.5 m takes about
  2 min (one CPU). That is too slow to do at once, so we mesh per chunk, near the player only, on workers.

Paris 2021 leaves out the small features (scallops, speleothems). We add them as real geometry (Paulo's
preference):
- wall roughness from noise displacement of the field (scallop size set by the flow speed, Curl 1974);
- stalactites and stalagmites as instanced meshes on drip points under the roof (density from the water above);
- flowstone as a smooth field offset on the walls;
- breakdown blocks in chambers from RockField3D's own stone shapes, so they break with Blast like other stones;
- sediment floors that fill the bottom of tubes flat.

**Sea caves** (coastal geomorphology: Trenhaile, *Coastal Dynamics and Landforms*; Moore 1954): wave quarrying
along a joint or fault at the notch level gives these features:
- short passages, mostly under 100 m, widest at the mouth;
- the floor at or below low tide, so the sea comes in;
- blowholes where the roof meets the surface.

They come from the same field, with key points on coast cliffs over 0.8–12 m deep water (the coast layer's rule).
Their water is the water thread's job (through the coordinator).

**Meshing**: surface nets (Gibson 1998) on 0.25 m cells near the player and coarser further away. Chunks have
shared seams, and a chunk's level steps by one at most (Transvoxel-style transition cells, Lengyel 2010). Surface
nets gives one vertex per cell, which keeps the walls smooth. That suits path tracing: the triangles go in the BLAS
like any mesh, so PT keeps the same detail as raster. Collision is a trimesh per chunk.

**Joining the terrain** (the entrance patch). The portal block alone cannot do it, which the 16 Nov docs review
showed:
- the block stands proud of any wall gentler than its lean, so it reads as a card;
- its colour differs from the cliff's;
- setting it back into the hill leaves hole edges with nothing behind them.

Paris 2021 carves the cave out of the terrain's own field: f = max(f_H, −f_K), with f_H = (p_z − h) / √(1 + λ²).
Voxel Plugin entrances over a UE landscape work the same way. So, round each entrance:
- a patch of about 15 m is meshed from that field with the same surface nets, so the cliff face, the opening and the
  passage are one surface;
- the landscape gets a hole over the patch's inner quads;
- the patch's outer ring stays on the heightfield: vertices within one cell of the patch edge are snapped to
  h(x, z), so the seam is exact;
- the patch takes the landscape's own material (TerrainMaterial3D, triplanar on the steep parts), so the face matches
  the cliff;
- the portal block becomes an optional hood of fallen rock and is no longer the opening.

The rest of the join:
- each entrance is a key point, and its passage is the first segment of the network;
- shafts that reach the ground add a TerrainGenerator3D hole (Landscape3D and Terrain3D both cut it);
- vegetation and stones already skip holes.

Plan (new node `CaveNetwork3D` in modules/rocks; reads the TerrainGenerator3D and the RockField3D; `caves` off by
default until the PC measures its cost):
1. **Network**: key points, Poisson samples, cost, A*, γ-skeleton, amplification, passage types. Bench
   `--caves --cave-graph` draws the skeleton.
2. **Geometry**:
   - swept profiles (tube, canyon, keyhole, bed);
   - chambers;
   - roughness;
   - chunked surface nets with LOD and seams;
   - collision;
   - limestone material shared with the rocks.
   The network joins the cave mouths. Raster and PT frames inside.
3. **Detail and sea caves**: speleothems, breakdown, sediment floors, sea caves and blowholes. Shafts open holes.
   Water handed to the water thread.

### Caves: second research pass (16 Nov): newer papers, itch.io and Unreal tools

Paulo asked whether itch.io gems and newer work had been checked. They had not. What they add:
- **Franke and Müller, "Procedural generation of 3D karst caves with speleothems"**, Computers & Graphics 102
  (2022). A karst density field on chunked voxels, passages by phenomenological dissolution, Marching Cubes.
  Speleothems are placed where the local conditions suit them (drip water through the roof, a free fall below) and
  grown as calcite deposits. Microforms are covered too. **Take for stage 3**: their placement rule for drip points.
- **Douda, "Cave Networks Generation"**, CTU Prague thesis (2024). Shortest paths, primitive sweeping, Marching
  Cubes, with stalactites and cave lakes. The same method we follow (Paris 2021), so it confirms the choice. Cave
  lakes go with the sea caves to the water thread.
- **"Modelling and animation of stalactite growth for virtual caves"** (Eurographics digital library): could not be
  fetched. Read it before stage 3's stalactites.
- **ProcGrotto** (itch.io, PROCJAM 2024): Perlin worms turned into a distance field, joined by a smooth minimum,
  warped by Simplex and **Worley** noise, Marching Cubes per chunk, triplanar texturing. Same structure as ours.
  **Take for stage 3**: Worley (cellular) noise for the wall relief, since real dissolution scallops are cells
  (their length is set by the flow speed, Curl 1974).
- **Instant Organic Caves** (itch.io and Fab, Unreal 5.7, MIT on GitHub): Perlin noise, a 3D cellular automaton,
  Laplacian smoothing. Blobby game caves with no geology. Not taken.
- **Terra Works** (Fab, Nov 2026): 30 cave schemes under an Unreal landscape. No method published.
- Cellular automaton and hex-graph generators on itch.io (Evans, Serra) build rooms for levels, not natural caves.

So nothing found beats Paris 2021 for the network, and surface nets matches or betters the Marching Cubes the
others use (fewer slivers, smoother walls). Stage 3 takes the Franke and Müller drip-point rule and Worley scallops.

### Caves stage 1: the network (16 Nov)

CaveNetwork3D (modules/rocks) grows a network from an entrance (`find_entrances`, `add_cave`). The method is Paris et
al. 2021, with these changes:
- **Cost**: a water-table term (level travel near the table is cheap) and an incline term (sin 2θ, so travel is
  level or vertical, rarely at 45 degrees). Without them the paths to sinks 130 m up hugged the hillside just under
  the cover as one long inclined canyon. With them the network is a bedding-plane trunk near the spring, plus
  shafts and canyons up to the sinks.
- **Graph**: 20 nearest neighbours instead of 40 (at 7 m spacing the paths are as straight).
- **Cover**: grows 0.08 m per metre in, up to 15 m more.

On the bench world (1 to 1.2 km of passage each, 250 m reach):
- 5,800 to 50,000 samples;
- 0.2 to 1.8 s on one core (the sample count follows the height of the sinks above the entrance);
- no passage under 1 m of rock.

Bench: `--caves[=N]` draws the skeleton through the ground, coloured by passage type.

Cave mouth review fixes (16 Nov, same commit):
- the portal block takes the landscape's rock colour (`cliff_match`);
- the passage is kept inside the block's full-thickness middle (it broke out of the side and left a floor plate);
- the passage walls are rougher inside;
- landscape hole quads are tested over the ground's full span across the quad, so a steep wall crossing the passage
  is cut (`TerrainGenerator3D.is_hole_span`, also for Terrain3D vertices);
- `cave_mouths` is off by default again: a block alone stands proud of gentle walls. The entrance patch in stage 2
  replaces it as the opening.

### Caves stage 2: the meshes (16 Nov)

CaveNetwork3D builds meshes for its caves (`build_meshes(area)`), off until called:
- **Field**: one signed distance F = min(ground, cave air). The ground term is the height difference over
  √(1+|∇h|²). The air is a sweep of per-type profiles: round tubes, wide flat beds, keyholes, tall canyons, shafts.
  Passages blend with a smooth minimum (k 0.8). Chambers are flat-floored ellipsoids (k 2). Wall roughness is a two
  octave value noise at 2.2 m and 0.7 m (`roughness`).
- **Mesh**: surface nets (Gibson 1998) in 16 m chunks with whole cells (`mesh_cell`, 0.3 m), meshed on worker
  threads. Each chunk is a MeshInstance3D with a matching trimesh collider (`collision`). Normals come from the field
  gradient, so chunks meet without seams.
- **Entrance patch**: inside a square (`patch_radius`, 14 m) along the entrance's facing, the mesh also carries the
  ground. The landscape is cut by a hole box 1.5 m smaller than the patch, so the opening is part of the hillside
  with no portal block. The patch uses the landscape material (`ground_material`); the walls use a world-space
  limestone shader that takes the landscape's rock colour unless `wall_material` is set.
- **Holes**: the landscape now cuts holes only at its finest level, so coarse rings no longer open early.

Streaming (`follow_target`, `stream_radius` 64 m): chunks round the target are meshed on low-priority workers,
nearest first, each time it moves 4 m; chunks past the radius plus about two chunks are freed. Entrance patch chunks
stay, since the landscape's hole is open from any distance.

Cost on the bench (one cave, 40 m around the entrance at 0.3 m): 27 chunks, about 139k triangles (88k wall, 51k
ground), 14 s on the cloud machine without a GPU. Streaming near the camera and coarser cells far away come next.

Fixes in the same commit: stones on slopes are seated on the lowest band of their hull (no more hovering talus), and
the rock field rebuilds cells when the terrain's holes change.

### Caves stage 3: drip formations, scallops, sea caves (16 Nov)

- **Speleothems** (`speleothems`, density, 1 by default; the placement rule of Franke and Müller 2022). Every meshed
  wall vertex under a roof (normal facing down) is a candidate drip point. It is taken more often where the rock
  above is thin, since thin cover lets more water seep through. It needs a free fall of at least 0.6 m, and there
  are none within 14 m of an entrance (air exchange dries the walls). Each drip point gets a stalactite and usually
  a stalagmite where the drop lands; where they would meet, a column. Lengths follow a power law: many short, few
  long. The shapes are lathed meshes with soft growth rings and a lobed, slowly twisting section, instanced per chunk
  (MultiMesh), in a wet calcite material. About 340 in one bench cave.
- **Scallops** (`scallops`, 1 by default): Worley (cellular) hollows 1.2 m across with ridges between them. These
  are the large scallops of slow phreatic flow (Curl 1974); the small fast-flow ones are below the 0.3 m mesh cell.
- **Sea caves** (`find_sea_entrances`, `add_sea_cave`; Moore 1954, Trenhaile):
  - Mouths are cliffs at least 8 m over the sea, with 0.3 to 25 m of water in front. A beach at low tide still
    floods at high water.
  - One straight passage along a joint (the facing turned by up to 15 degrees), 35 to 95 m long. It stops where the
    rock over it or to either side gets thin.
  - The section is a tall arch (`PASSAGE_SEA`), widest at the mouth, floored 1.5 m below the sea at the mouth (level with the beach where one runs in front) and
    rising to high water at the back.
  - Half of them end in a chamber. Where its roof is under 25 m thick, a shaft opens to the ground as a blowhole:
    a terrain hole, see `get_blowholes`.
  - No speleothems in sea caves (spray and waves).
  - The water inside comes from the sea itself: the sea reads the cave floor through its terrain holes (#330).

### Caves stage 3b: docs review rework (16 Nov)

The review asked for irregular, fused formations seated in the rock, wall texture with real relief, and sea caves
that are not round tubes.
- **Drip points from the field**, not the mesh. They sit on a jittered 0.6 m grid, mostly along joint lines 3.5 m
  apart, so formations stand in rows and curtains. Chambers get twice the density. Each grid column is walked down
  on a fixed global step, so every chunk finds the same drips.
- **Flowstone**: a mound under each standing form and a boss round each hanging one, added to the rock with a
  smooth maximum, so the forms rise out of the floor and roof. The mesher passes the coating to the wall shader
  (vertex red), which turns it to wet calcite.
- **Shapes**:
  - Stalactites follow the universal carrot of Short, Baygents and Goldstein (Physics of Fluids 2005): a flared base
    tapering to a soda-straw drip tip.
  - Stalagmites are candles with spread feet; columns are waisted.
  - Curtains are folded sheets 2 to 3 cm thick along inclined roofs, with drip tips on their lower edge.
  - Four variants per kind: lumpy (3D noise, not lathe rings), bent, and some fused twins and triplets.
  - The calcite shader gives wet tips and wet, muddy feet.
- **Walls**: the shader bumps the normal with relief the 0.3 m mesh cannot hold (spalls, pits, bedding steps). It
  also draws two vertical joint sets and the beds as broken crack lines, and flowstone streaks down the fall line.
  Floors are uneven (ripples, mud banks).
- **Sea cave section**: cut along a joint and its beds, with straight sides stepped at each bed, a gabled roof and
  a dipping joint. The width varies along the passage.
- **Fallen blocks** (rounded, spalled boxes): breakdown slabs in chambers and boulders in sea caves.
- **Water**: the bench coast is a beach, so its sea cave floors stand dry above the water at this tide. The sea fills
  caves at plunging cliffs, where it reads the cave floor through the terrain hole (#330, `terrain_source`).

Defaults (Paulo, 16 Nov: every feature on by default; the PC tunes cost later): `ground_blend`, `mineral_glints` and
`live_rockfall` are now on. `cave_mouths` (the portal block) stays off: the meshed entrance patch of CaveNetwork3D
replaced it as the opening.
`ground_material` is now an override: unset, the field takes the TerrainMaterial3D of the Landscape3D drawing its
generator, so the blend matches the ground with no wiring. The blend band is thin by design (`ground_blend_height`
0.25 m times sqrt(scale): a full foot ~10 cm up a 1 m stone, dust to ~45 cm); scene's seat3 tries 0.45.

## Scree as real stones (18 Nov, with terrain)

Talus is clast-supported: the stones touch, some 60 to 70% of the ground under them, gravel between (Statham 1976;
Sass and Krautblatter 2007). The scree layer now tries four times the blocks a 65% cover needs (a power law of b axes
cut at 0.9 m, mean b squared about 3.4 dmin^2) and lays them largest first where each touches but does not pass
through those already down (footprint 0.45 b, nestled to 0.8 of the sum), then a 10 to 15 cm cobble tail in the gaps
(about 6% more; RockField3D `ground_detail` draws the 1 to 15 cm chips on the GPU, so the field keeps to the coarse
end). Each sunk a fifth to a third. Talus drains, so the ground contact map draws no damp ring there and the banked
fines at half strength (both read as dirty smudges on dry grey talus). Inside the scree radius the ground's own
parallax blocks go: the field sets TerrainMaterial3D.scree_blocks_distance to the layer's radius whenever its ground
material has it. `max_stones_per_cell` 20000 (a 32 m scree cell holds about 13000).

## Stone and ground contact (Paulo, 17 Nov: "stone and ground side")

The ground blend paints only the stone, and on grey scree the stone and ground colours are close, so it hardly
reads. What the eye uses to see a stone sitting in soil is on the ground as much as on the stone.

What others do:
- **Terrain blending on the object**: Far Cry 5 (GDC 2018, "Terrain Rendering in Far Cry 5") and UE's Runtime
  Virtual Texture blending (Megascans rocks) sample the terrain's own colour and normal near the object's base.
  We already do this (`ground_blend`). The flat ground colour reads as paint, so it needs the ground's grain.
- **Objects writing into the terrain**: UE "Draw in Virtual Texture" lets a mesh render a mask into the
  terrain's RVT (dirt and damp under rocks, paths). Frostbite (Andersson 2007; Widmark 2012, "Terrain in
  Battlefield 3") uses terrain decals and procedural masks. We have no RVT, but we do have a working pattern:
  a CPU-written global mask the terrain material samples (`terrain_cover_map` from vegetation).
- **The physics of it**: rock fragments act as a mulch. Soil under and beside a stone loses less water, so it is
  wetter and darker (Poesen and Lavee 1994, "Rock fragments in top soils"). On a slope, a stone traps the fines
  washed down against its uphill side, and the downslope lee holds less of them. Wet porous ground darkens and
  smooths (Lagarde 2013, already used for the sand wetness).

Plan:
- **Stone side** (rock shader): the blend band takes the ground's grain (fines with pale and dark chips) instead
  of a flat colour, a damp foot (darker, less rough), and the band rising on the uphill side where debris banks up.
- **Ground side**: `rock_contact_map`, a global R8G8 mask around the follow target (64 m at 6.25 cm). RockField3D
  stamps each near stone's footprint:
  - R: a damp ring falling off over about a third of the stone's radius;
  - G: fines and grit banked on the uphill side.
  TerrainMaterial3D reads it the same way it reads `terrain_cover_map`: wet darkening and lower roughness from R,
  and a fines tint with a little grit from G. Under the path tracer it is the same shader, so the two match.
- Setting: `ground_contact` (on). It is redrawn when the target moves 8 m or cells change.

## Moss cushions as real geometry (Paulo, 17 Nov: "all three, in that order", first)

Moss on stones is a painted tint today (`moss_cover` in the rock shader). Up close it reads flat,
and Paulo prefers real geometry to texture tricks (fungi, ivy, mushrooms). Vegetation already
grows separate cushion plants on stone tops (`stone_top` species on `get_stone_caps`). What is
missing is the moss mat itself: the layer that wraps a stone's damp, shaded faces and hollows.

What is known:
- **Where it grows**: epilithic bryophytes follow moisture. They grow on the shaded (pole-facing)
  sides, tops that hold water, hollows, and the foot near damp ground. Fresh breaks stay bare
  (Bates 2000, "Bryophyte ecology"; Glime, "Bryophyte Ecology", ch. 8 on substrate).
- **Growth forms** (Mägdefrau 1982): mats (Hypnum on sides), cushions (Grimmia and Racomitrium
  on tops), and turfs. Mats are 0.5 to 2 cm deep and cushions 2 to 5 cm, with ragged, lobed
  margins.
- **How others build it**:
  - Shell or fur layers (Lengyel 2001) fake depth with stacked textures. That is the texture
    trick we want to avoid, and it is wrong under the path tracer.
  - Megascans and the UE5 demos use scanned moss meshes.
  - Our trees thread (trees.md 5c, round 6) builds moss on logs as an offset shell of the bark
    mesh. The shell thins to nothing at a ragged margin, and shoots are carried by the material.
    That pattern holds under PT.

Plan (`moss_cushions`, on):
- **Placement**: stones drawn with their close-up shape (within `close_up_radius`) get a moss
  shell built on a worker from that shape. The moss weight is computed per vertex with the
  shader's own moss terms (up-facing, hollow, pole-facing, near the ground, not fresh, the same
  noise), so the shell sits exactly where the tint is.
- **Shape**: the shell is lifted along the normal by a lumpy depth (mats about 1 cm, cushions up
  to 3 to 4 cm where the weight is high). It sinks 3 mm into the rock at its ragged margin, so it
  grows out of the stone with no seam or z-fight.
- **Material**: moss with shoots of a few millimetres (domain-warped noise in the normal),
  greener in hollows and yellow-olive on sunny tops, matte, with soft thin-shoot transmission.
  The shoots grow in tufts about 12 mm across (lit tops, dark gaps, relief), the scale a mat reads at from a metre:
  the 2 mm shoots are under a pixel there, so the path tracer averages them out and raster aliases them. The
  pattern space is the stone's rest position in UV and UV2 (metres, seeded per stone), as the rock shader's.
  The same shader runs under PT.
- **Cost**: one mesh per near stone, built once and kept while it is near.

## Rockfall dust with PhysX Flow (Paulo, 17 Nov: second of the three)

What a real fall does. A block that hits a slope throws up a dust cloud at each impact, not a steady trail. The
cloud's fines come from the fresh fracture and crushed contact surfaces, so the dust grows with the impact energy and
with fragmentation (Ruiz-Carulla et al. 2015, fragmentation against impact energy; the 2017 Pousset rockfall, De Biagi,
Crosta et al.: dust clouds from fragmentation at the impact points). The biggest falls make an air blast that carries
the cloud downslope (Morrissey et al. 1999, the 1996 Happy Isles event, Yosemite). Rolling and sliding blocks abrade
a little dust between hits. Dust is heavier than air: it slumps, rolls out along the ground, and settles over tens of
seconds. Wet rock in rain makes far less of it.

How engines drive it. Unreal's Chaos destruction feeds Niagara through the Chaos Destruction data interface: collision,
break and trailing events, each filtered by a minimum impulse or speed, spawn the dust. Frostbite's destruction works
the same way, with impulse-driven dust emitters. We do the same, but the dust goes into a fluid solver rather than into
sprites.

What we build:
- RockField3D measures every live piece of its falls on the physics tick (PhysXDestructible3D pieces, tracked by
  chunk). An impact is a speed change of more than 2.5 m/s in one tick. Its dust is the kinetic energy it lost times
  `rockfall_dust_per_joule` (kg/J), placed at the piece's underside. A piece that breaks off sheds a little more (new
  fracture faces), and a piece sliding or rolling on the ground sheds a trickle. Rain on the rock cuts all of it.
- PhysXRockDust3D (godot_physx, Flow) claims those events near the follow target. It feeds them through a pool of
  sphere emitters, each a short burst about the size of the piece, with a jet along the piece's motion plus an outward
  swell. The dust is heavier than air (negative smoke buoyancy), fades slowly, rests on the terrain floor (the
  generator's heights), curls round the moving pieces as solids, and drifts with the weather's wind.
- RockField3D makes one itself when Flow is built in (`rockfall_dust`, on).

## Cave rimstone pools and sea-cave mist (Paulo, 17 Nov: third of the three)

Rimstone (gours). Water spreading thinly over a cave floor loses CO2 fastest where it speeds up and thins, over
any slight rise, so calcite grows there first. The rise becomes a dam, and the dam holds a pool behind it (Hammer,
Dysthe, Jamtveit et al. 2007 and 2010, "The dynamics of travertine terrace formation": growth set by the local
flow thickness and speed gives self-organised terraces whatever the slope). Pools step down the floor in a cascade.
Each rim is a few centimetres wide, rounded, sinuous and crenellated, and its crest is close to level (the water
trims it). The pools are clear and still, often with a white crystal lining, and their levels step down the slope.
Steeper floors give smaller, closer rims (microgours); gentle ones give wide shallow pools. In games, caves show
these as flat decals or sculpted props. We build the dams as real geometry from the floor's slope and drip.

What we build:
- CaveNetwork3D places rimstone cascades on chamber and passage floors where the floor falls gently (2 to 20
  degrees) under drip formations (the water comes from the roof). Each cascade is a run of pools down the fall line.
  Each pool's dam is a sinuous crescent across the slope, with a level crest whose height is set by the pool's
  spill level, a rounded section, and crenellations. The pool fills behind it to the crest.
- The water is the fork's own still water (PhysXWaterSurface3D with a surface_mesh outline, as lakes use), so it
  takes shadows, refraction and transmission like every other water body. Underground it has no sky reflection and
  no caustics, and it steps once per drawn frame. Each surface is a ripple simulation of its own, so only the
  `rimstone_water_max` (12) pools nearest the camera within `rimstone_water_radius` (16 m) get one. The rest show their
  calcite-lined floor dry until the camera comes near.

Built (17 Nov): `CaveNetwork3D.rimstone` (density, 1) and `rimstone_water`. The chunk owning a standing formation's
foot fits a plane to the floor round its flowstone mound. Where that floor falls 2 to 20 degrees, the chunk walks
2 to 6 pools down the fall line, steering with the local cross slope. Each pool's rim height is 9 cm on gentle
floors, falling to 3 cm on steep ones. The pool length is height over slope, and the width is 1.6 to 2.8 times the
length. The crest is bowed downstream and wanders. It runs out to each side until it meets floor rising above the
level or a wall, so a level-bottomed passage gets dams wall to wall. A pool that would spill round an open end, or
whose dam would stand over 25 cm, ends the cascade. The dam is a swept section: a rounded top 4 mm over the water,
a lobed downstream face flaring with the drop, and a buried foot. The floor inside each pool is coloured as calcite.
Bench: `--caves --cave-mesh --cave-view=rimstone [--pool-water=n | --no-pool-water]`. Cost: once the water surfaces shared one
compiled shader (afead43f), each live pool costs about 50 MB and 30 ms a frame on lavapipe; the PC queue measures
it on the GPU.

Grown from the floor (22 Nov). The dams read as tubes laid on the floor: a round section standing on beach sand or
cave earth, a hard line where the two met. Rimstone is one deposit (Hammer et al. 2007): the spill over each dam coats
the floor below it, and the pools sit in a single flowstone sheet. Now:
- Each dam's downstream face is a concave apron of flowstone flaring out over the floor below it (one and a half times
  the drop), meeting it at a low angle, its outer edge buried; the apron's edge takes the sheet's colour, so there is
  no line where the dam meets the floor. The upstream face stays a steep lip into the pool.
- The floor inside and round each pool (fading over 0.45 m) is flowstone (the mesher's red), so the cascade stands in
  one calcite sheet. The fill's grit and the rock's pits are off under flowstone, and the sheet carries microgours:
  sinuous rims a few centimetres apart, each a sharp lip with a gentle back (contours of a smooth field cut as a
  sawtooth, in the normal only, fading by 6 m).
Bench: `--cave-view=rimstone --low --downstream` frames a dam from below. The view now passes over pools in passages
too low for the camera (it had put the camera and lamp inside the rock, a black frame). PT check of the pair: PC queue
(the world's PT still runs out of memory in the cloud).

Test note (22 Nov): the "standing in air" check read the rock field without the flowstone, so pools on a formation's
mound or on the rimstone sheet showed air under their floor. `get_meshed_rock_distance` is the field as meshed (with the
flowstone), and the check reads it for the floor.

Sea-cave mist, built (17 Nov): PhysXCaveMist3D (godot_physx, Flow). A CaveNetwork3D with sea caves makes one itself
(`sea_mist`, on). Each of the 4 nearest sea-cave mouths whose floor the sea reaches gets an emitter: a low slab across
the mouth's lower half, 2 m inside and 0.6 m over the water. The emitter pulses with the swell (the sea's
`shore_wave_period`, else 10 s). Each cave runs on its own phase. On a surge the slab pushes mist out at up to
`push_speed` (2 m/s); in the trough it draws a little air back. The mist is pale, heavy (`heaviness` 0.4) and
evaporates (`evaporation` 0.25), and it never jets up, so it spills out level and sinks over the water. Bench:
`--sea-caves --cave-mesh --water --mist-period=2.5 --settle=200`, run with `--fixed-fps 60` (Flow steps per drawn
frame). The bench's sea cave opens onto a beach, so a mouth with the swell in it is still to be framed (PC queue).
- Sea-cave mouths: cold mist in PhysX Flow. Spray and saturated air pushed out of the cave by the swell, where the
  sea is in the mouth. A slow, heavy, pale Flow emitter at the mouth, pulsing with the swell, near the target only.

## Salt bloom (19 Nov, from the weather's sea spray)

Sea spray leaves salt on the shore stone (WeatherMap3D `sea_spray`, the `weather_spray_map` global, G = 1 - exp(-g/m2 /
10)). Where the film dries the salt crystallises at the surface as a white halite crust (efflorescence; Mustoe 1982,
Goudie and Viles 1997, Huinink et al. 2004 on salt and tafoni): heaviest in the sheltered stone (hollows, ledge
undersides, overhangs), thin on open faces, washed off the lines rain runs down. It dissolves in any water film, so it
shows only on dry stone and returns as the stone dries. The rock shader reads `weather_salt_at` (shared with the salt
burn on plants): crust = smoothstep(0.2, 0.8, salt x (0.5 + 0.5 shelter) x (1 - 0.6 runnel) + noise), off where wet,
in the tidal film, under moss and mostly under lichen; matte, near white. Setting: rock material `salt_bloom` (on).
Bench: `--rock=... --salt=k` (a stand-in salt index), `--no-salt-bloom` for a before frame.

## Sea stack faces (19 Nov, round 5)

The C++ `rk_fbm` returns -1..1, but the coast and landform shaping used it as 0..1 (`rk_fbm(...) - 0.5`), so every such
term pushed the whole surface out by half its amplitude. On stacks, whose amplitudes are large, the faces were pushed
out to the voxel pass's cutoff (0.5c past the cut) and came out as flat planes there: the "slab" at 30 m. Fixed
everywhere (the terms are centred now; the other forms change by a few percent of their thickness), and the stack's
own terms retuned on the centred noise: beds set back 0.07 to 0.2c (hard ones standing 0.05c proud), clefts opened
along a share of the joints (a slot 0.24 to 0.44c wide and 0.55c deep, dying out up the column), a plan that changes
with height, the voxel pass reaching 1.2c outside the cut. Hoodoos and goblins take the slope's rock colour
(`cliff_match`), like cliff blocks and cave mouths: they are the bedrock left standing.

Round 8 (20 Nov). The lower half was smooth lobes, and two seeds stood top-heavy (a slab on a narrow foot, a ball on a
stem). Now:
- The foot is cut flat where it stands on the platform (the body's rounded end had stood it on a stem), and the notch
  is a band: under its floor the foot stands out again.
- Low down the blocks stand out most (quarrying along the joints): block offsets up to 0.14c and fallen blocks
  0.2c back below a third of the height; the face swells only start above that.
- No level wider than the foot, no knob on a neck: each level's plan (a chamfer distance in the plan) must lie inside
  the foot's plan grown by half a lip low down and shrunk by a lip at the top, and inside every plan between the
  notch's roof and it, grown by a lip (0.1c). A slot narrower than the lip is not a neck. The cut follows quantised
  blocks, so it reads as a fallen face. Toppling stability: Hoek and Bray, Rock Slope Engineering (1981), ch. 9.
- Hoodoo caps: chips off the cap edge, a tilt of 3 to 6 degrees, and tafoni down to a few (paired pits read as eyes).

Round 9 (20 Nov). Checked against the foot only, the column still overhung its lower third at the notch's roof (a
shelf), with big smooth faces above. Now every level must lie inside the plan of the levels just below it (their
union over about a bed, 0.3c, so a soft bed's groove does not count as a neck), grown by a lip of 0.1c (about 0.3 m
on a 25 m stack), and inside every such plan further down, from the foot to the top; each bed's cut sits back by its
own amount, in blocks. The notch is shallow (0.07c, no caves under it), bed setbacks 0.04 to 0.1c, the block offsets
and fallen blocks all the way up, face swells 0.1c. Hoodoo caps: a slab about a quarter as thick as it is wide, flat
topped, tilted 2 to 4 degrees, standing out all round (a little more on one side).

## Far rocks (19 Nov, Paulo picked all three)

Before: each layer ended at a fixed radius with a hard cut (scree 60 m, river 120 m, glacial 300 m, rockfall 600 m,
tors, coast forms and landforms 900 m), and the ground past it showed nothing of the fields. References: Horizon Zero
Dawn's GPU placement (van Muijden, GDC 2017) uses the same density maps near and as terrain tint far; Ghost of
Tsushima holds full density over the inner half of a range and thins in steps (Rockenbeck, GDC 2021); Unreal 5 keeps
big rocks as cheap far geometry (Nanite, HLOD) and cross-fades instead of popping. Same design as the vegetation's far
reach (docs/ideas/vegetation.md, "Far vegetation without bald ground").

1. Thin, don't pop (`far_thinning`, on). Each cell's records go to the GPU in rank order (a hash per stone, the big
   ones last); past half a scatter layer's radius a cell draws only the first (r/2 / d)^2 of them, in eighths, and
   none by the radius. Landmark layers (tors, coast, landforms) stay whole.
2. Big stones to the horizon: stones over about 2 m, tors, stacks and hoodoos to 4 km on their coarsest LOD; cells
   past the near radius keep only those, rebuilt in full as the camera nears. Built (`far_radius` 4000 m,
   `far_min_size` 2 m): blocks of 8 x 8 cells, each cell placed and packed exactly as its near cell (same seed),
   keeping the records of its big stones, which lead each group's records, so a thinning near cell keeps them to the
   end and the swap is the same stone. A block holds only the cells no near cell covers and is placed again on a
   worker when that set changes. Cave mouths (which open the terrain as they are placed) are not drawn far.
3. Far ground takes the rocks' colour: a rock cover map to 4 km (32 m texels) from the same placement rules, read
   by the terrain material as a tint and roughness where each field is.

Added 20 Nov (ground detail thread, Paulo's pick "thinning plus 4 km rock colour"):
- `far_widen` (on): a thinned cell's survivors past the far tier's big stones grow so their plan cover (reach
  squared) matches the whole group's, capped at 1.6x, as Ghost of Tsushima widens its thinned grass. The cover holds
  to about 0.8 of the radius, where the terrain's own scree blocks come in. Drawn buffer only: xforms, colliders,
  breakables and the far blocks' records stay at true size. Scree bench, eye level round the camera: 226 of 715
  thinned groups widened, 1.36x on average; raster and PT keep the edge's stone cover.
- build_area does not thin (tried 20 Nov, reverted): with ~1,170 far coarse MultiMeshes thinned (all 55 m and more
  from the camera) the near shore grains of a path-traced still turned into dark pits and smooth patches, while the
  raster frame was identical; leaving the shore layer unthinned mostly cured it. Not the off-screen cull, not
  c1f09374, not zero counts. Open: whether play (streaming thins every frame) shows the same in PT; check on the PC.
  Narrowed 21 Nov (cloud, shore eye view 640x360, 4 spp; PT is deterministic there, so frames diff exactly):
  - Raster is bit-identical. PT loses whole near boulders and grain patches, but their shadows stay, so the
    stones are still in the TLAS for shadow rays and are gone for camera rays.
  - Thinning only the first 100 or 600 groups changes nothing; 750 breaks it. Groups 601-750 are all layer 4,
    n about 17-23 halved, 72 m and more out. Those groups cut 448 TLAS instances, so they cross
    `rendering/pathtracing/multimesh_merged_blas_max_triangles` (65536): from expanded TLAS entries to one merged BLAS.
  - With the cap at 256 (every MultiMesh expanded) the thinned still matches the unthinned one. So the fault is in
    the path tracer's merged-MultiMesh path when MultiMeshes become merged, not in the rocks.
  - Ruled out: widening (`--no-far-widen` still breaks), `persistent_instances` off, build budget (60 more frames
    make it worse), allocation errors (none logged).
  - 21 Nov, later (main 1259619e on): still broken, and now worse since PT stills wait for first captures. Every
    TLAS entry has its camera bit, none is shadow-only, every row index is in range, and the MultiMeshes within 40 m
    of the camera are planned identically thinned and unthinned (same counts, all expanded or merged alike). The dark
    spots left where boulders were match the ground's contact darkening, so the boulders are likely gone from the
    TLAS altogether rather than hidden from camera rays. Next suspect: the per-mesh BLAS that expanded copies share
    (cache key = mesh RID index and surface), lost when far copies of the same mesh turn merged.
  - 22 Nov (main 986d3581, still broken; parked for the weather work). Ruled out: the near merged entries (every
    merged MultiMesh within 30 m has the same count, primitives, divisor, flags, scheduler action and record
    positions thinned and unthinned); the BLAS batch's scratch rollover (scratch raised to 4 GB: same frame); a
    first-frame race (every merged BLAS forced to rebuild 15 frames later, a few per frame: same frame). No single
    group does it: thinning groups 600-675 alone changes 855 pixels, 675-750 alone none, 0-750 together 10,700, so a
    total crosses a line. It is not an overflow: the broken frame has fewer TLAS instances (28,255 against 28,703
    good and 32,207 unthinned), fewer primitives (256 M against 259 M and 301 M) and fewer rows. Next: look at what
    the camera rays hit at a missing boulder's pixel (instance and primitive ids, a debug output in raygen), and the
    merged bakes' GPU contents rather than their CPU records.
  - 22 Nov, evening (main 6f33ff9f): gone. The same repro (shore eye view 640x360, 4 spp, first 750 groups thinned,
    --no-far-widen, plus --no-tiles since tile erosion now moves the shore) gives a thinned still bit-identical to the
    unthinned one (0 pixels over 20 against 10,718 on 986d3581). Not from b06913a51d (the compiling-shader fallback):
    with it taken out the pair still matches. Camera rays forced opaque change nothing either, so the any-hit path
    is clear. Some change between 986d3581 and 6f33ff9f removed it (or moved the far groups off the line); stills may
    thin again once a PC check of a thinned build_area still in PT agrees.
  - 22 Nov: build_area thins again (`_update_thinning` at the target), so stills show the field as play does. The
    next PC PT still of a --world view is the confirmation.
- The far cover map takes coarse river beds (45% stone over the bed where the median is drawn near). In the bench
  world only narrow boulder streams qualify, so it adds 67 faint texels; it matters where big rivers carry boulders.
  Shore stones are left out: the shingle band is a few metres wide, under a tenth of a 32 m texel.
- Bench: --focus=camera, --no-far-widen, --save-far-cover=path; get_far_cover_texture().

## Next options (17 Nov research, nothing built before Paulo picks)

1. Tafoni and honeycomb weathering as real geometry. Coastal sandstone and granite boulders hollow out into
   cavernous pits (tafoni, decimetres to metres) and cell-like honeycombs (centimetres) where salt crystallises in
   the drying zone just inside the surface. The 2023 model of moisture transport and salt deposition (Geosciences
   13(6) 161, "Mathematical Simulation of Honeycomb Weathering via Moisture Transport and Salt Deposition") grows them
   from wetting and drying: hollows deepen where the rock dries slowly, and walls between them survive where it dries
   fast. Plan: carve the hollows into the stone's own mesh (RockGenerator, coast and sea-spray zones and dry faces),
   with sizes from exposure to salt (the sea's spray reach) and aspect; small honeycombs on the fins and stacks,
   large tafoni on boulders and cliff feet. Real geometry, so PT keeps the cavities' shadow and occlusion.
   Built (17 Nov): RockGenerator `salt` (-1 = from the family: 1 for sea stacks, arches and platforms, 0.4 for cave
   mouths, 0 elsewhere; `get_effective_salt`). Tafoni strength = lithology (sandstone 1, granite 0.8, limestone 0.5)
   times the larger of salt and an inland term (0.6 for corestones, hoodoos, goblins and fins, else from
   weathering), times detail; none on pebbles, cobbles, rockfall, scree or cliff blocks. 2 to 12 hollows clustered
   round one or two faces, mostly the lower half, each a squashed sphere sunk 0.55 R past the face and raised 0.22 R
   so a hood stays over its opening, with 1 to 3 nested hollows in its back wall; carved over the whole fine field
   before the weathering band, in the stone's stood-up frame (a column's x, a fin's z). Honeycomb: Worley cells
   (F2 - F1 walls) about 1.3 periods across over the sides, in patches, deepest where the tafoni are. Bench
   `--rock=sandstone,hoodoo --stand`, frames in fork/rocks/tafoni.
2. Rain-wet rock with rivulets. Under rain, rock darkens and gains a water film first where water collects. Then thin
   rivulets run down the fall lines of faces and drip from overhangs, and the stone dries from the tops and the sunny
   faces after the rain (Tatarchuk, "Artist-Directable Real-Time Rain Rendering in City Environments", 2006;
   fxguide's "Game environments: making wet environments"). Plan: wetness per stone from WeatherMap3D rain and
   drying, a flow field down each stone's faces baked with the mesh (steepest descent on the surface), rivulets
   along its converging lines with a thin film and lit drops at the lips, in raster and PT.
   Built (17 Nov): each stone's mesh carries where rain runs, COLOR.a = 1 - log2(1 + upslope cells) / 12, from
   multiple-flow-direction routing down the mesh in its stood-up frame (Freeman 1991; Quinn et al. 1991, slope to
   the power 1.1); 1 means no data. The rock shader reads the weather map: while it rains (state map R, mm/h) the
   steep faces carry rivulets, wavy lines of standing water a few mm to 1.4 cm wide, about 6 cm apart, counted
   round the stone's vertical axis (so the bumpy normal cannot scatter them), more and wider where the flow
   converges, broken where the film thins, with beads running down them; past a pixel they fade to their share
   of the face. Drying (state map G) is patchy: the open tops and the sunny side first, the hollows, the shaded
   side and the flow lines last. Material `rain_rivulets` (on). Bench `--rock=... --rain=4 [--dry-hours=0.4]`,
   `--no-rivulets` for a before frame; frames in fork/rocks/wet. Not yet: drops hanging from the overhang lips.
3. Cave streams. Vadose canyons and keyhole trenches carry streams; sea caves take the swell. Plan: a PhysXRiver3D
   along the trench floors of passages above the water table (from the network's own segments), sinks and resurgences
   at its key points, and the swell running in and out of sea caves (the sea surface reaching in), so the mist and
   the cave's sound follow real water.
   Built (17 Nov, first stage): CaveNetwork3D `cave_streams` (on). Runs of joined keyhole and canyon segments at
   least 8 m long, about two in three of them (the rest abandoned, dry), carry a stream (Segment.stream): their fill
   is washed level across the trench 0.1 r below the dry fill, and a PhysXRiver3D (given depth 0.08 r, at least
   6 cm; no snapping, no carving, raster reflections off as the pools) runs along it downhill to where the passage
   meets the water table. Bench `--caves=1 --cave-mesh --cave-view=stream`, `--no-streams`. 18 Nov: clear water a few
   centimetres deep shows by its bed, not its body (as in any caver's photograph): the mesher marks the stream's bed
   and a margin 12 cm plus 0.1 r wider than the water (vertex green, _stream_wet), and the wall shader lays washed
   gravel and cobbles there, darker than the dry fill, wet and glossy; with the lamp glints on the water (#359) the
   stream reads in raster and PT alike. 18 Nov, second pass (docs review): the bed stones are real cobbles (14
   shapes: limestone from subrounded to well rounded, 0.6 to 3.5 abrasion lengths of transport; sandstone, granite and
   basalt washed in, one in eight), dart-thrown so none goes through another, each set on the field's own floor and
   sunk 30 to 50% of its height, washed clean (no soil contact band); under water they and the bed lose their own
   sheen (Fresnel at 1.5 / 1.33), the water surface carries it. The fill meets every wall in a bank (a smooth maximum
   of the floor and the section, 0.3 r wide) instead of a crease, with spalls from the wall on it (scree-family
   limestone at each wall's foot, and paler flakes in the shader where vertex blue, _wall_foot, marks the foot).
   Wall joints are dark recessed slots (albedo to 0.22 in the crack, kinked at the centimetre, hairline to a few
   centimetres wide, broken en echelon), and the wall bump's tilt is bounded at about 25 degrees: unbounded, the
   summed octaves turned normals past the lamp into black blots and a groove's lit side into a white wire. Close up,
   the micrite has its own millimetre speckle. Bench stream view: 0.4 m over the bed, 50 degree lens, a wide
   segment, and a second lamp 5 m downstream 0.2 m over the water so the surface glints toward the lens.
   Still to do: sinks and resurgences, the sea swell into sea caves (needs a
   coast with a cliff at the swash line; none of the generated coasts has one yet).
   Sea mouths (17 Nov): find_sea_entrances loosened for cliffed coasts with a beach or platform at the foot (top 6 m
   over the sea, sea within 150 m, a steep rise to 6 m within 6 to 25 m behind the mouth); CAVE_SEA_DEBUG=1 prints
   why candidates fail. 18 Nov: a 40 m beach let mouths open onto dry sand, which is not a sea cave; the foot must now
   stand within 8 m of the waterline with the ground between under high-water swash (sea + 0.6 m), and the mouth
   sits at the last swashed point.

## Next round candidates (research notes, 18 Nov)

Four options put to Paulo after the tafoni, wet rock and cave stream round. Nothing built yet.

### Lichen on rock
- What it is: crustose lichens (flat crusts: map lichen *Rhizocarpon geographicum*, *Lecanora*, *Lecidea*), foliose
  (leafy lobes lifting a millimetre or two: *Xanthoria parietina*, *Parmelia*) and fruticose (tufts: *Ramalina* on
  sea cliffs). They grow as round rosettes from a point, at under a millimetre a year for map lichen (lichenometry,
  Armstrong 2011; Bradwell 2009), merging into patches over centuries. So patch size tells age: fresh fracture
  faces and rockfall scars are bare, old tors are covered.
- Placement: on exposed, lit, well-drained faces (the opposite of moss, which wants shade and damp). Bird perches,
  where nitrogen falls, are orange (*Xanthoria*). Coasts have zones above the tide (Fletcher 1973, "The ecology of
  marine (littoral) lichens"): black *Verrucaria maura* in the splash zone, orange *Caloplaca*/*Xanthoria* above it,
  then grey *Ramalina* and *Lecanora* tufts. Acid granite and siliceous rock take yellow-green map lichen; limestone
  takes grey-white crusts and pitting (endolithic lichens etch it).
- Technique: placement is the moss pass's input (exposure, light, slope, moisture, salt), with lichen taking the
  lit, dry share. Crusts in the shader as Voronoi rosettes with lobed rims (the rosette's own age gives its radius);
  foliose lobes as real relief in the mesh near the eye (the stones are dense enough at 1 to 2 cm cells), as moss
  cushions are. Fresh faces (Blast breaks, rockfall scars, the `break_stone` cut) start bare.
- Compare: Quixel scans bake lichen into the albedo; KCD2 and Crysis show it as colour at range. Here the gain is
  that age, exposure and spray drive it, and it never covers a fresh break.
- Built (18 Nov): the rock shader's lichen is thalli, not noise patches. Each 9 cm cell may hold one colony at a
  random point, its radius from its age (lichen_cover) and from a patch field read at its own point (so colonies at
  a patch's edge are small and whole); the colony holding a point is the one with the least distance over radius (a
  weighted Voronoi diagram), so neighbours meet in lines, drawn black where the species has a prothallus (the map in
  map lichen). Lobed margins (1 cm lobes, finer crenulation), areolae (3 mm cracks) and black or coloured apothecia
  near the eye, faded to their share past a pixel. Species per colony by substrate (material `calcareous`, on for
  limestone) and exposure: siliceous rock takes yellow-green Rhizocarpon, grey Lecanora and foliose Parmelia, black
  Lecidea, orange Xanthoria on a few tops; limestone white-grey Aspicilia, dark Verrucaria, orange Caloplaca, grey
  Lecanora. No screen derivatives (the path tracer's hit shaders have none): the pixel footprint comes from the
  view distance, as the crystals' does. Placement as before: open,
  lit, old faces, never fresh breaks or under a water film. Not yet: crust and foliose relief, coastal zonation
  of the lichens above the splash zone.

### Stream sinks and springs
- Karst hydrology (Ford and Williams 2007, "Karst Hydrogeology and Geomorphology", ch. 5 to 7): a surface stream
  that crosses from impermeable rock onto limestone sinks at a swallet (a swallow hole). Upstream of it lies a blind
  valley, a dry bed that floods only in storms. The water runs through the cave's vadose passages and comes out at a
  resurgence (a spring or a vauclusian rise) at base level, often where the limestone meets an impermeable bed or
  the valley floor.
- Hooks: CaveNetwork3D already marks vadose stream runs. A sink joins a PhysXRiver3D's end to a cave entrance (the
  river thread owns the river; the coordinator routes it). A spring starts a river from a cave mouth low on a valley
  side, with tufa (calcite) at the spring and its first fall. Seasons: dry beds downstream of the sink in summer.

### Frost-shattered block fields
- Periglacial weathering (Ballantyne 2018, "Periglacial Geomorphology"; Hallet, Walder and Stubbs 1991; Murton,
  Peterson and Ozouf 2006): ice grows in cracks over long cold spells, not just by freeze-thaw, and splits rock along
  its joints. On flat, high ground the result is a block field (felsenmeer): openwork angular boulders, a metre or
  two thick, with lichen-dark tops and fresh, pale undersides where blocks have recently turned. On slopes it is
  talus, and under it there are sorted stone circles and stripes on fine ground.
- Hooks: the joint sets RockGenerator already cuts give the split faces; terrain-gen's scree already places stones.
  Block fields would be a RockField3D layer above the tree line (WeatherMap3D temperature by height), with stripes
  on slopes over 2 degrees. Snow fills the gaps (the snow thread).

### Desert varnish and rock coatings
- Rock varnish (Dorn and Oberlander 1981; Dorn 2009, "Rock varnish and its use to study climatic change"; Liu and
  Broecker 2000): a manganese and iron oxide coating 5 to 200 micrometres thick, built by microbes and dust over
  thousands of years. It is black where manganese dominates and orange-brown where iron does. It runs in dark
  streaks (varnish curtains) down the paths runoff takes, and is darkest on the upper faces of stable stones. Case
  hardening (Conca and Rossman 1982) leaves a hard rind over a soft core, which the tafoni already carve. Salt bands
  and a pale ground-line ring mark where stones meet the sand.
- Technique: the MFD flow routing baked for rivulets (COLOR.a) already gives the runoff paths, so curtains follow it.
  Varnish age comes from the stone's exposure (rockfall and Blast breaks are fresh, so bright). Shader-only, in
  raster and PT alike.

## Sea cave walls in fixed-camera stills (20 Nov)

The bench's `--cam` stills meshed every cave chunk on the coarse 0.3 m grid, while play refines the chunks within
10 m of the camera to the 0.1 m grid with its knob and spall relief. Lit walls looked smeared and a vertical seam
showed on the coarse mesh. Stills now refine round the camera (`--no-refine` keeps the coarse grid). Chunk borders were
checked: the field, border vertices and normals of two neighbouring chunks match exactly. A refined chunk now fades
its fine relief out over the last 3 m to a face whose neighbour is coarse (geomorphing, as in chunked LOD terrain:
Ulrich 2002, Lengyel 2010), and is remeshed when that neighbour's grid changes. Debug views: CAVE_WALL_DEBUG=3 chunk
grid, 4 vertex colour, 5 mesh normal, 6 bumped normal, 7 world x/z bands.

## Sea cave patch fitted to the passage (20 Nov)

A pale strip showed through the right side of a sea arch: the landscape's own cliff, 20 to 35 m out, seen from behind
through the arch. The entrance patch (where the cave mesh draws the ground and the landscape has its hole) was a
square of the mouth's size round the entrance, and the passage's air reached the ground well past it. Outside the patch
the landscape is drawn, so its cliff stood inside the passage.

`_fit_patch` now samples the ground surface round each entrance (80 m square, 0.5 m grid) and grows the patch to every
point the passage holds more than 0.3 m clear of the ground, plus 2.5 m (the hole is 1.5 m inside the patch). The patch
became a rectangle along the facing: half width, reach behind and in front of the entrance. The beach in front of a
sea cave, whose floor runs a few centimetres under the sand, stays the landscape's, so the sand is not cut wider than
the passage needs.

## Sea cave mouth crease (21 Nov)

A dark straight blade ran down one side of a sea arch, seen from inside looking out. The debug views (`CAVE_GROUND_DEBUG`,
`CAVE_WALL_DEBUG=5`) showed it was the crease where the patch's ground meets the cave's side. The ground there is the
heightfield's own surface, flat across each 5 m generator cell, and a sea cave's side is cut along a joint, close to a
plane. Two planes meet in a straight line. Neither the patch outline nor the overlap band was the cause: dropping the
cave's ground from the band changed nothing on screen.

On rock slopes (20 to 32 degrees and up) inside a patch, the ground now gets relief: noise of 2.5 m amplitude over 4 m,
1.25 m over 1.6 m, and the wall's finer octaves. It fades to nothing 1 m inside the patch edge, so it meets the
landscape. At 0.4 of that amplitude the crease stayed straight. `CAVE_PATCH_DEBUG=1` prints each patch's fit.

(For a day the cave's ground also stopped 0.5 m outside the landscape's hole. That was undone on 22 Nov: the landscape
drops each quad whose centre is in the hole, so its edge can stand half a quad outside it, and hairline cracks along
that edge showed the bright sea through as a dotted pale streak. The cave's ground again covers the whole patch.)

Follow-up (21 Nov): the lower end of the crease still showed as a thin pale streak down to the water, where the relief
was still fading in near the patch edge. The fitted patch now reaches 4 m past the passage's air (was 2.5 m).

The same shots logged about 15,000 "Vector3 cannot be normalized" warnings. The landscape cuts its collision hole with
NaN heights, and Godot Physics' height map still built triangles over them: the sea's contact-foam rays and Flow's
floor probe got NaN planes. Height-map triangles with a NaN corner now count as hole (ray test and shape cull), and
the per-chunk height bounds skip NaN. Found by linking an unstripped binary and breaking on `_err_print_error` in gdb.

Follow-up (22 Nov): a faint dotted hairline stayed down the mouth in the lit sea cave shot. Pixel checks put every dot
on the outermost pixels of the cave's ground along its silhouette, and the dots vanish when that ground is drawn
without specular: the low sun ahead (12 degrees up) glints off the wet ground seen exactly edge on, under a pixel wide.
It is not a crack (shadow bias, chunk seams, the fine/coarse seam and the landscape overlap were each ruled out).
Changes made on the way: chunk sample and vertex positions now come from whole world cell indices (N * c is not
exactly chunk_size in floating point), and the crest relief starts 2 m inside the patch, past where the landscape's
ground can reach (its hole is 1.5 m in, less half a quad), with the fitted patch 5 m past the passage's air.

Arch shards (22 Nov): along a sea arch's edge, shards of rock seemed to float free against the sky. The cave's ground
is drawn with the landscape's TerrainMaterial3D, whose vertex() lowers the ground by the GroundSurface3D skirt (0.5 m)
inside a beach patch so the patch draws over it. The patch is cut over the cave's hole like the landscape, so there the
cave's ground is the only surface and dropped half a metre off the walls. The cave's ground now uses its own instance of
the material with ground_deform_enabled off (a duplicate() would share the source's shader), and tucks 4 cm into the
rock where it overlaps the landscape and the patch (to 2 m inside the patch's edge) so the two do not z-fight.

## Stones on the drawn ground (22 Nov)

Landscape3D draws cliff and bank relief over the heightfield (up to about 0.8 m on 40 to 55 degree soil banks,
57b253b5) and answers WorldGround.relief_at. RockField3D's `_rest` seats every stone on the heightfield plus that
relief (centre, normal, footprint ring and underside), so stones on a relief-drawn bank neither float over its
hollows nor sink into its lobes. Cliff blocks and coast forms still read the heightfield (they are the bedrock the
relief is drawn over). Test: "stones on relief-drawn ground" (seated within 2 cm of the drawn surface).


## No twins in packed beds (23 Nov)

Audit (`--repetition`, middle 240 m round the scree view): for each stone, its nearest neighbour within four
footprints; "same shape" is the same library and variant, "twins" also within 15% of the scale and 25 degrees of
the rotation. Before: scree 8.2% same shape (0.3% twins), river cobbles 10.6% (1.5% twins: their yaw only spread
17 degrees round the imbrication bearing). Two changes:

- Packed beds (scree, river, shore, detail) pick each stone's variant away from its neighbours: a variant that a
  stone of the same library within four sizes already has is drawn again, up to three times (greedy colouring, as
  tile sets avoid two equal neighbours). A 1 m bin grid over the cell, lists through the placed stones.
- River cobbles: the a axis is a line, so either end points to the left bank, and the bearing spreads 26 degrees
  either way (a-axis fabrics of imbricated gravel are broad).

After: scree 3.0% same shape (0.1% twins), river 0.6% (0.1%). Rockfall 3.7% (no twins), unchanged.

## Camera cut-out and the path tracer (23 Nov)

Custom shaders that adopted the camera cut-out discard were flagged alpha-clipped, and the path tracer's any-hit
dropped them: near rocks went missing in path traced frames while raster drew them (a tor at 4 m: base and close-up
shapes both gone; with the cut-out line removed they came back). camera_cutout_discard() now returns false in path
tracer shaders (raygen starts camera rays past the cut), and a shader whose only discards are the guarded cut-out
stays opaque with no any-hit (12b84872).

## Instanced cells in the path tracer, measured (23 Nov)

`rt_instanced_cells` stays off. At the gully (1080, path traced) it traced 1.4% slower, and at 640 it held 1.37 GB
more (AS 1676 against 1061 MB, buffers 2264 against 1507 MB). There only 168 small groups (396 stones, 4.8M
triangles) take the merged path anyway; the rest are over the cap and already instanced, and the toggle adds the CPU
copy of every instanced group's records. It would pay only where many groups sit under the cap.

## Ledge slabs (23 Nov)

Hard beds over weaker ones weather into benches whose caprock stands out over the lip: the weak bed under it retreats
faster (differential erosion), the cap hangs out as a cantilever until it breaks off along its cross joints and a fresh
tension fracture, and the block topples onto the talus (Lamb and Dietrich 2009, block toppling of jointed caprock;
Niagara's and the Colorado Plateau's ledges). The overhang is limited by the cap's strength and thickness.

- Shape: `RockGenerator.FAMILY_LEDGE`, a tabular slab (x along the lip 1.5 to 2.6 times its depth, y the bed's
  thickness, 0.17 to 0.4 of its depth, z out over the lip). The beds' laminae run in it as in a cliff block. The lip
  carries one or two slanted breaks (the last sometimes fresh), the soffit's front edge is bevelled up (the drip line),
  the ends are cross joints at a slant in plan, and 60% of slabs have a stress-release crack open behind the lip.
- Placement: `RockField3D.ledge_slabs` (on), in the cliff layer, along `TerrainGenerator3D.get_ledge_lips` (terrain's
  contract, docs/ideas/terrain-generation.md, "Ledge lips"). Slab after slab along each lip, each as thick as the lip's
  cap and hanging out by its overhang, but never more than 45% of its depth (as much lies under the bench as hangs
  out). The top sits a few centimetres under the drawn bench's edge (heightfield plus WorldGround.relief_at) and
  follows the bench's rise behind the lip, never a shelf on it. The lip points come in no order, so each slab (or
  gap) claims its stretch of lip. The lip's `weight` is the chance of a slab, so gaps show where blocks fell. Ends and
  back must stand on the bench, so a spur or gully corner gets none. Its own random stream keeps the cliff blocks'
  draws unchanged. Bench view: `--world --view=cliff --form=ledge` (the biggest sunlit slab, from out over its lip).
- The library key's family field grew to 5 bits (17 families); the roundness bin moved up one bit.
- Plants keep off them (24 Nov): `get_obstacles` now includes the cliff layer while `ledge_slabs` is on, placing
  only its ledge slabs (the wall blocks stand in faces too steep for plants). Each slab is a row of discs a depth
  apart along the lip, as wide as the slab is deep, at the top's height, with obstacle height 0 (so not a wind
  obstacle). Test: "ledge slabs as plant obstacles".

## Shape libraries in the world cache (24 Nov)

Library shapes are a pure function of (type, family, seed, transport, detail, marks_scale) and the
engine build, so each built shape (ImporterMesh surface data plus its meta, materials stripped) is
stored in the world cache (`rock_shape_*`) and reloaded with the shared rock material. Close-ups and
fracture pieces are not cached. Same bench view, cold vs warm: shape libraries 10.8 s to 0.6 s, frames
byte-identical. Turning the world cache off restores the old path.

## Ledge slab runs and get_ledge_slabs (24 Nov)

Slabs were beams up to 15 bed thicknesses long and 6 deep, so a lip held one, and benches narrower than its depth
held none. Joint spacing scales with bed thickness, about one to three thicknesses for both sets (Ladeira and Price
1981, Narr and Suppe 1991), so the slab family now has both its length and depth in that range. A slab also tilts
with the bench's fall along the lip, is drawn back from the lip a third of its hang at a time where the bench bends,
and tries the shortest joint block before giving up. About half the lip points the terrain reports still seat none:
there the ground falls a metre or more per metre along the "lip" (a steep face, not a bench), so the slab is left out.
RockField3D.get_ledge_slabs(rect) lists the placed slabs with their top, front edge, along/out directions, size and
run (slabs end to end along one lip); the bench's `--view=cliff --form=ledge-run` frames the longest run.

## Pedestals on the shared bare patches (24 Nov)

_rain_soil spread the climate's bare share evenly, so stones stood on pedestals in meadow that the plants and the
terrain material drew as sward. It now reads the shared pattern (WorldContracts::bare_patch with the share at the
point, the same as climate_bare_patch_at): a pedestal only on bare soil patches, each soil stone checked at its own
spot. get_pedestal_spots(rect, step, max) lists bare spots, deepest splash first, for framing.

Cobble caps (25 Nov). A column is capped at 0.35 of its cap's width, and the soil stones are 1 to 8 cm, so no column
stood above about 3 cm however much soil the rain took. Field pedestals of 5 to 10 cm, on badly eroding slopes and
fresh burns, stand under cobbles. Now 4 percent of the pebble class's soil stones are 15 to 40 cm cobbles, with the same
rule and cap. Default valley: a few cm of soil lost is right (mm to a few cm a wet season). For 5 to 15 cm, use a burn
scar under half a year old on a bare 15 to 25 degree slope with 1200 mm or more of rain (10 to 14 cm lost), or a fully
bare 20 degree dry patch (about 7 cm).

## Falling ledges (24 Nov)

Technique: cantilever failure. An undercut caprock slab fails at its root over the bench's edge, where the bending
moment is greatest, along a joint or the stress-release crack behind the lip, and the part out over the lip topples
and falls (Young and Ashford 2008 for notched sea cliffs; Stock et al. 2012 for Yosemite's slab and cantilever
failures; Lamb and Dietrich 2009 for toppling joint blocks). Ledge slabs now join live rockfall. A failing slab is
swapped for its Blast pieces as a static destructible held at its back face (PhysXDestructible3D ANCHOR_BACK, new),
and a crack is opened across it over the bench's edge (PhysXDestructible3D.apply_plane_cut, new: exactly the bonds
between chunks on opposite sides of the plane break). The back stays under the bench; the front is cut free, falls
under PhysX as one piece, breaks where it lands by the usual impact rule and raises the rockfall dust. Where its
underside starts in the face below the lip it eases out (max depenetration 1 m/s) rather than leaping.

Later the same day: slabs need a real edge (the ground in front of the lip a cap's thickness down, past the most a
slab hangs), slide along the lip to abut a neighbour instead of being dropped, and a run follows the bed's dip. A
slab whose free part would start in the ground is never loosed. The rain-pedestal bare check reads the climate's
precipitation captured on the main thread (the weather provider is a node; workers read only the factor map).

Open (24 Nov, evening): measured with the bench's `--ledge-fall` probe, the ground under a slab's free part is the
face below the lip at about 60 degrees, and its collision (the landscape, heightfield plus relief) stands over the
free part's middle: the relief draws the soffit's undercut for the eye but no heightfield can hold it, so the freed
part starts inside the one-sided surface and falls through it. A slab now falls only where its free part's middle
starts in the air (on the bench world the 8 biggest are refused, the 9th is loosed); live rockfall leaves ledges out. Falling
ledges need collision for the undercut (for example a soffit collision strip along the lips from the terrain side).

## Close-up shapes get LODs (24 Nov)

A 480 px raster frame over a talus field drew 238M primitives on the rocks bench (658M in the showcase scene). The
`--no-close-up` A/B drew 9.2M: the close-up shapes (millimetre fabric, within close_up_radius of the follow target)
were built without LODs, so every stone near the player drew its full grid in the main pass and in every shadow
cascade, however small it was on screen. They now get mesh LODs like the base shapes (the renderer's 1 px rule, so
full detail wherever a pixel shows it), and every rock MultiMesh takes its biggest stone's scale as its lod_bias
(the shapes are built a unit across and the renderer does not see the instance scale). Same bench frame: 11.2M
primitives, image difference at the run-to-run noise (4.6% of pixels over 8/255 against 4.2% between two runs of
the old build). The path tracer keeps full meshes (rendering/pathtracing/mesh_lod off). Tried and dropped: splitting
heavy cell groups into 8 m clusters (no gain, twice the draw calls). `--prim-report` lists the heaviest MultiMeshes.

## River bed stones off the line (24 Nov)

A narrow steep rill (well under a metre wide, cobbles of 20 to 30 cm) laid its coarse stones in a band 0.6 of its width
either side of the line, so they ran single file and evenly spaced along each straight polyline segment, like stepping
stones. Now the band is at least three medians wide (the bed's coarse grains sit side by side), the stones keep to the
riffles (the detail bed's riffle phase, about six widths apart; pools between hold few) and gather about a thalweg
that swings from bank to bank at twice the riffle spacing (alternate bars). Same count per segment on average.

## Ledge slabs in the cliff's own rock (25 Nov)

Ledge slabs drew in their own stone colour, pale procedural granite on a grey scanned face, so they read as wedges
stuck on rather than the bench's caprock standing out. They now join the wall's blocks (cave mouths, cliff courses,
stacks) under cliff_match, and cliff_match also takes the landscape rock layer's scan: TerrainMaterial3D's
tm_rock_albedo_tex and tm_rock_normal_tex at the layer's tile size, triplanar in world space with the UDN normal blend
(Golus 2017), the colour over the texture's mean so it keeps the face's mean colour. The stone keeps its own light
and dark (beds, marks, rind) on top. Without a scan on the ground material nothing changes.

## Scanned detail on stones and caves (25 Nov)

Under the scan rule (docs/ideas/scan-textures.md): stones take a scan as detail only, triplanar in the stone's own
frame (it moves with a loosed or broken stone) at the scan's real size, its colour divided by its linear mean so the
rock type and its mottling, beds and rind keep the colour, and its normal on the surface's (UDN blend, Golus 2017).
Two scans, half the stones each, so neighbours of one shape do not carry one texture: rock_boulder_dry (1.8 m, p98/mean
1.68) and rock_surface (2 m, 1.73). Not on ground detail grains (a metre-scale scan on a 2 cm grain is a tint) nor on
the wall's own blocks (cliff_match takes the landscape's rock scan). Cave walls take rock_face_03 (2.7 m, 1.88) in world
space on the rock, not the fill, flowstone or stream bed. rock_wall_02 was turned down (p98/mean 2.5, wet moss tones).
Loaded by RockField3D.load_scan_set and CaveNetwork3D.load_scan_set from the art folder's scans.cfg sections marked
system="rocks" (TerrainMaterial3D.load_scan_set skips them); benches: --stone-scans=0 for the A/B.
Follow-up (25 Nov): the wall's blocks take the cliff overlay's face scan where the ground has one (what the face past
about 45 degrees shows, rock_face_03), not the rock layer's, divided so their mean is the face's own (the face's
albedo_mean when detail_only, times the rock texture's mean that ground_rock already holds); and their light and dark
is capped at the wall's colour (it ran to 1.3 times: a block paler than the face round it reads as a plate stuck on).

## Wall blocks on steep faces (25 Nov)

A heightfield cannot hold a wall. Past about 60 degrees one terrain cell covers metres of height with only a few
vertices, so the face is a stretched step. Its shading and textures draw long vertical bands (draped folds) that no
texture projection hides. Open worlds built on heightfields cover these faces with real geometry. Horizon Zero Dawn
placed rock and cliff kit pieces by slope density maps (Guerrilla, GDC 2017, "GPU-Based Run-Time Procedural Placement
in Horizon Zero Dawn"). Far Cry 5 placed cliff meshes along steep terrain (Carrier, GDC 2018, "Procedural World
Generation of Far Cry 5").

Here the cliff layer's coursed blocks are that kit. They used to stop at 76 degrees, the faces that need them most.
Now they run to `cliff_block_max_slope` (88 degrees by default, 76 for the old look). On faces steeper than 68 degrees
the courses are nearly closed (95 percent kept, not about 65), and a block may stand up to 0.45 of a course proud of a
fold, because on a sheer wall that reads as a jutting joint block, not a shelf. The face-fit test (the face must stay
near planar where the block sits) is now measured square to the face and over the wall's own width. A whole terrain
cell up and down the fall line landed on the bench above and the foot below, which rejected every sheer wall.

Bench (rocks, --world --world-size=1024 --view=cliff, fixed camera): 159k to 365k stones; frame primitives 6.9M to
10.0M in 111 to 723 draw calls; rock build 34 s to 110 s. The path tracer uses the full meshes.

### Jointed rock, not masonry (25 Nov, second pass)

Fixed 1.4 m courses with one block per 3 m cell read as man-made rows of same-sized bricks. Real sheer faces break
along bedding and joint sets:
- **Bed thickness varies.** In a sedimentary pile it is close to log-normal. The height is now cut into packages of
  about 3.6 m with jittered tops, each split into one to three beds of 0.4 m and up. The cut is the same for every
  cell, so a bed runs on along the wall.
- **Joints are spaced in proportion to bed thickness.** Thin beds break into short blocks and thick beds into long
  ones (Narr and Suppe 1991, "Joint spacing in sedimentary rocks"; Bai and Pollard 2000, fracture spacing). Each bed
  has its own ratio, 1 to 2.6 times its thickness, with joints jittered by 0.3 of the spacing either way. A quarter of joints never opened, so some
  blocks span two or three spacings and blocks along one bed differ in length. Joints are spaced along a world axis, so
  neighbouring cells agree.
- **Blocks fill the span between joints, less an open joint.** Of three shapes, the one whose proportions best match
  the span is used. The open joints read as shadowed lines.
- **Hard and soft beds weather apart (differential erosion).** Each bed has a hardness. On faces past 60 degrees a
  hard bed stands out half its depth and a soft one a fifth, with more gaps where blocks have weathered out. The
  setback blends in from 60 to 75 degrees, so the 50 to 60 degree faces keep the flush blocks that avoid pale shelves.
- **Down to 50 degrees (third pass).** Terrain tried joint-bounded pillars as offset relief, and they do not hold past
  about 60 degrees, so the placed blocks cover the fluted walls from 50 degrees. Every face the layer takes now gets the
  closed beds. The setback blends from flush at 50 degrees to fully weathered at 65.
- **On sheer faces the rock exposure map no longer gates placement.** Soil does not hold there, and the map fades
  out over the flutes. Faces past 50 degrees need only 0.15 instead of 0.5.

Rocks bench cliff view: 362k to 368k stones. --slope-map=cx,cz,half,step prints a slope map, to find the faces past
60 degrees.

## Cliff kit on fluted walls (25 Nov)

A rounded or fluted wall of 50 to 75 degrees turns away the bed blocks. Their fit tests need the face near planar
across a block, so the heightfield's vertical flutes stayed bare. Terrain tried joint-bounded pillars as offset relief
on these faces, and offset relief does not hold past about 60 degrees. Open worlds on heightfields cover such faces
with placed kit meshes, fitted to the terrain they stand over:
- Horizon Zero Dawn placed rock and cliff pieces by density maps and fitted each piece to the terrain (Guerrilla,
  GDC 2017, "GPU-Based Run-Time Procedural Placement in Horizon Zero Dawn").
- Far Cry 5 placed cliff meshes along steep terrain and sank them in, so the heightfield's stretched faces sit behind
  real geometry (Carrier, GDC 2018, "Procedural World Generation of Far Cry 5").

Here the kit pieces are the cliff layer's own joint blocks, stood on end as pillars. This is how a sheer jointed face
breaks: vertical joint sets cut each bed into columns.
- **Tiers.** Pillars come in tiers of about 5.5 m up the face, with jittered tops.
- **Width.** Each tier's pillars are a third to three fifths of its height wide. Their joints are spaced along a world
  axis so neighbouring cells agree.
- **Fitted plane.** Each piece's plane comes from the face's gradient over 5 m, across a flute rather than one flank of
  it. The face is sampled at 9 points over the piece's footprint and measured square to that plane.
- **Placement.** The piece's front stands 0.3 m proud of the highest ridge, because the drawn face sits decimetres off
  the bilinear heights on a steep face. Its back must reach 0.15 m behind the deepest groove, so no piece floats.
- **Rejected.** A footprint whose ridge stands more than 2 m out of the plane gets no piece; that is a convex corner or
  a ledge, where a piece reads as a slab.
- **Bed blocks.** The bed blocks a piece would bury are left out, so the stone count barely changes (cliff-foot view:
  357k to 363k).

Second pass, to remove visible repetition:
- **Beds weather out in stretches.** Runs of two to five joint spans either stay or go, 30 percent gone, so no bed
  stands out as one unbroken course.
- **Blocks vary.** Each block sits up or down in its bed by a quarter of the bed, fills 62 to 94 percent of its joint
  span, has its own setback (0.3 of its depth), and tilts on its bed.
- **Pillars vary.** Kit pillars sit up or down their tier by 0.3 of it.
- **Deep grooves.** A piece whose back cannot reach behind a groove is tried again at 1.35 and 1.8 times its size,
  deeper as well as larger.
- **Kit range.** Kit pieces start at 50 degrees, like the beds.
- **Skyline.** The wall must go on past a piece's top (within a quarter of its height) and sides (within 0.6 of its
  width). A grown piece once stood on a spire's tip above the skyline. A narrow convex spire now keeps its bed blocks
  rather than pillars.

On the cliff-foot view the dark vertical bands left on the far right are on faces of 40 to 50 degrees (--slope-map).
These are the terrain material's streaks, below where the wall's rock goes, and they show the same with the cliff
layer off.

On the cliff-foot view the fit tests pass 88 percent of the tried pieces. `cliff_kit` (on by default) and the bench's
--no-cliff-kit give the before frame. The path tracer draws the same full meshes.

## Cliff kit recess contract (25 Nov)

Rigid pillars against flutes metres deep either sink in (only dark specks and feet show) or stand stuck on. No
placement rule fixes that, so the terrain steps back under the kit the way Far Cry 5 sinks its heightfield under
cliff meshes (Carrier, GDC 2018, "Procedural World Generation of Far Cry 5"): the pillars become the visible wall.
Agreed with terrain on 25 Nov.

- **Contract.** `WorldRecessProvider::contract_recess_at(p) -> (depth, share)` in world_contracts.h, script side
  `WorldGround.recess_at(position)`, `has_recess()`, `notify_recess_changed(AABB)` and signal
  `recess_changed(box)`. `p` is a point of the drawn, unrecessed face. Depth is metres back along the face's
  horizontal outward normal. Share is 1 where a pillar's front covers the point and fades to 0 within the last 0.5 m
  of its outline, unless a neighbouring segment or column continues the wall there (then the outline grows 0.3 m
  under the joint). The deepest answer of all providers wins (depth times share). Zero with no provider.
- **Producer: RockField3D** (`cliff_recess`, on by default). Kit boxes are built per 8 m plan square on first use
  from the unrecessed face (`relief_at(p, true, false)`), from world hashes only, and cached until the kit's
  settings or the generator change. The pillars each cell lays come from the same boxes, so pillars and recess
  always agree. The query is const and thread-safe (RWLock; squares are built outside the lock because they read
  the landscape's relief). A box stands at the face's mean plus up to 0.27 m; the face behind steps back by a third
  of the column's width (0.5 to 1.1 m), and every pillar's back stays at least 0.15 m behind that. No box where the
  face would step back more than 3 m or the pillar would bridge a gully deeper than 3 m.
- **Consumer: Landscape3D** (terrain thread). It moves the finest cliff vertices back by depth times share,
  includes the recess in its split test, fades its analytic relief by (1 - share), applies it in
  `relief_at(p, steps, recess = true)`, and rebuilds the finest chunks a `recess_changed` box overlaps. Collision
  is unchanged (heightfield tiles have no relief; pillars carry their own collision). With no provider its output is
  byte-identical.
- **Standalone tests.** Terrain alone: rocks bench `--no-rocks --recess-stub=x,y,z,nx,nz,w,h,d` installs a
  WorldGround script stub that recesses one box. Rocks alone: with no landscape, or `--no-cliff-recess`, the pillars
  stand just proud of the ridges as before. All together: the cliff-foot bench, raster pair and a cloud PT frame.
- **Cache.** `contract_recess_object()` is the field; its state is cliff_kit, cliff_recess, the cliff layer's
  on/density, cliff_block_max_slope and a rules version, bumped whenever `_kit_square` changes.
- **Second pass (26 Nov), from the first frames with the landscape's recess.**
  - Pillars are square joint-bounded prisms: four upright sides near square to the face, flat bed-joint tops, no
    taper or lean (the stack family's pointed tops read as spikes).
  - Each segment is set by the 3 m square nearest its root; before, a segment whose root fell between two squares'
    planes was dropped by both, and the wall showed gaps.
  - The skyline test read single grooves as the wall's edge and dropped half the pillars. Now each edge (top, foot,
    sides) holds where the face beyond it stands within 1.5 m of the step back at two of three points (one of three
    at the sides), and under the foot it must reach the step back itself (else the underside shows). The kit covers
    55% of the steep face in the cliff-foot view, up from 31% (`--recess-probe` in the rocks bench).
  - No pillar where the rock 2.5 m behind the step back is air: on a fin thinner than that the step back cut through
    it and the far side's pillars hung in the sky. None on a fin's nose either.
  - Pillars fill their segment's height (width up to 1.5 times the column's); scaled to the width, the segments of a
    column left gaps.
  - The contract gains `recess_vector_at(p) -> (offset xyz, share)`: the step back along the pillar's own normal.
    Along the drawn face's normal the flanks of a flute moved sideways and the ridges stayed in front of the pillars.
    Landscape3D uses it.


## Cliff kit in the world cache (26 Nov)
The kit's boxes are fixed by the world (hashes of positions, the unrecessed face), so they go in the world cache
(world_cache.h) like the obstacle cells: zones of 4 by 4 squares (32 m), keyed by the generator's result, every
relief provider's state (the drawn relief the kit fits to), `cliff_block_max_slope`, `cliff_recess` and the kit's
rules version. A zone not in the cache is built whole and stored. The relief and generator part of the key is worked
out on the main thread after each change (relief_changed, the generator's `changing`, a reset); a square asked for
while it is stale is built and not stored. `get_kit_stats()`: squares built and loaded, build time, a hash of every
box. Check (rocks bench cliff view): warm 17,456 squares loaded, none built, the same box hash; the cliff stage
13.1 s cold to 3.7 s warm (10.9 s of kit building gone); a warm raster still byte-identical to one with the kit
rebuilt.

## Frost-shattered block fields, built (26 Nov)
`block_fields` (on), in the scree layer's packing. Gate (`get_block_field_at`): periglacial cold (MAAT under -1 to
-3 C at the point's height) and a tundra summer (warmest month under 11 to 8 C: Koeppen ET, the tree line's 6 to
10 C) from the weather's climate; slopes under 20 to 26 degrees (Ballantyne 1998: blockfields only under about 25
degrees, steeper is talus); thin regolith, the complement of the frost sorting's soil gates (deep, fine soil sorts
into circles and stripes, thin or rocky ground stays blocks); no water, sand, sea or ice. Blocks 0.25 to 2 m, a power
law (2.2), angular (the scree family), 85% cover, little sunk (openwork: resting on each other), leaning to 25
degrees. On 4 to 20 degrees they gather in block streams, downslope stripes 8 m apart, long axes down the fall line
and dipping upslope (imbrication; Boelhouwers and Sumner 2003). The climate joins the obstacle cache's key. Bench:
`--world --uplift=0.5 --frost=-6 --view=scree --block-spot[=stream] --eye=1.7 --dist=10 --beyond=25`,
`--no-block-fields` for the before frame. Not yet: a second tier of blocks perched on the first, far cover maps
(the far tier's big blocks only), lichen-dark tops by age (the shared lichen terms apply as on other stones).

## Desert varnish, built (26 Nov)
`desert_varnish` (on). The amount follows the climate's precipitation at the field (with the terrain's rain pattern):
none over about 400 mm a year, full under about 180 mm (Dorn 2009: thick dark varnish in arid lands, patchy and thin
in semi-arid, washed off and outgrown by lichens where wetter), checked every 2 s. In the rock shader, on old surfaces
(surface_age, never fresh breaks): manganese-black on the exposed tops and in curtains down the runoff lines of steep
faces (the mesh's flow routing, COLOR.a, broken along their length), iron orange-brown on the undersides and near the
soil, a pale ground-line band, one satin sheen over the crystals. Lichens and moss go with it (they need frequent wetting; lichen
acids etch varnish). Pieces of the terrain's own wall (cliff blocks, pillars, ledges) take the wall's varnish instead:
one shared rule, `WorldContracts::cliff_varnish_shader_code()` (wc_cliff_varnish: patina film, manganese curtains from
lips 30 m apart, iron band over the foot from terrain_cliff_foot_map), which the terrain's cliffs draw by too, so a
piece and the face round it carry the same film and curtains (26 Nov). The climate rule is
WorldContracts::varnish_at (wc_varnish in GLSL). Bench:
`--rock=granite,glacial --varnish=1` (one stone), `--world --rain-climate=150 --view=tor --dist=7` and
`--no-varnish` for the world's before frame.

## Stream sinks and springs, built (26 Nov)
`karst` (on). RockField3D answers WorldCaves (terrain's contract, dd8e6009) from the generator's raw river network
(`get_rivers(area, false)`) and its own rock types (`_type_at`: limestone, or the soft strata when soft_rock_type is
limestone). A swallet where a stream first runs onto limestone after two points off it (an allogenic sink at the
contact; Ford and Williams 2007, ch. 5), capacity 0.6 to 1.8 times its mean flow (it sinks whole in the dry months,
floods run on down the blind valley), not on rivers over 20 m3/s, the shore or the map's edge. Its resurgence lower
down the same valley: where the river leaves the limestone (a contact spring), else 1.2 km on or at the river's end,
at least 8 m lower and 150 m on, on the valley wall 0.5 to 12 m above the bed (the water table), facing the channel;
base flow 0.02 to 0.08 m3/s; lag at 1.7 km a day (Worthington 1991). karst_changed on a new ground or a change of
rock types, seed or river area. The river layer's stones follow the raw network, so a blind valley keeps its
boulder bed. Bench: `--world --world-size=2048 --karst-report` (2 sinks, 2 springs lower, 2 rivers ending at a sink,
1 spring-fed), `--no-karst`. Not yet: a sink's swallow hole in the bed.

### Tufa at the springs (26 Nov)
Spring water loses its carbon dioxide as it leaves the rock and tumbles, so calcite comes out of it as tufa, laid
round mosses and algae (Pentecost 2005, "Travertine"; Ford and Pedley 1996; Arenas et al. 2014 on barrage tufa). Each
WorldCaves resurgence gets a mound of 7 to 11 lumps (0.45 to 0.95 m) round the back and sides of its mouth, set a fifth
of their height in their own deposit, and 3 to 6 low crescent dams (barrages) across the outfall below, convex
downstream, closer and smaller near the spring, wet. The lumps are their own family, RockGenerator::FAMILY_TUFA: a
cluster of 7 to 11 rounded lobes joined by a smooth minimum (botryoidal), a knobbly skin and holed by vugs where the
plants rotted out; cream-buff, porous, no beds or dark minerals, mossier than the rock round it. The cliff kit leaves
its pillars out within 2.5 m of a spring mouth, so the water comes out of an open face. Bench:
`--world --world-size=2048 --karst-report --spring-spot=2 --view=river` (per spring: stones by the mouth),
`--rock=limestone,tufa` (one lump), `--no-karst`.
