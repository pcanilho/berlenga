# Procedural trees, shrubs and deadwood

Research note, 10 Nov 2026, written before any code. Scope: every woody form from hazel and
hawthorn size up: trees, saplings, multi-stem shrubs, hedges, standing dead trees (snags),
fallen logs and broken branches. Everything below about 1.5 m (herbs, ferns, grasses, dwarf
shrubs, brambles) stays with `PlantGenerator` ([vegetation.md](vegetation.md), grass.md).
Placement, canopy shade and the weather canopy map already exist in `VegetationField3D` and
`PlantSpecies`; the trees made here replace the stand-ins placed there today.

The bar: dense forests that hold up at 100% crop under both raster and path tracing, move
realistically in the weather wind, take snow, and scale to an open world (hundreds of
thousands of trees, streamed).

## 1. How trees are modelled: the algorithm families

**Parametric, rule per branch level.** Weber and Penn, *Creation and Rendering of Realistic
Trees*, SIGGRAPH 1995: per level, length, taper, curvature, split rate, down angle, rotation
and gravity. Fast and controllable, used by Blender Sapling, Arbaro, EZ-Tree, gdTree3D and
SpeedTree's rule generators. Weakness: the crown is the sum of rules, not of competition, so
crowns look regular, interior branches never die, and a tree does not react to neighbours,
walls or light.

**Grammar based.** L-systems (Prusinkiewicz and Lindenmayer, *The Algorithmic Beauty of
Plants*, 1990), and positional information (Prusinkiewicz et al. 2001, which `PlantGenerator`
already follows for herbs). Ideal for small plants with regular phyllotaxis; for big trees
the grammar becomes the same hand-tuned rule set as Weber-Penn.

**Space colonization.** Runions, Lane and Prusinkiewicz, *Modeling Trees with a Space
Colonization Algorithm*, 2007. Attraction points fill a crown envelope; each point pulls the
nearest bud; points are consumed as branches reach them. Natural gaps and competition between
branches, shape set by the envelope. Weakness: the envelope is drawn, not grown.

**Self-organizing growth (the botanical standard).** Palubicki et al., *Self-organizing Tree
Models for Image Synthesis*, SIGGRAPH 2009. Buds compete for space and light (marker points
or a voxel shadow grid), resources flow down from the leaves and back up by the
Borchert-Honda model with an apical-dominance parameter, branches that stay in shade are shed.
Crowns, interior gaps, dead lower limbs, forest-grown versus open-grown shapes and lean
toward light all emerge. Follow-ups:
- Stava et al., *Inverse Procedural Modelling of Trees*, CGF 2014: a developmental model with
  about 24 biologically meaningful parameters (apical dominance, phototropism, gravitropism,
  shedding, branching angles) and a fit of those parameters to an example tree. Gives a species
  table we can fill from botany.
- Pirk et al., *Plastic Trees*, SIGGRAPH 2012: re-shape a finished tree to a new environment
  (wall, neighbour, light) without regrowing.
- Pirk et al., *Windy Trees*, ToG 2014: branch stress from wind (sigma = 4cM / 3 pi r^2,
  break when above d^3 p), and rest pose slowly following the long-term wind: wind-shaped
  crowns at coasts and tree lines.
- Makowski et al., *Synthetic Silviculture*, SIGGRAPH 2019: the same growth model drives both
  tree shape and the ecosystem, few parameters per species.
- Li et al., *Rhizomorph*, SIGGRAPH 2023 (vigor-based plant graphs, shoots and roots).

**Strand-based volumetric trees.** Li et al., *Interactive Invigoration: Volumetric Modeling
of Trees with Strands*, SIGGRAPH 2024: every leaf tip sends a strand down to the root; strands
are packed per cross section (PBD), and the bark surface is extracted from the packed strands.
Junctions, root flare, twisted and fluted trunks come out right without any junction code.
Cost: 3 to 13 s per tree for profiling plus 3 to 10 s meshing (RTX 3090). Li et al., *Stressful
Tree Modeling: Breaking Branches with Strands*, SIGGRAPH 2025: Cosserat-rod XPBD on the same
strands gives bending, breaking and splinters, 21 to 108 ms per tree per step: offline or
hero-only. No public code found for either.

**Commercial and production tools (bar setters, not usable).**
- SpeedTree 10: generators (trunk, branch, frond, leaf mesh, batched leaf, vine, decal) driven
  by rules, forces and "shade pruning"; 10.1 adds seasons. Games wind: shared sway, one or
  two independent branch levels (bend, oscillation, turbulence, flexibility, independence),
  leaf ripple, gusting with rise and fall, response times scaled by mass.
- Unreal 5.7 Procedural Vegetation Editor (experimental): a node graph over a skeleton
  (trunk, branch generators, gravity and phototropism bends, leaf placer), exports a Nanite
  static mesh or a skeletal assembly. Natsura (Houdini) does growth simulation upstream of it
  (Grow, Split, Repeat nodes, run by a Simulate solver) and exports Nanite assemblies.
- The Grove (Blender and Houdini): botanical growth with shade shedding, the closest
  commercial match to Palubicki.

**Godot ecosystem (checked).** EZ-Tree port and gdTree3D are Weber-Penn style with
vertex-shader sway. Cosmo Tree Gen (MIT, GDScript, WIP, 22 commits) makes low-poly trees with
no LOD, bones or wind; it stays parked. Zylann's tree generator plugin is old and unmaintained.
None meets the bar or reaches the path tracer with its wind.

## 2. Meshing: bark, junctions, leaves

- **Generalized cylinders** along each branch with parallel-transport frames are the norm
  (SpeedTree, Sapling, PVE). The weak spot is the junction: child tubes poke through the
  parent. Fixes, in rising quality: weld child into parent with a collar (SpeedTree), smooth
  convolution or implicit junctions (Bloomenthal 1985, Lluch et al. 2004), or strands (Li 2024).
- **Bark detail must be geometry where the camera is close.** Path tracing rejects normal-map
  only detail in silhouettes and contact shadows. Real bark ridges near the camera (displaced
  tessellation or displaced mesh LOD 0), normal maps farther. Procedural bark patterns: Lefebvre
  and Neyret, *Synthesizing Bark*, EGWR 2002 (fracture of an expanding bark layer); Dale et
  al., *Bark Fracture*, 2014. Moss and lichen by up-facing and wind-side wetness, which the
  climate maps already provide.
- **Leaves: real geometry near, alpha clusters far.** The Witcher 4 forest demo (Unreal Fest
  2025) uses no alpha-tested foliage at all: cards were 3 to 4 times slower in Nanite, and a
  fully modelled tree costs 63 MB against 6 MB for a carded one. Under path tracing alpha costs
  any-hit work on every ray: Indiana Jones went from 7.9 to 3.6 ms TraceMain with opacity
  micromaps (any-hit samples 17% to 3%). Our answer: modelled leaves (shared shape and texture
  baker from `PlantGenerator`) on near LODs, leaf clusters as cards with OMMs on mid and far
  LODs, which the fork already bakes.
- **Leaf optics:** thin-leaf transmission and back lighting are in the fork; leaf colour by
  season (SpeedTree 10.1 seasons) gives autumn, and bare winter crowns let snow sit on twigs.

## 3. Wind

- **Vertex-shader layers** (Sousa, GPU Gems 3 ch. 16; Horizon Zero Dawn: distance to trunk,
  distance to branch and per-branch phase baked in vertex colours; SpeedTree games wind; God of
  War's "boneless" sway on a 3D wind field). Cheap, but invisible to the path tracer: shadows
  and reflections would show a still tree.
- **Physically guided hierarchy.** Habel, Kusternig and Wimmer, EG 2009: each branch a tapered
  Euler-Bernoulli beam, deflection u(x) = c2 x^2 + c4 x^4, a damped oscillator per branch with
  resonance f = 2.55 L^-0.59 Hz (broadleaf; about 2.5 times higher leafless) driven by a Kaimal
  style wind spectrum through precomputed noise textures, one per hierarchy level; parents
  deform children. 0.1 ms for 1,500 branches on 2009 hardware. Diener et al. 2009 (modal
  wind projection basis) is the alternative per-branch response.
- **Bones, evaluated on the GPU (current state of the art).** Unreal 5.7 Dynamic Wind moves
  bones, not vertices. The Witcher 4 forest: 500,000 trees, 28 species, about 135,000 animated
  bones in view, one compute thread per bone accumulating to the root in about 0.1 to 0.3 ms
  (under 20 us on async compute); 150 unique skeletons with 8 orientation variants instead of
  one skeleton per tree; rigid skinning (one bone per part). Vertex wind was rejected because
  it inflates Nanite cluster bounds, the same reason it breaks BLAS bounds for us.
- **Breaking and permanent bend:** Pirk 2014 stress; PhysX for snapped branches and felled
  trees.

The fork already has the delivery path: `MultiMesh.bones_per_instance`
([multimesh-instance-bones.md](multimesh-instance-bones.md)) skins instanced trees from
per-instance bone records, raster and path tracer match (mean pixel difference 0.013), and a
compute pass can write the records (gpu-multimesh-records.md). What is missing is the solver.

## 4. Far trees and open-world scale

- **Billboards and impostors** (Horizon: top LODs are cross planes; God of War: card clusters
  and billboard clouds; octahedral impostors in many engines) fail under path tracing: they show
  as flat cards in shadows and reflections.
- **Stochastic pruning.** Cook, Halstead, Planck and Ryu, *Stochastic Simplification of
  Aggregate Detail*, SIGGRAPH 2007 (Pixar): at distance, drop a random subset of leaves and
  enlarge and darken the rest so area and contrast are kept. Appearance-preserving and ray
  traceable, so it suits a path tracer.
- **Clustered LOD plus OMM, partitioned TLAS.** van Antwerpen, Gautron, Biddulph, Kubisch,
  Schmid and Stich, *Real-time Path Tracing of Massive Dynamic Foliage*, ACM 2026: partitioned
  TLAS, automatic clustered LOD, OMMs replacing twigs, needles and leaf clusters at distance,
  per-plant animation with screen-space-bounded error, tens of millions of plants. RTX Mega
  Geometry and OMMs are what The Witcher 4's ray-traced forest uses. Our route: mesh LODs per
  tree (twig pruning, fewer sides, leaves to clusters) shared through the RT LOD and persistent
  slot work ([open-world-geometry.md](open-world-geometry.md), [instanced-forests.md](instanced-forests.md)),
  Mega Geometry later on the PC.
- **Voxels** (Nanite Foliage, 4x4x4 bricks for sub-pixel clusters) and prefiltered volumes
  (Loubet and Neyret, *Hybrid Mesh-Volume LoDs*, EG 2017) are the far-far answer; a later
  stage, and it overlaps with the voxel stand-ins already noted for vegetation.

## 5. Shrubs, hedges, the in-between forms and deadwood

- **Shrubs** are the same growth model with basitonic dominance: many stems from the base,
  each short-lived and replaced (hazel, hawthorn, elder, willow scrub, juniper, dwarf pine
  krummholz at the tree line). In Stava's parameters this is apical dominance low plus basal
  bud activation.
- **Saplings and age classes** come free from growth: stop the simulation earlier. One species
  gives seedling, sapling, pole and mature variants, which forest edges and clearings need.
- **Hedges** are shrubs grown inside a clipped envelope (space colonization markers only
  inside the hedge volume, pruning of buds outside it), like Palubicki's topiary examples.
- **Deadwood.** Standing dead trees and logs follow the five decay classes used in forestry
  (Maser et al. 1979; Mellen and Ager 2002): class 1 twigs and bark intact, 2 twigs lost, 3
  bark sloughing and top broken, 4 only stubs and a soft stem, 5 a fragmented stump. Each is a
  post-process of a grown tree (drop leaves, then twigs, then bark, break the top), so snags
  and logs match the living species around them. Shade-shed branches from the growth model
  give dead lower limbs on living conifers. Fallen logs and broken branches become PhysX
  bodies; stumps and logs feed the snow and wetness shaders like any other mesh.

## 6. What the games at Paulo's bar do

- **Kingdom Come: Deliverance 2** (CryEngine): Warhorse has published no vegetation talk; what
  is known is CryEngine's toolset (painted and rule-placed vegetation, layered vertex bending,
  touch bending of physicalized branches, mesh LODs then billboards). From the game itself, the
  look comes from species variety and botanically right forest structure (age mix, deadwood,
  undergrowth, forest edges), not from a special renderer.
- **Crysis** (CryEngine 2): Sousa's layered vertex bending, physicalized breakable trees and
  palms that react to explosions, vehicles and the player.
- **Star Citizen, Planet Tech v5 (Nyx and later)**: rule-based GPU scattering with competition
  for water, nutrients and light; distant forests from ground-cover data; touch bending was in
  R and D. Its strength is the ecosystem layout at planet scale, which our placement already
  follows.
- **Horizon Zero Dawn / Forbidden West**: SpeedTree plus Houdini, skinning approximated in
  vertex colours, 3D wind textures in camera cascades, geometry grass, depth-prepass for
  alpha, separate shadow meshes.
- **The Witcher 4 (UE 5 demo)**: the scale reference above: modelled leaves, bones on GPU,
  voxels far, Mega Geometry for ray tracing.

## 7. Proposed design

One generator, three stages per tree, everything deterministic from (species, seed, age).

1. **Growth (`TreeSpecies` resource, `TreeGenerator`).** Self-organizing growth after
   Palubicki 2009 with Stava 2014's parameter set: buds, a voxel shadow grid for light, the
   Borchert-Honda resource model, gravitropism, phototropism, shedding. Optional environment:
   neighbours (forest-grown versus open-grown), terrain slope (lean), prevailing wind (Pirk
   2014 rest-pose drift: flagged crowns at coasts and tree lines), a clip envelope (hedges).
   Output: a branch graph with radii (pipe model, da Vinci's rule), age and vigor.
2. **Meshing.** Generalized cylinders with welded collars at junctions and a flared root base
   (strands from Li 2024 later, as an upgrade for hero trees if the collar looks short of the
   bar); bark UVs continuous along the stem; procedural bark textures and real displaced bark
   on LOD 0; leaves as modelled geometry from the `PlantGenerator` leaf baker, clusters with
   OMMs on far LODs; season and snow parameters on the materials (snow_enabled on crowns).
   LODs: twig pruning plus stochastic pruning of leaves (Cook 2007), fewer radial sides, then a
   coarse mesh with cluster cards; never a flat billboard.
3. **Rig and wind.** A skeleton of trunk plus one or two branch levels (Witcher 4 style rigid
   parts, about 20 to 60 bones a tree), per-bone Habel 2009 beam response to `WeatherMap3D`
   wind (mean, gusts, canopy sheltering), solved in one compute pass that writes
   `bones_per_instance` records; a few dozen unique skeletons per species with orientation
   variants share solves. Leaves flutter through the twig bones, so the path tracer sees it.
   Far trees hold their pose (scheduler).

Plug-in points, agreed with the vegetation side: each tree species is a `PlantSpecies` whose
meshes are generated variants, listed first in the Biome; `canopy_radius` and
`canopy_opacity` come from the grown crown (crown mesh origin at ground level, since
`canopy_top` is read from mesh 0's AABB); climate ranges give altitude belts and tree lines;
trees carry no colliders on `SnowCover3D.ground_collision_mask`. Generated variants are cached
as resources at edit time (binaries to Gitea LFS) or rebuilt deterministically on load.

## 7a. Collision and touch

Asked for on 10 Nov: vegetation should react to bodies, not only grass.

How others do it:
- **CryEngine (Crysis, Kingdom Come).** Trunks carry simple physics proxies (cylinders or
  capsules) and stop characters and vehicles. Bushes and low branches use "touch bending":
  artist-placed chains of branch helpers become physicalized ropes that living entities and
  vehicles push aside, and that spring back by their stiffness and damping. Breakable trees
  split at authored joints when an impact exceeds their strength, and the pieces become rigid
  bodies (CryEngine docs: Touch Bending; Vegetation 04 Bushes; Vegetation 05 Trees, Breakable).
- **Horizon Zero Dawn.** Canopies have no collision, trunks are cylinders, and canopies react
  to force fields (the player, machines, explosions) written into the same wind textures as the
  weather (Sanders, GDC 2018).
- **God of War.** Wind "motors" on characters and effects add to a 3D wind field that bushes
  and trees receive (Feeley, GDC 2019).
- **Unreal 5.7 / The Witcher 4 demo.** Trees are bones; wind and interaction are transforms
  written to the bones, so anything that moves bones (a field, a contact) moves the tree.

Our design follows the same split and reuses what the fork has:
1. **Trunk and limb colliders.** From the branch graph: capsules for the trunk and every limb
   thicker than a threshold (about 8 cm), one static compound per tree variant, created through
   the physics server per instance only inside the physics radius and streamed with the tiles.
   They use their own collision layer, never `SnowCover3D.ground_collision_mask`.
2. **Touch bending.** The bone wind solver (section 3) takes contact as an extra force: bodies
   stamp position and velocity into the trample field the grass already reads, and each bone
   chain near a body is pushed out of a few collision spheres, as the hair solver does
   (`PhysXHair3D`). The same beam stiffness that drives the wind response springs the branch
   back. Shrubs, saplings and low branches react; trunks do not.
3. **Breaking.** Pirk 2014's stress test on a contact impulse snaps a branch or a sapling: its
   bones are frozen and the part becomes a PhysX rigid body (Blast for trunks later). Felled
   trees and logs are PhysX bodies with the same capsules.

## 8. Stages

1. Growth model and species table (oak, beech, birch, Scots pine, Norway spruce, hazel,
   hawthorn), with a bench that grows each species open-grown and forest-grown, and timings.
2. Meshing with junction collars, bark and leaves, three LODs, raster and path traced 100%
   crops against the stand-ins.
3. Skeleton and GPU wind solver over instance bones, with the weather wind.
4. Shrubs, saplings, hedges, and deadwood decay classes plus PhysX logs; trunk and limb
   colliders, touch bending through the bone solver, breaking.
5. Hook into `VegetationField3D` (real trees replace stand-ins, canopy from the grown crown)
   and the showcase forest.
6. Scale on the PC: RT LOD sharing, OMM cost, partitioned TLAS, bones-in-view budget.

Needs the PC (no GPU or denoiser in the cloud): GPU timings of the wind pass and of leaf
geometry versus OMM cards, denoised path traced crops at 100%, and the forest-scale bench.

## 8a. Species, habitats and stands (as built)

Seventeen presets: oak, beech, birch, Scots pine, Norway spruce, hazel, hawthorn, small-leaved
lime, sycamore, ash, horse chestnut, black alder, white willow, larch, silver fir, rowan, elder.

**Habitat.** Each preset has a climate envelope from the species range maps and climate profiles
of the European Atlas of Forest Tree Species (San-Miguel-Ayanz et al. 2016): annual and January
mean temperature, the dry end of precipitation and the aridity index, slope, soil depth, and for
alder and willow the distance to water. `apply_habitat()` writes them as `PlantSpecies` rules
with soft edges (1.5 C, 3 C, 150 mm). Only the dry end of rainfall limits: the wet ends of the
range maps are cold or oceanic climates the temperature rules already cover (the first version
capped rain and emptied a wet mountainside).

**Stand-grown and open-grown.** The stand-grown form grows with `forest_shade` among neighbours
at 1.3 times the stand spacing (the mean nearest-neighbour distance of a Poisson-disk pattern),
with side shade past half that distance at every height up to the tree top: Palubicki's trees
grown in one grid meet at a shared boundary, and the stand form now does too (crown radius about
0.65 of the spacing, so the canopy closes). The open-grown form keeps to light gaps and edges
through a shade rule (Ellenberg light value). Crown radius is the drip line, the 95th percentile
of foliage reach, not the longest twig.

**Stands** (Ellenberg 1988; EUNIS G1-G3 and FA): beech (with sycamore and ash in complementary
patches); oak-lime with ash in moist hollows and a hazel coppice layer; montane spruce-fir-beech
(1.5 to 6 C annual mean); subalpine larch-spruce up to the tree line (-1.5 C); pine heath
(under about 750 mm); riparian alder, willow and elder; hedgerow (hawthorn, hazel, elder, rowan
with oak, lime and horse chestnut standards, placed by the scene along its lines). In each:
canopy trees, then open-grown trees in gaps, then shrubs under their shade limits.
`make_woodland()` joins the woodland stands with every canopy tree first; later stands' trees
take only gaps earlier crowns leave, so overlapping belts form ecotones instead of stacking.
Bench: `docs/bench/trees -- --stands --test | --map= | --region= | --plot=N`.

Known limits: open-grown conifers spread too wide at the base (spruce about 45 m across where a
solitary one is 10 to 14 m); a lateral-extension factor (Wilson 2000) made crowns collapse in
the shadow model, so this needs its own pass. Generated mountainsides from TerrainGenerator3D
are bare rock at 40 to 50 degrees with no soil, so no tree grows on them; the region bench uses
a smooth massif until that changes.

## References

- Weber, Penn, SIGGRAPH 1995. Prusinkiewicz, Lindenmayer, *The Algorithmic Beauty of Plants*,
  1990. Runions, Lane, Prusinkiewicz, 2007. Palubicki et al., SIGGRAPH 2009. Stava et al., CGF
  33, 2014. Pirk et al., SIGGRAPH 2012 and ToG 33, 2014. Makowski et al., SIGGRAPH 2019. Li et
  al., SIGGRAPH 2023, 2024 (ToG 43(4)) and 2025.
- Habel, Kusternig, Wimmer, *Physically Guided Animation of Trees*, EG 2009. Diener et al.,
  EG 2009. Sousa, GPU Gems 3 ch. 16, 2007.
- Cook, Halstead, Planck, Ryu, SIGGRAPH 2007. van Antwerpen et al., *Real-time Path Tracing
  of Massive Dynamic Foliage*, ACM 2026 (doi 10.1145/3820021). Loubet, Neyret, EG 2017.
- Lefebvre, Neyret, EGWR 2002. Bloomenthal, *Modeling the Mighty Maple*, SIGGRAPH 1985. Lluch
  et al., 2004.
- Maser et al. 1979 and Mellen, Ager 2002 (decay classes).
- NVIDIA, *Path Tracing Optimizations in Indiana Jones*, 2025. Intel, *Jungle Ruins*, 2025.
  Unreal Fest 2025, *Witcher 4 large animated foliage*. Epic forum thread 2685424.
  SpeedTree Modeler docs (wind games, what's new 10.0 and 10.1). Natsura vs Unreal, 2025.
  Sanders, *Between Tech and Art: The Vegetation of Horizon Zero Dawn*, GDC 2018. Feeley,
  *Interactive Wind and Vegetation in God of War*, GDC 2019.
- Code read: Cosmo Tree Gen, EZ-Tree, gdTree3D (MIT).
- San-Miguel-Ayanz, de Rigo, Caudullo, Houston Durrant, Mauri (eds.), *European Atlas of Forest
  Tree Species*, Publications Office of the EU, 2016. Ellenberg, *Vegetation Ecology of Central
  Europe*, 1988. Ozenda, *La vegetation de la chaine alpine*, 1985. Korner, *Alpine Treelines*,
  2012. Wilson, *Apical control of branch growth and angle in woody plants*, Am. J. Bot. 2000.
  Kantola and Makela, *Crown development in Norway spruce*, Trees 2004.
