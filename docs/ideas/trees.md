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

### 2a. Bark (stage 6 plan)

What the field does:
- **Fracture of an expanding outer layer.** Bark patterns come from the dead outer bark cracking
  as the cambium widens the stem: Lefebvre and Neyret, *Synthesizing Bark* (EGWR 2002) grow
  cracks along the stem as circumference grows; Wang et al., *Interactive Modeling of Tree Bark*
  (PG 2003) synthesise from samples onto the stem; Kratt et al., *Woodification* (CGF 2015) tie
  bark thickness and cracking to cambial growth; Pixar's *Elemental Bark* layers a procedural
  fracture over shaded wood. All share one rule we can use directly: crack spacing and depth
  follow how much the stem has grown since that bark formed.
- **Games.** SpeedTree tiles a scanned bark per species with UVs continuous along the stem and a
  fixed number of repeats around each branch, plus tessellated displacement near the camera; the
  Witcher 4 forest (UE 5.6, Nanite foliage) models trunks as real geometry and displaces them.
  Normal maps alone go flat in silhouettes and under path tracing (section 2).

Our plan (assets stay out of git: everything is procedural):
1. **Bark baker.** One tileable texture set per species (albedo, normal, roughness, height; 512
   px over 0.4 m), baked once at generation and cached like the leaf textures. Families:
   smooth (beech, hornbeam, young birch, rowan: grey, faint lenticel dashes), furrowed (oak,
   ash, lime, chestnut, elm, willow: anisotropic Voronoi cracks stretched 4-8x along the stem,
   interlaced ridges), plated (Scots and white pine: wide plates, deep furrows, orange upper
   stem), scaly (spruce, larch, hemlock: small flakes), flaking (sycamore, maples: patch
   mosaic of colour), papery (birch: white with horizontal lenticels, black diamond fissures
   low on the stem). Crack depth and width scale with stem age at that height (Lefebvre-Neyret),
   so the base of an old oak is deeply furrowed and its limbs nearly smooth.
2. **UVs.** u around the stem in a whole number of repeats chosen from each branch's base
   circumference, v along the branch as arc length from the root, continuous through junction
   collars, so the texel density stays even and nothing stretches.
3. **Real relief near the camera.** The near variant (the `near_meshes` swap the needles use)
   rebuilds stems and limbs over 8 cm with 32 or more sides and 4 cm rings, displaced by the
   baked height times the species' bark depth (oak and pine up to 3-4 cm, beech under 1 mm).
   Raster and the path tracer both see the ridges. Mid and far LODs keep the texture with its
   normal map.
4. Later: moss and lichen on up-facing and wet sides from the climate maps; decayed classes
   peel to bare wood with the same baker.

Checks: 100% crops raster against PT (detail kept), texel density along a stem, no seams around
or along, cost of the near trunks (vertices, BLAS memory).

### 2b. Species leaves (11 Nov; Paulo picked)

Leaf architecture by species (Ellis et al., *Manual of Leaf Architecture*, 2009): simple
(beech, birch, lime, alder, willow, holly), pinnately lobed (oaks: rounded lobes in white and
pedunculate oak, bristle-tipped in red oak; hawthorn deeply lobed), palmately lobed (sycamore and
maples), pinnately compound (ash, rowan, elder) and palmately compound (horse chestnut).
Silhouette is geometry, never alpha: the path tracer sees the true outline and needs no any-hit.
- Blade: the current folded lens with a width profile per form; lobes sampled at peak and sinus
  (two rows per lobe); palmate leaves as lobes fanned from the petiole; compound leaves as
  leaflets along a rachis or from one point.
- LOD: the detailed blade is LOD 0 (and the path tracer); coarser levels keep today's simple
  lens of the same area as a stored twin, so far LODs cost what they do now.
- Texture: venation baked per form (Runions et al., *Modeling and Visualization of Leaf
  Venation Patterns*, SIGGRAPH 2005, for the look; drawn here as a midrib, secondaries at the
  species' angle and a margin band), multiplying the vertex colour; a weak normal map for vein
  relief.
- The leaf form and colour per species are exposed for the ground plants' leaf litter.

As built (11 Nov): `TreeGenerator.get_leaf_form(preset)` returns `{kind, count, lobes, depth,
width, size, color}` (kind is `LEAF_SIMPLE/LOBED/PALMATE/PINNATE/DIGITATE/NEEDLE`); stand species
carry meta `leaf_preset`. Pointed lobes take two rows a lobe, rounded ones (pedunculate and white
oak) three on an obovate blade with a terminal lobe. A compound leaf fills several leaf slots
(count / 3) with the same total area, so crown density, drag and mass do not change. Horse
chestnut leaflets are obovate. Venation is one shared 128 x 256 texture (midrib, eight
secondaries, margin band), albedo lifted 1.13 so the average colour stays the species'. No
normal map yet. LOD 0 leaf triangles in a stand (forest_shade 0.6, seed 1), lens before:
oak 209k to 975k, sycamore 86k to 287k, ash 423k to 846k, horse chestnut 92k to 322k, rowan
294k to 638k, red oak 80k to 239k. LOD 1 and coarser are unchanged (the twins). Whether an
alpha-cut lens with an opacity micromap beats this under the path tracer is on the PC queue.

### 2c. Seasons (as built, 11 Nov; Paulo picked)

One season state for every system: WeatherMap3D.get_season() (water/weather owns it), dated by
SkyClock and built from ClimateRecipe's climatology plus the live temperature anomaly, so any
date is right at once. Keys read here: day_of_year, latitude_deg, photoperiod_h (Forsythe et
al. 1995), forcing (degree days above 5 C from 1 Jan), chilling (days of 0-5 C from 1 Nov),
senescence (Delpierre et al. 2009: from the first day under 13.2 h after midsummer, the sum of
max(0, 20 - T) (P / 13.2)^2).
- Budburst: thermal time (Cannell and Smith 1983), the requirement falling with chilling as in
  Murray, Cannell and Smith 1989; leaves then expand over 150 degree days, small and pale at
  first. Species order follows European observations (Vitasse et al. 2009; PEP725): elder,
  hazel, birch and horse chestnut early, beech and oak late, ash last. With the bench's
  stand-in climate (50 N, 0 C January, 18 C July) budburst runs from day 107 (elder) to 126
  (ash).
- Autumn: each species colours from a senescence onset over a span, and drops leaves after a
  lag (ash and alder drop nearly green, oaks hold theirs into December). Each leaf has a fixed
  place in the order, so the first to turn is the first to fall. Colours: carotenoid yellows
  (birch, lime, larch), anthocyanin reds (red maple, rowan, red oak), tannin browns (oaks,
  beech copper).
- `TreeGenerator.season` (the dictionary) leaves out the leaves not grown or fallen, so wind
  drag and mass follow the season; far_color follows the turned share and the bare crown.
  Evergreens never change; larch turns gold and goes bare.
- `TreeGenerator.get_leaf_season(preset, season)` gives leaf_out, autumn, fallen_fraction and
  autumn_color for vegetation's leaf litter (fallen_fraction stays 1 from the last leaf fall
  until budburst).
- Changing the date regenerates the meshes (stands cache by season). A per-instance shader
  path, without regeneration, is a later step if a game day has to pass in view.
- Bench: `--day=N` renders and tests at a day of year with WeatherMap3D.get_season() (on main
  since #252; it matches the bench's stand-in season_for() within 5%); `--test` checks the leaf-out order, red
  maple before oak, birch bare by late November, pine unchanged and bare January crowns.

### 2d. Welded junctions (as built, 11 Nov; Paulo picked)

A limb's tube used to start on its parent's axis, a pipe pushed into a pipe: from close by
the cut showed as a hard ring, and on smooth bark (beech) the pipe's end poked out. Now
(`junction_weld`, on by default) every vertex of the limb inside its parent slides along the
limb's axis onto the parent's bark: SpeedTree's weld. The contact curve is the intersection of
the two generalized cylinders, as in Bloomenthal 1985 and Lluch et al. 2004, without their
implicit surfaces, so it adds no triangles.
- The parent is modelled as up to three tapered segments (the internodes before, at and after
  the junction), so a weld never lands on bark that is not there where the parent's axis bends.
  If any vertex would leave through an open end, the limb is not welded at all.
- The slide works on the undisplaced tube and moves the displaced vertex by the same amount,
  so bark relief keeps its shape at the seam. v grows with the slide, so the bark keeps its
  texel density over the long footprint of a steep limb.
- Normals blend to the parent's over about 0.8 limb radius, so the seam shades as one surface.
  On rigged trees the seam takes the parent's bone with the same weight, so wind does not open
  it.
- Left alone: near-parallel limbs (under about 12 degrees) and near-equal forks (limb over 0.6
  of the parent's radius). These are crotches with bark inclusion, not collars, and need
  a crotch model of their own.
- Seen while testing: some limbs passed through a neighbouring stem or limb. Growth keeps buds
  apart through the shadow grid, not the wood they become, and the radii and the sag under
  gravity come after growth. Fixed (12 Nov) the way trees settle it: two branches that touch
  rub, the bark wears through and the thinner one dies back to the contact (branch abrasion;
  crown shyness, Putz et al. 1984, Fish et al. 2006). After the sag, wood over 1.2 cm is tested
  against thicker wood that is not its close kin, as capsules in a 0.75 m grid. The thinner
  limb is cut just short of the contact and everything beyond it goes; stems are never cut.
  An open oak loses 82 of 65,000 internodes this way, a forest sycamore 186 of 32,000. Crowns
  look the same at a distance and generation time does not change. `get_collision_prune_count()`.

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

### 3b. Leaf flutter (as built, 11 Nov; Paulo picked)

Bones carry the branches; each leaf adds its own flutter in the leaf shader (`leaf_flutter`,
default 1). The material is BaseMaterial3D's generated shader for the leaf surface, so skinning
from instance bones, snow, wetness and backlight stay as they were, with Sousa's detail
bending added at the top of vertex() (*Vegetation Procedural Animation and Shading in Crysis*,
GPU Gems 3, ch. 16). The leaf bends along its normal with the square of the distance from the
petiole (UV.y), and twists about its midrib (UV.x - 0.5), which is the motion SpeedTree's leaf
ripple and Unreal's Pivot Painter show. Each leaf runs at 5-9 Hz plus a little more in strong
wind, its phase taken from its vertex colour (the same on all of a leaf's vertices at every
LOD). The amount follows the ground plants' wind (`plant_wind`: the weather map's wind, or the
fallback), so trees, plants and grass share one wind. It is zero in calm air and full from
about 9 m/s, and moves a leaf's tip by millimetres and its edges by about a centimetre.
- Path tracer: the leaves keep their rest shape and follow the bones. Static trees are traced
  from the rest mesh (`rt_vertex_static`). Rigged trees are captured when their bones move
  (`rt_vertex_frozen`), as with the plain material, never because TIME moved: recapturing a
  20-million-triangle open-grown oak crown every frame ran the cloud out of memory.
- At rest the material is pixel-identical to the plain one in raster (mean difference 0.000002)
  and in the path tracer.
- Climber leaves flutter too; needles do not.

### 3c. Wind cost budget (as built, 12 Nov; Paulo picked)

The wind solve is closed-form in time, so a pose can be solved at any frame without history,
which makes rate and bone LOD cheap to add.
- **Significance per tree**: the scheme of Unreal's Animation Budget Allocator and its update
  rate optimisation (tick rate from screen size, staggered across frames), with SpeedTree's
  wind quality per LOD. A tree's crown tip moves about 4% of its height per second at 8 m/s
  wind (0.2-0.6 Hz sway, Moore and Maguire 2004). From that and the tree's pixels per metre, the
  pose is held for as many frames (1-8) as keep its lag under `lod_pixel_error` (0.5 px).
  Instances take a hashed frame slot so the held ones spread evenly.
- **Bone LOD**: a bone whose length plus foliage reach is under the error over 0.15 rad of bend
  keeps only the lean the mean wind gives it (no gusts or resonance, the costly part of the
  solve). Until 16 Nov such a bone rode its parent unbent and lost its whole lean, which the
  0.15 rad bound did not count: the stand bench measured 1.36 px p99 against its 1 px bar, with
  the held poses alone at 0.13 px. Now 0.46 px p99 (0.94 max), 0.86 ms a frame against 7.3 ms
  for the full solve (it was 0.66 ms).
- **Out of view**: a tree whose bounding sphere, grown by 2.5 heights for its shadow, is
  outside the frustum is solved every 8th frame. Reflections and a camera turn still find it
  near its pose.
- **Budget**: with `budget_msec` above 0, the error grows (up to 8x) while the solve runs over
  and relaxes back below 70%. It is off by default, so the error bound holds.
- **Math**: the gust transfer function's lag came from atan2 per wave per bone; cos(phase - lag)
  now uses re and im directly. The result is the same and the full solve is about 30% faster.

- **Range** (12 Nov; the far-wind card, approved with "address all open issues"):
  `wind_range` is 1000 m (was 200). Past 200 m trees stood still while near ones moved in the
  same wind. Far out, the same error bound lets a tree hold its pose up to 32 frames (8 before)
  and only its big limbs are posed, so a far tree costs a small fraction of a near one.

Bench `--stands --wind-lod [--view=over]`, 615 rigged beech in the 160 m stand, 1080p, fov 60.
Two solvers run on the same frames, one with LOD and one without, and the bone pose difference
is projected to pixels at each tree's distance:

| view | full solve | with LOD | trees solved | error p99 / max |
|---|---|---|---|---|
| eye height, at the stand edge | 5.1-7.6 ms | 4.3-5.2 ms | 382 of 615 | 0.47 / 0.75 px |
| over the stand from 85 m out | 5.1 ms | 2.7 ms | 152 of 615 | 0.55 / 0.93 px |

The times are wall time on the 4-core cloud machine. Of the 2.7 ms, the worker solve is about
1.1 ms (4.3 ms without LOD). The rest is the MultiMesh buffer upload (0.8-1.5 ms here, on a
software Vulkan). The PC should time the upload: if it is real there, cells that changed only a
few instances want a region update. `--stands --test` checks that the LOD is faster and its p99
error stays under 1 px. Trees past `wind_range` (200 m) still hold their rest pose. With the
LOD they cost about one eighth, so a longer range is now cheap, but that is a separate look
change.

### 3a. The solver we build (stage 3)

Wind on a tree is a structure loaded by turbulence, the problem wind engineers solve for
towers and bridges in the frequency domain: the load spectrum times the structure's transfer
function (Davenport, *The application of statistical concepts to the wind loading of
structures*, Proc. ICE 19, 1961). Shinya and Fournier (*Stochastic motion: motion under the
influence of wind*, Eurographics 1992) and Stam (*Stochastic dynamics: simulating the effects of
turbulence on flexible structures*, Eurographics 1997) brought it to graphics: each mode of a
plant is a damped oscillator, and its motion is the turbulence spectrum filtered by that
oscillator. de Langre (*Effects of wind on plants*, Annu. Rev. Fluid Mech. 40, 2008) reviews the
mechanics this rests on.

`WeatherMap3D` already synthesises its gusts as Kaimal-weighted plane waves under Taylor's frozen
turbulence: each wave has a wave vector and so a frequency, omega = k U. A linear oscillator's
steady response to one wave is exact: the wave scaled by |H(omega)| and delayed by its phase,
H = 1 / (1 - (omega/omega0)^2 + 2 i zeta omega/omega0). So every bone's motion is a closed-form
sum over the same waves the grass and the clouds see. No state, no time step: it is
deterministic, it never blows up at a frame hitch, any instance can be solved on any thread or
on the GPU, raster and the path tracer read one pose, and the whole forest stays coherent (a gust
front visibly rolls through the stand because the waves' phases depend on position).

1. **Rig, from the grown branch graph.** Rigid parts, as in The Witcher 4: trunk (and each
   basal stem) split into up to four segments with linear blending across the joints; then
   the thickest limbs, whose parent is already a bone, down to about 2 cm radius; at most 48
   bones a tree, one root bone. Every internode, twig, leaf and spray takes the bone of the
   nearest boned axis below it.
2. **Bone mechanics.** Each bone is a cantilever spring at its base. Rotational stiffness
   k = 3 E I / L with I = pi r^4 / 4 at the bone's base (green-wood modulus E about 8.5 GPa;
   Forest Products Laboratory, *Wood Handbook*, 2010). Inertia J sums the wood (green density
   about 900 kg/m3) and foliage (about 0.2 kg per m2 of leaf, fresh) of everything the bone
   carries about its pivot, so omega0 = sqrt(k / J). Checked against measured trees: sway
   frequencies of 0.15-0.8 Hz for mature trunks, rising with diameter over height squared
   (Moore and Maguire, *Natural sway frequencies and damping ratios of trees*, Trees 18, 2004),
   and several Hz for limbs. Damping ratio about 0.1 for trunks with foliage, 0.15 for limbs
   (same review; most of it is aerodynamic).
3. **Load.** Drag on the bone's subtree, F = 1/2 rho Cd A |U| U, applied at the centroid of
   that drag area, with A the frontal area of wood and foliage the bone carries. Foliage
   streamlines as the wind rises, so Cd falls as U^B above about 4 m/s, Vogel's exponent
   B of about -0.7 for broad leaves and -0.5 for needles (Vogel, *Drag and reconfiguration of
   broad leaves in high winds*, J. Exp. Bot. 40, 1989). The quadratic drag is linearised about
   the mean wind at the bone (the quasi-steady gust load of Davenport 1961): a static bend from
   the mean, and a gust part that passes through the oscillator.
4. **Turbulence: background plus resonance.** Davenport's gust loading splits a structure's
   response in two, and so does the solver. The background part is the weather's own waves
   (8 log-spaced Kaimal bands per axis, up to a few Hz at moderate wind), each passed through
   the bone's quasi-static gain, so a gust front sways the whole stand together. The resonant
   part is what a lightly damped oscillator does with the broadband load near its own
   frequency: a narrow-band sway of variance S_F(f0) pi f0 / (4 zeta k^2), with S_F the Kaimal
   load spectrum at f0. It is drawn as two waves at f0 (1 +- zeta) with phases hashed per bone
   and per tree, which gives the beating envelope such a response has, along and across the
   wind (across-wind sway of trees is of the same order; de Langre 2008). Mean wind is the
   weather's at the bone's height (log law, canopy and lee shelter, gust patches).
5. **Pose.** Each bone turns about its pivot by theta = (c x F) / k (the moment over the
   stiffness), clamped to 0.5 rad, and carries its children. Records are written into a
   MultiMesh with `bones_per_instance` (placement times the bone chain), so raster and the path
   tracer skin the same pose. Trees past `wind_range` keep their rest pose.

In a forest, one TreeWind3D aims at the VegetationField3D: the field gives a rigged mesh's cells
rest bones (`TreeWind3D.fill_rest`), near and far cells alike, and the solver poses every cell it
finds (a test beech stand: 78 cells, 625 trees, 7.6 ms on four CPU threads, so the compute
version matters at forest scale). The path tracer drops per-instance COLOR and INSTANCE_CUSTOM
on MultiMeshes with more than 12 bones per instance; the tree material reads neither, so nothing
visible differs, but no tree material may use them for tint, season or phase without lifting
that limit first.

The CPU solver runs over worker threads and is the reference; a compute version of the same sum
is the step for forest scale on the PC (one thread per bone, as in The Witcher 4). The colliders
and touch bending of section 7a add a contact moment to the same bones.

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

### 4a. Measured (11 Nov)

- **Compressed vertices** (`compress_vertices`, on): Godot's compressed format (positions as
  16-bit fractions of the mesh box, octahedral normal and tangent, 16-bit UVs). Vertex memory
  of the stand-grown oak, beech and spruce falls to 67% (80% rigged, where the skin weights
  dominate). Raster at 100% crop: indistinguishable (mean 0.48/255, sub-pixel edge shifts);
  path traced: same mean, differences at noise level. The path tracer reads the format.
- **Shadow meshes**: no gain. The renderer uses an ArrayMesh shadow mesh only with the shared
  shadow material, which wind-skinned (particle-trails) and two-sided leaf materials never get.

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

### 5a. As built (stage 4)

- `TreeGenerator.decay_class` 1-5 applies the classes above to the grown tree: 1 drops broad
  leaves but keeps a conifer's needles, gone red-brown (the red stage of beetle-killed spruce and
  pine, shed in the second or third year), 2 the twigs under 1.2 cm, 3 the limbs under 3 cm and snaps the top at 55-85% of the
  height (a splintered break of pale wood), 4 leaves limb stubs of 15-50 cm on a stem broken at
  30-60%, 5 is a stump of 0.4-1.5 m. Bark weathers grey, then to pale and darkening wood.
- `fallen` lays it on the ground as a log; limbs that would reach into the ground break off, and
  on a decayed log the ones standing up have rotted off too.
- `envelope` clips growth to a box (shoots leaving it are cut back each year), for hedges and
  topiary; a clipped hawthorn keeps its full leaf count inside the box.
- Every stand (`make_stand`) now carries the regeneration and deadwood of its leading tree:
  saplings (a fifth of the age, half the spacing, tolerating deep shade) at density 0.6, snags
  (classes 2-4, 2.5x spacing) at 0.25 and logs (classes 2-4, felled) at 0.4, placed by the
  lead's own climate rules. Hedgerows have no lead and get none.

### 5b. Fallen branches and climbers (plan, 11 Nov)

**Fallen branches.** Most woody debris on a forest floor is shed limbs and broken tops, not
whole trees (Harmon et al. 1986). `FORM_BRANCH` takes one first-order limb of the lead tree,
4-14 cm at its base, after its decay class (1: twigs on, 2: twigs gone, 3: rotting), lays its
overall direction level along a random bearing and rolls it about its length, dropping side
branches that would reach into the ground. Each stand places them at density 0.7 next to its
logs, never rigged (wood on the ground does not sway).

**Climbers (vines).** What the field does:
- Thomas Luft's *Ivy Generator* (2007): a growing tip is pulled by adhesion to the nearest
  surface, gravity and a random walk, with roots where it touches; leaves along the stem, sized
  by light. The standard look for ivy on trunks and walls.
- Hädrich, Benes, Deussen and Pirk, *Interactive Modeling and Authoring of Climbing Plants*
  (Eurographics 2017): climbers as an anisotropic particle system competing for light and
  support, with stems that hang under gravity where they lose support, and that can be
  attached to moving supports.
- Benes and Millán, *Virtual Climbing Plants Competing for Space* (CASA 2002).

Our plan, joint with vegetation (which builds the leaves with its plant generator):
1. **Ivy on trunks.** Luft's tip growth over our own bark surface (the stem and limb tubes are
   analytic, so the nearest-surface pull is exact and cheap): from the root flare up the shaded
   side, thinning with height and light, in the host's bark mesh and skinned to the host's bones
   so it sways with the tree. Leaves: points along the stem (position, normal, up, size, light)
   published as mesh meta `tree_climber_leaves`; vegetation's ivy leaf mesh is instanced there
   into a `climber_leaves` surface of the host (as built: `PlantGenerator.make_climber_leaves`,
   four variants a climber baked once and shared; liana uses clematis; its albedo and backlight
   textures on the tree's own skinned material, so vines sway with the host; `climber_plant_leaves`
   off falls back to a built-in lens leaf).
2. **Hanging lianas** in deep forest (old man's beard, honeysuckle): stems that leave the trunk
   for a neighbour's limb and hang between them as catenaries (Hädrich 2017's unsupported
   stems), which needs pairs of trees, so it is built per stand by the stand methods.
3. Controls: `climber` (none, ivy, clematis, honeysuckle), `climber_cover` 0-1, and the stand
   methods set them per stand (ivy in oak, beech and riparian woods; lianas in riparian and
   oak-lime-ash on rich soils).

**As built (11 Nov).** Shed limbs are 1-14 cm, weighted to the thin end. `FORM_STICKS` adds
a dense scatter of 1-4 cm side shoots every 1.5 m inside the stand's belt and in its clearings
(shade 0-1). Calibration: beech branches of 1-10 cm rot within about 18 years (t95, Muller-Using
and Bartsch, Eur. J. Forest Res. 128, 2009), a mean stay near 6 years, so a branch fall near
1 Mg/ha a year keeps about 6 Mg/ha (11 m3/ha) of fine wood on the floor; sticks plus limbs come
close. Forestry measures it by Brown's line-intersect classes (under 0.6 cm, 0.6-2.5, 2.5-7.6,
logs over 7.6 cm); the split with the ground plants follows that: twig litter under 1 cm is
theirs, wood from 1 cm up is ours. Interface for plants growing through deadwood: log, branch
and stick species carry meta `deadwood`, and each of their meshes `tree_deadwood_axis`
(mesh space; transform by the placement): a (3), b (3), the widest reach from the a-b line
anywhere on the lying stem, N (12), then the reach at N points evenly from a to b, linear between
(the first also takes wood behind a, the last wood beyond b). Measured from the stem's own bark
and moss as built (15 Nov): it takes in the butt, a root plate and the stem's own bends off the
straight line, which the first node's radius (the old 7th float) missed by up to half a metre.
A reader of the first 7 floats keeps out of all the wood.
Appended (16 Nov, vegetation's request; 8 + 5N floats): N points along the stem's own
centreline by arc length, each x, y, z and the girth round it to the outside of the moss. The
chord reach counts a bent stick's bow as girth (a 3 cm, 3.5 m stick read as a 21 cm log), so
honey fungus and on-log plants lined thin sticks; spans between centreline points do not.

### 5c. Moss on bark (as built, 11 Nov)

Where moss grows: epiphytic bryophytes follow bark texture, pH and water holding (Barkman,
*Phytosociology and Ecology of Cryptogamic Epiphytes*, 1958): thick on rough, nutrient-rich bark
(ash, sycamore, elder, oak, alder, willow), sparse on acid or shedding bark (pine, birch, larch)
and on smooth beech except at the foot. On each tree it gathers at the foot of the stem
(stemflow and splash), on the upper faces of level limbs, and fills furrows before it covers
plates. Dead wood is colonised from the ground up and gets mossier as it softens (Soderstrom,
*Bryophyte succession on decaying logs*, 1988).

How games do it: CryEngine (Ryse) layers a moss shell over the base mesh with a vertex-painted
mask; most engines blend a world-up moss layer in the material. A material-only blend shows no
volume and the fork's path tracer runs a fixed-function StandardMaterial3D without the detail
layer, so we build geometry:
- Weight per point: `moss_cover` x species affinity x stand shade (open-grown 0.65) x decay
  (class 1 0.8 up to class 5 1.4, wood on the ground x1.3), times max(foot skirt e^(-h/1.1 m),
  upper faces) times patch noise.
- **Tint** at every LOD: the bark's vertex colour goes to moss green with the weight.
- **Shell** at LOD 0 and for the path tracer, on wood 5 cm and up: the displaced bark tube
  again, lifted by up to 1.8 cm of moss (hand-sized hummocks) where the weight is high and sunk
  under the bark where it fades, so the edge needs no alpha; it enters furrows just ahead
  of the plates. Quads wholly under the bark are dropped. Its own surface "moss": a baked turf
  texture (shoots of about 3 mm in cushions of about 4 cm, domain-warped), matte.
- Aspect is not used: placed trees turn about their axis, so a north side baked into the mesh
  would point anywhere.
- Cost: oak open-grown 56k moss triangles on 4.2M of bark; stand-grown oak 19k on 0.8M; a
  class 3 oak log 190k on 0.18M.
- Fixes (12 Nov), from a fallen beech that read as a flat lime capsule with stepped windows:
  - **Winding.** Every bark and moss tube was wound counter-clockwise seen from outside, but
    Godot's front faces (and the path tracer's back-face culling) are clockwise. Raster and path
    tracer both drew each stem's far inner wall; on a cylinder the lighting looks almost right,
    so it went unseen, but the moss shell lay behind the bark. `_build_mesh()` now flips the bark
    and moss builders, as PlantGenerator does.
  - **Tiling on logs.** The closed butt starts the tube at radius 0, and the bark tile width came
    from the first ring: bark and moss tiled every few millimetres and mipmapped to one colour.
    The tube now takes the stem's radius for its tiles.
  - **Edge.** On heavily mossed wood the weight crossed the whole ramp between two vertices, and
    the shell hugged the bark's underside, so the edge snapped to the 2.5 cm grid as stairs. The
    ramp now spans at least 2.5 vertices (from the unclamped weight) and the shell keeps its own
    depth below the bark, so the edge falls between vertices.
  - **Where on a log.** The stem-foot skirt covered a lying log all round, its underside too; on
    fallen wood the skirt is replaced by the upper half and flanks (the face in the litter stays
    bare). Fallen wood also keeps its bark relief (the soil-line fade is for standing stems).
  - **Butt face.** The closed butt's rings take normals that lean with the radius' slope, v over
    the surface, flat mapping and an end-grain colour on the face, no relief and no moss: the
    bark had converged on its centre as radial pleats.
  - **Turf contrast.** The baked turf's tone spread was about 0.06 of its mean; stretched to 0.22
    (lit tips against shaded gaps, as scanned moss shows).

### 5d. Ground contact: root flare, moss at the foot, broken and bedded logs (research, 12 Nov)

Docs declined close-ups because a trunk stood on the ground like a cylinder on a plane, the foot
moss was one pasted patch, a log's end read as sawn and logs sat on the plane with a hard edge.

What real wood does:
- **Root flare.** The stem widens over its last metre into the main roots, which run out
  radially as ridges and dive under the soil 0.5-2 stem radii out; the surface meets the ground
  at a shallow angle, never as a wall (Mattheck, *Design in Nature*, 1998: buttresses follow the
  load paths; Coutts 1983 on root architecture).
- **Soil at the foot.** Splash and stemflow darken and soil-tint the bottom 20-30 cm of bark;
  litter banks against the windward and upslope side.
- **Foot moss.** Bryophytes form a near-continuous collar at the base, where stemflow and splash
  keep the bark wet, with a ragged upper edge that runs higher up the furrows and on the side the
  stem leans away from (stemflow runs down the underside of a lean); patches only higher up
  (Barkman 1958; Studlar 1982, bryophytes on tree bases).
- **Snapped wood.** A stem breaks by diffuse fracture (fibres splinter and pull out at different
  lengths), clean fracture or buckling (Li, Schwarz, Palubicki, Pirk, Michels and Benes,
  *Stressful Tree Modeling: Breaking Branches with Strands*, SIGGRAPH 2025; Hadrich et al.,
  *Interactive Wood Fracture*, SCA 2020). A fallen trunk's butt is a root plate torn out or a
  stem snapped low: jagged fibre ends and splinters, never a flat disc.
- **Bedding.** Logs settle as they decay: class 1-2 rest on their limbs or touch the ground
  along a line, class 3 sits in the litter, class 4-5 sag, flatten underneath and sink a third
  or more into the soil and moss (Harmon et al. 1986, *Ecology of coarse woody debris in
  temperate ecosystems*; Sollins 1982).

How engines do it:
- SpeedTree builds the flare as part of the trunk spine and has a roots generator whose roots
  follow and enter the ground; artists hide the ground line with blending.
- Unreal blends meshes into terrain with Runtime Virtual Textures: the mesh material samples the
  landscape's colour and height and fades to it near the contact; Pixel Depth Offset softens the
  intersection line. Frostbite and others use the same idea (terrain colour read back in the
  mesh shader). Godot has no RVT; godotshaders' terrain mesh blending does the same with a
  height/colour texture.
- Fracture in games: authored splinter meshes or Voronoi pieces; the papers above give the shape
  vocabulary (splinter length spread along the grain).

What we build (TreeGenerator only; terrain-colour blending is a material feature for later):
1. Flare whose wall turns shallow at the soil line and keeps widening just below it, plus 5-7
   root ridges pushed out sideways that dive under the soil (no undercut).
2. Soil tint: the bark's vertex colour darkens and browns over the bottom ~25 cm.
3. Foot moss as a collar: full weight near the soil, a noisy upper edge 15-60 cm, higher on the
   lean's underside; patch noise only above it.
4. Log butt as a diffuse fracture: the end face's rings displaced along the grain by angular
   noise into fibre tongues, plus splinters standing out of the face.
5. Bedding by decay class: sunk 15% of the radius (class 1-2) to 45% (class 5), the underside
   flattened from class 4.

As built (12 Nov): 1, 2, 3 (without the lean bias), 4 and the sink of 5. The flare, ridges and
soil tint act on the bark, its relief tube and the moss shell alike, so the shell stays on the
bark. The soil tint is applied after the moss tint and on the shell's colour too, or the green
covered it. Standing out of the butt, splinter sheets read as dark slits and were dropped; the
tongues alone read as broken. Tongue lengths run in straight lines between 22 sector values:
stepped per sector, the walls between tongues faced away from their shading normals and drew
black slits under the path tracer only. The end grain is weathered grey-brown, the heart darker
with rot, and its normals and tangents come from the displaced face.
Surface roots (13 Nov): each root ridge runs on out of the flare as a lateral root. Laterals keep
their girth to the flare's edge and then taper fast (the zone of rapid taper, within 1-3 stem
radii; Eis 1974, Coutts 1983) while sinking, so they dive under the soil while still thick
instead of thinning to a point on top of it. They start inside the stem and end below ground
(no open ends), are spread 1.5x across and flattened to 0.7x (a surface root shows as a broad
low ridge, not a round stick), wander sideways, and run further on shallow-rooted species
(root_flare). They take the stem base's bone, so they do not sway.
Root weld (research, 13 Nov). Separate root tubes meet the flare in an intersection crease
and read as lobes stuck on. The published ways to a continuous junction:
- One swept mesh refined at the branch point (Lluch, Vivo and Monserrat, *Modelling tree
  structures using a single polygonal mesh*, Graphical Models 66, 2004): child and parent share
  vertices, no intersection.
- Implicit or convolution surfaces round the skeleton (Bloomenthal and Wyvill 1990;
  Galbraith et al., BlobTree trees, 2004), polygonised: smooth by construction, costly, and the
  bark UVs must be rebuilt.
- SpeedTree's branch welding (weld distance and spread blend a child into its parent's mesh),
  and for scanned trunks vertex or texture blending of the generated roots into the scan.
Chosen: the first idea in its simplest form. The trunk's own lower surface (from about one stem
radius above the soil down through the part sunk below it) is swept out into the roots: at a
root's bearing its rings run outward and down along the root's profile (full girth to the
flare, then the rapid taper, diving under the soil), between roots they keep the flare and
sink. Positions blend between the two by a cross-section bump whose width narrows along the
root, so flanks, crown and trunk are one surface with no crease. Normals come from the
displaced mesh. Bark, its relief tube and the moss shell take the same map.
Not done: the flattened underside of class 4-5 logs, and blending into the terrain's own
material (the ground side: litter and grass placed against the foot, the scene's job).

### 5e. Old trees (as built, 11 Nov; Paulo picked)

Veteran trees carry tree-related microhabitats (TreMs). Larrieu et al. (*Tree related
microhabitats in temperate and Mediterranean European forests: a hierarchical typology for
inventory standardization*, Ecological Indicators 84, 2018) catalogue 47 types in 7 forms:
cavities, injuries and exposed wood, crown deadwood, excrescences, fruiting bodies, epiphytes
and exudates. Their number rises steeply with diameter, and trees over about 70 cm carry
several (Vuidot et al. 2011; Larrieu and Cabanettes 2012). We build the ones that read at game
distance; epiphytes (moss, ivy) and crown deadwood (dead limbs) already exist:
- **Crown breakage** (injury): for broadleaves, the stem snapped at 60-85% of the height, with
  the existing splintered-top geometry. Limbs below the break keep growing.
- **Basal hollow** (cavity, heart rot reaching the root collar): an opening 0.9-2.2 stem radii
  tall, pushed 50-70% of the radius into the stem, rough-floored and dark, with a callus lip.
- **Rot holes** (cavity where a limb broke off): 20-50 cm, on the clear bole.
- **Woodpecker holes**: round, 5 cm (great spotted) or oval, 12 cm (black woodpecker, a quarter
  of them), 2.5 m up to the crown. They are their own small disc with a pale chiselled rim, since
  the stem's vertices are too coarse for them.
- **Burls** (excrescences): lumpy swellings of 0.3-0.7 stem radii, 30-55% of the radius proud
  (at 18-35% they did not read at 4 m).
- **Epicormic shoots**: clusters of 3-8 water shoots from dormant buds on the stem, many on
  oak and lime and few elsewhere, leafy by season. They live for years and branch (Meier,
  Saunders and Michler 2012; Colin et al. 2010 on oak), so each shoot has an age: 0.3-1.1 m long,
  4-11 mm thick, out from the bark then up, older ones with 1-3 side twigs, in a warmer brown than
  the stem. In winter a cluster reads as a twiggy brush (12 Nov; before, single 3-6 mm shoots
  were lost against the bark).
Hollows, burls and shoots sit on the clear bole: below the crown base and below the first fork
with a limb over 45% of the stem's radius, since in the first renders features higher up hid among
the limbs. `veteran` (0-1, default 0) sets how many; `get_veteran_features()` lists them (kind,
height, azimuth, size) for birds, bats and gameplay. Stems under 16 cm diameter get none. Features are cut
into the main stem's plain, relief and moss tubes alike, working on the undisplaced tube so the
three stay together, and normals are rebuilt there. The tree's growth and skeleton do not change.

### 5f. Bark peeling on dead wood (15 Nov; Paulo: "all of them if they are worthwhile")

Research. Bark loosens once the cambium under it dies and rots, and falls in plates and strips
along the grain. The deadwood decay classes (Maser et al. 1979) set how much: class 1 intact,
2 loose with some lost, 3 traces only, 4 and 5 none. A standing snag keeps its bark longest at
the foot and loses it from the top; a log first on its upper side, where it dries and splits.
Weathered wood in the open goes silver-grey, rotting and on the ground it browns, and it checks
along the grain as it dries.

As built (`bark_peel`, 1 by default, 0 keeps the bark):
- **Where.** A field per bark vertex: noise stretched fivefold along the stem, plus height on
  snags or the upper side on logs, against the class's share (8, 35, 65, 95, 97%). Moss holds
  the bark under it. A log's broken end face is left out.
- **Shape.** Bare vertices sink to the furrow floor of displaced bark (the plates above it are
  gone), or by the bark's thickness on smooth tubes. Per vertex and interpolated, never stepped.
- **Look.** No extra surface or geometry: the bark shader reads the bare field from the vertex
  alpha (lichen keeps the upper half of it), breaks the edge along the bark's furrows (the
  relief in the roughness map's blue channel), darkens the bark's broken rim, and draws wood:
  fibres along the stem, long drying checks, grey or brown by class, smoother normal, roughness
  0.8. The whole-tree weathering tint of classes 3 to 5 is lighter, since the wood now shows.

### 5g. Fungi on wood as geometry (15 Nov; Paulo: "real geometry is going to add a lot")

Research. The fruit bodies on a tree tell which rot is in it and how far it has gone (Rayner and
Boddy 1988, *Fungal Decomposition of Wood*; Stokland, Siitonen and Jonsson 2012, *Biodiversity
in Dead Wood*):
- Perennial brackets on standing dead and dying wood: the tinder hoof (Fomes fomentarius) on
  beech and birch, grey with a ridge for each year; the red-belted bracket (Fomitopsis pinicola)
  on conifers; the birch polypore (F. betulina), a smooth buff cushion, on birch only.
- The artist's bracket (Ganoderma applanatum), a flat brown shelf with a white margin, low on
  old broadleaves; chicken of the woods (Laetiporus), sulphur-orange fans in tiers, on old oaks.
- Annual tiers of thin zoned brackets (Trametes versicolor) on logs and stumps.
- Gilled mushrooms in tufts: honey fungus (Armillaria) at the foot of dying and dead broadleaves,
  sulphur tuft (Hypholoma fasciculare) on stumps and logs.
- Brackets are level whatever the wood does: the tubes grow downwards (gravitropism).
Shape references: caps as revolved profiles and gills as radial blades (Desbenoit, Vanderhaghe,
Galin and Grosjean, *Interactive modeling of mushrooms*, Eurographics 2004). Games at the bar
(KCD2's forests, Nyx I) place fungi as authored props; nothing public on how. Here they grow from
the tree's own decay state.

As built (`fungi_cover`, 1 by default, 0 none), a surface "fungi" of the tree mesh, skinned to the
wood it grows on:
- **Where.** By host and decay class: snags carry hooves (2-3 on class 2-3), broadleaf snags a
  Ganoderma low down, logs Trametes tiers (up to 5 on class 2-3), veteran oaks a Laetiporus, the
  feet of dying broadleaves honey fungus, stumps and logs sulphur tufts. A node of the wood thick
  enough is picked in proportion to its bark area; the bracket sits on the nearest bark vertex.
- **Brackets.** A shell from the back edge (following the stem's round, sunk a fifth into it) out
  to a rounded margin: hoof or shelf by profile, a ridge per growth zone, zoned colour, a pore
  face underneath (texture of tube mouths), wavy margins for the fans.
- **Mushrooms.** Stalks fanning from one foot and turning up, caps revolved and kept level (domed
  when young, flat and wider when old), an underside and 22 real gill blades kept under the cap's
  margin.
- **For ground plants.** Mesh meta `tree_fungi`: per cluster position (3), the wood's normal (3),
  width and kind (0 Fomes, 1 F. pinicola, 2 F. betulina, 3 Ganoderma, 4 Laetiporus, 5 Trametes,
  6 honey fungus, 7 sulphur tuft), in mesh space. Ground mushrooms belong to vegetation.
- **Cost.** A heavy log or snag gets 2-8 clusters, about 2,000-20,000 triangles, against 1-4 million
  in the tree; one draw call more per tree mesh that has any. Living trees without veteran
  features get none.
- **Through the moss.** Fruit bodies push up through moss, so on mossed wood a cluster stands on
  the moss shell's surface (lifted by the shell's own height ramp), not on the bark under it: in
  the beech stand most turkey tails and tufts on mossy logs were buried. Vegetation bench:
  `--deadwood` with `VEG_DW_SIZE=96 VEG_DW_SEED=24 VEG_DW_VARIANTS=3 VEG_DW_FUNGI_VIEW=1` frames a
  class 3 log with a Fomes and a sulphur tuft in the stand; `VEG_DW_FUNGI=0` and the printed
  `VEG_DW_CAM` give the same view without fungi. About 24 minutes a frame on the cloud.

### 5h. Lichen strands on twigs (15 Nov)

The crust lichens (2h) are a texture; the fruticose lichens that hang from or stand off twigs
are what reads as "old, damp wood" at arm's length, and no texture can give their silhouette.
Two forms, as geometry in the fungi surface:

- **Beard lichens (Usnea).** Tufts of 6-14 hanging strands 5-16 cm long, under 1 mm thick,
  branching twice and set with short fibrils (spinules), from the underside of twigs 4-35 mm
  thick in the crown. Hosts: conifers, oak, birch, black alder (clean, humid air; Hawksworth and
  Rose 1970 on Usnea as an air-quality indicator).
- **Oakmoss (Evernia prunastri).** Tufts of 5-9 flat lobes from one holdfast, each forking two or
  three times (4-6 mm wide below, 1-2 cm per fork), standing off the twig and drooping as they
  lengthen; grey-green above, white beneath (two faces). On broadleaf twigs.
- **Where.** Clumps are placed by the crust lichens' light and patch weight (without their
  preference for older bark: fruticose lichens take to young twigs), more in a stand's lit crown
  (`forest_shade`), scaled by the tree's height, `lichen_cover` scales them; none on logs.
- **Cost.** LOD 0 only (the surface's coarser levels keep the fruit bodies and drop the strands,
  as the moss shell does): stand-grown oak +31k triangles on 1.68M (+1.8%), Scots pine +8.8k on
  110k, beech +52k on 544k. Generation time unchanged within noise. Millimetre threads alias in
  raster beyond a few metres; that is where LOD 1 takes over.
- **Bench.** `--view=lichen` (env LV = a vertex of the fungi surface, LDIST), with `--day=20` for
  bare winter twigs. Path tracing keeps every strand.

### 5i. Old ivy stems (15 Nov)

Ivy on an old trunk is a net of woody stems, not a garland of leaves: stems thicken for decades
to several centimetres, grey-brown and fissured, pressed flat to the bark by their aerial
rootlets (Metcalfe 2005, Biological Flora of the British Isles: Hedera helix). The climb itself
stays Luft's Ivy Generator walk (2007, 5b).

- **Age.** Each main stem gets an age from the host's (height over 6-28 m, or `veteran`) times
  `climber_cover`, jittered per stem; side shoots carry 0.6 of their parent's. Main stems start
  at 6-11 mm and thicken up to 4.5 times with age; colour goes from brown to grey-brown.
- **On the bark.** The stem's axis lies 0.8 of its radius out from the bark, plus half the bark
  relief's furrow depth where LOD 0 displaces it, so stems sit on the ridges rather than inside
  them (it was a fixed 6 mm lift: thick stems floated, thin ones sank into relief bark).
- **Rootlets: built and dropped.** Tufts of 3-9 mm hairs either side of the stem below 3.5 m
  cost about half a million triangles on an old oak and were under a pixel at a metre; a felt
  fringe needs thousands per stem. Not worth it as geometry.
- **For vegetation.** `tree_climber_leaves` is now 13 floats per leaf: the 11 before (position,
  blade normal, petiole direction, size, light), then the stem's age (0-1) and how far up the
  stem the leaf is (0 foot, 1 tip). Vegetation's adult ivy leaves (unlobed, on shoots standing out
  from the stem) are for points above about two thirds of an old stem.
- **Adult leaves (as built).** Ivy leaves on stems of age 0.35 and up switch to vegetation's
  `CLIMBER_IVY_ADULT` (unlobed, ovate, glossier) above 0.6-0.75 of the stem's height, the band
  starting lower on older stems, in their own surface `climber_leaves_adult` (its own texture).
  An old oak with full ivy: about 11% of the climber leaves are adult.
- **Cost.** Open-grown oak with full ivy: its stems add 158k bark triangles (+4%); the stems'
  side count follows their radius, so thicker stems cost about what the thin ones did.

### 5j. Broken limbs, splinters and hangers (15 Nov; Paulo picked)

A storm-broken limb is the commonest wound on an old tree: wood fails in tension on the side the
load pulls and buckles on the side it pushes, so the stretched side tears out long splinters of
the outer wood, the compressed side breaks short, and bark tears back on the stretched side (van
Casteren et al. 2012, Why don't branches snap?; Li et al., Stressful Tree Modeling: Breaking
Branches with Strands, SIGGRAPH 2025, fracture by strain thresholds along the fibre). Broken
limbs and hangers are among the commonest tree-related microhabitats (Larrieu et al. 2018).

- **Fracture (`_fracture`).** Every snapped end (a veteran's broken top, a snag's top, a limb
  that rubbed through, a storm break) gets a jagged face over the end, higher and more ragged on
  the tension side; 12-20 three-sided slivers tapering to blunt ends, three quarters on the
  tension side; and a torn strip of bark. It replaces the nine spikes there were. Green wood
  (living trees) breaks fibrous: up to 12 more slivers, about twice as long, the face torn
  further; dry dead wood snaps short and flat. Fresh wood is pale, weathered wood grey. Its own
  surface "wood" (one LOD) with a fibre texture and normal map.
- **Storm breaks (`limb_breakage`, default 1).** Living trees (not hedges) lose a Poisson number
  of limbs: mean 0.5 for a tree of 28 m and up, plus 1 for a full veteran. Limbs 5-22 cm thick
  above 2.5 m, picked by cross-section and by how much they spread (their own weight bends them
  too); each snaps 25-65% along its length.
- **Hangers.** About 45% of spreading breaks keep the broken part hung by a strip of bark: it
  swings down about the break beside the stub, dead and leafless, with its own fracture facing
  the stub. One that would reach the ground falls instead.
- **Cost.** Each fracture is about 670 triangles; a veteran oak with breakage gains 700-2,000
  triangles (under 0.01%), and generation time is unchanged within noise.
- **Checks.** Raster and path-traced close-ups keep the same face and splinters (PT does not
  remove detail).

### 5k. Uprooted trees: root plate and pit (research, 15 Nov; Paulo picked)

Windthrow tips a tree out by its roots rather than snapping it: the root plate (the lateral
roots and the soil they hold) hinges up about its leeward edge, and the tree lies with its butt
held up by the plate, which stands on edge beside the pit it came out of (pit-and-mound
microtopography; Schaetzl et al. 1989, Norman, Schaetzl and Small 1995).

- **Sizes.** Norway spruce thrown on mineral soil: DBH 25.5 cm, plate radius 1.4 m, plate height
  1.1 m, centre depth 28 cm (max 46 cm), 0.5 m3; on peat DBH 31 cm, radius 2.0 m, depth 45-50 cm
  (max 83 cm), 1.5 m3 (Krisans et al. 2020, Forests 11:1143). So radius about 5.5-6.5 DBH and a
  plate 0.3-0.5 m thick. Shallow-rooted species (spruce, beech on wet soil) throw the largest.
- **Pit.** Pits are deepest when fresh and fill as the plate sheds soil back into them: 0.95 m at
  one year, 0.65 m at three, 0.52 m at five (Zadrozny et al. 2017, iForest 10:783); the soil
  that falls off the plate builds the mound, which can last centuries.
- **Decay of the plate.** A fresh plate is a wall of soil, stones and torn roots, its old top
  (litter, moss) facing away from the fall; as the roots rot it slumps into a mound beside the
  pit (Krisans et al.). By decay class: 1-2 a full plate, 3 a slumped plate with roots showing,
  4-5 a low mound with root stubs.
- **Roots.** Rhizomorph (Li et al., SIGGRAPH 2023) grows roots with the shoots in a soil
  volume; we keep our own structural roots (the root collar, 5d) and add the plate's laterals
  radiating to its rim, sinkers under it, torn ends past the soil.
- **As built (`uprooted`, on a fallen tree; class 3 and older rot to a plain log).** The plate
  hinges about a point 0.75 of its radius out on the fall side, the stem turning with it until
  its top reaches the ground, so the butt stands about a metre up on the plate. Plate radius 11
  stem radii (0.9-2.8 m), thickness 0.22 of that (0.25-0.6 m), both 0.8 once rotting. Its own
  surface "soil" (vertex colour, one LOD): a ragged lobed rim, the old forest floor on top
  (litter hummocks, moss patches), a lumpy domed underside with clods, humus over mineral soil
  mottled grey and rust, and 6-15 stones. In the bark surface: 8-13 laterals from a thick root
  crown along the underside, mostly bared, to torn ends past the rim that hang down; side roots;
  3-5 sinkers torn under the stem; about 30 fine roots per metre of radius hanging off the
  underside. Torn ends over 1.2 cm get a fracture (5j).
- **On landing (Blast).** From the break on, a worker thread builds the part's Blast asset
  (cached per break, by tree, bone and wind bearing): one chunk per bone of 3 cm and up, thinner
  bones riding the chunk below, every surface of its wood and foliage per chunk, a closed hull
  of its wood (cylinders at 0.85 of the bone's radius, starting clear of the wood they grow from
  so pieces never start inside each other; a root plate's hull is a thin disc of 0.8 of its
  radius, so the thrown tree lies propped on its plate standing on edge, the butt held off the
  ground) and a bond to the chunk it grows from with the wood's breaking moment as its health.
  At the part's first hard hit (a contact impulse over 1.5 m/s of its mass) it is swapped for a
  `PhysXDestructible3D` in the same pose, and the hit breaks the joints around it by its
  impulse over a metre's lever, against each joint's strength. A thrown tree swaps only once
  it has turned 30 degrees on its hinge (earlier, the plate's disc starts in the ground and the
  tree is stood back up on it), on a hit or once it rests propped on its crown.
  Pieces weigh by hull volume, so the plate's disc is thin (a full-thickness one outweighs the
  stem and rights the tree). The plate's soil is a lens, about a quarter of its disc's volume
  (Krisans et al. 2020: 0.5-1.5 m3 for spruce plates of 1.4-2 m radius); old trees' plates
  reach 3.6 m radius (7 m across).
- **Depenetration.** Blast pieces that split start touching at the break, and PhysX by default
  pushed overlapping bodies apart at any speed: limbs flew off at 30 m/s and through the ground.
  `PhysXDestructible3D.max_depenetration_velocity` (3 m/s by default, from the PhysX thread) caps
  it per piece.
- **Crown crush.** While a fallen part rests propped (its centre of mass 1 m or more above
  what it rests on), the limb under it carries the share of its weight the foot does not; from
  that rest toward the foot, the first joint whose wood cannot carry that load's moment breaks,
  and it settles further, until it lies on wood that holds. Limbs hung on their fibres let go
  when the wood below them breaks or the tree is thrown.
- **Cost (cloud, software renderer, 960x540).** Asset build 0.5-1.2 s per thrown tree and 20-50
  ms per limb, on a worker thread, then `PhysXBlastAsset.prepare_render` there too (0.3-0.7 s),
  so the swap only uploads: about 0.25 s on add_child and 0.3 s on the first hit for a thrown
  tree (was 1.1 and 1.2 s before the PhysX thread's per-chunk meshes), 1 ms for a limb. The
  destructible starts with the part's velocity. Under path tracing a zero-sized skinned instance
  still costs its full BLAS, so once every placement of a MultiMesh is uprooted the MultiMesh is
  hidden (a thrown birch 9.9 GB to 8.1 GB in the cloud). PC timings and memory are on its queue.
- **For terrain and plants.** Mesh meta `tree_root_pit`: centre (the old foot, mesh space),
  radius (0.9 of the plate's), depth (1.6 plate thicknesses, 0.35-0.95 m, shallower with decay).
  The bench's ground is flat, so the pit shows only where the terrain digs it. The deadwood reach
  profile (5b) takes in the plate and roots.
- **Stands.** Of each stand's three log variants the freshest (class 2) is uprooted.
- **Cost.** Beech log (stem radius 0.54 m): +34k bark (roots), 14k wood (fractures), 5.4k soil
  triangles, about 8% over the snapped log; generation time unchanged within noise.

### 5l. Pollards and coppice (15 Nov; Paulo picked)

For centuries woodmen cut trees back for poles, fodder and firewood: at the ground every 7-25
years (coppice: hazel 7-10, ash and oak 20-25) or at 2-3 m, out of reach of browsing stock
(pollards: willow every few years, oak, beech, hornbeam and lime every 10-20). Dormant and
adventitious buds round the cut break into a ring of straight poles of one age; cut after cut,
stubs and the callus over them build a swollen head (the bolling) or a widening stool. A lapsed
pollard, uncut for decades, carries a few limbs as big as trunks from a short fat bole: the
veterans of wood pasture (Rackham 2006, Woodlands; Read 2000, Veteran Trees: a guide to good
management).

- **As built (`cut_height`, `cut_cycle`, `cut_since`).** Cutting is grown, not drawn on: the
  growth model cuts the tree back during its years. The first cut, once the stem stands 2 m over
  `cut_height` (and from year 8), takes each stem (coppice, `cut_height` up to 0.5 m) or the
  thickest (pollard) at that height, clears the bole below, and sets 3-6 (stool) or 6-12 (head)
  short knuckles round the cut. Every `cut_cycle` years each pole grown since is sawn off 3-10 cm
  from its knuckle, and one or two new sprouts break from round the cut on the knuckle (not from
  the stub), 15-40 degrees off vertical on a stool, 20-55 on a head. Each cut builds the knuckle
  out a little (a stool's by about a centimetre a year, up to 45 cm: old stools are rings of
  lobes a metre across). Stubs end in a flat sawn face (surface "wood": rings, pale when fresh,
  grey and dark-hearted with age) and rot off after two cycles. The last cut is `cut_since` years
  before the end (-1: within the last cycle).
- **Poles.** A resprout's first four years get 1.6 times the vigor and twice the yearly length
  cap (coppice shoots make 1-3 m a year): straight poles of one age; after that they grow on as
  ordinary limbs, so a lapsed pollard's poles become a crown.
- **Head.** At the end the head's nodes swell (the stem's top 1.35 times plus 0.12 per cut, up to
  1.9; knuckles 1.2 plus 0.06 per cut; the stem flaring over its last half metre), end full-girth
  in their callus knob, and the knuckles reach out to the swollen head's surface carrying their
  stubs and poles. Each knuckle runs on into its thickest pole. A pollard head carries 4 plus 2
  per cut (up to 14) burrs round it (the old-tree burls of 5e, each a cluster of knobs, most with
  a few water shoots); a stool's lobes are at least 0.4 of their length thick, fused into a low
  mound. Whatever the species' bark, the head itself is knobbly geometry (rings every 4 cm; broad
  and fine noise knobs, ridged-noise fissures darkened in their floors) with a green film of
  algae and moss in its hollows and on its tops.
- **Fixed after the first docs shots (15 Nov).** Later cuts used to leave a stub and sprout from
  its tip, so cut after cut stacked swollen stubs into stepped "telescopes" with dark lips under
  them; the poles left their knuckle as forks from a capped end and sat in round sockets; the
  head's own swell was skipped (the ramp below a knuckle claimed the stem top first); basal
  sprouts kept coming after the first cut and grew into big uncut stems on a stool; the foot
  moss sleeve wrapped the whole stool. Smooth bark (beech, hazel, hornbeam) now carries fine
  crossways creases and stronger mottling. Bench: `--pollards` (willow pollard, lapsed beech
  pollard, hazel stool in winter grass).
- **Cost.** A cut tree is smaller than an uncut one of the same age: willow pollard (cut every
  10 years) 1.1M bark and 1.3M leaf triangles against 5.7M and 8.0M uncut; generation 1.3 s
  against 5.3 s. Hazel coppice regrown 6 years: 91k bark.
- **Modelled, not painted (16 Nov, after the third docs review).** The reviews kept finding the
  relief painted: soft clay lumps, fissures as dark lines, poles as smooth pipes plunged into the
  head, a willow head too small with twiggy poles. Now:
  - *Burrs.* Hard-edged knobs on a jittered 3D grid, each a spherical cap with a near-vertical
    foot (profile (1 - d^2)^0.25), present where a cluster noise is high, so they gather into
    burrs (each knob a dormant bud's trace that swelled; Read 2000). Crevices round them are
    darkened. Cells 0.12 of the head's radius (3.5-7 cm). The old smooth burl lumps under them
    are half as tall. Ridged-noise fissures cut 0.14 of the head's radius deep.
  - *Mesh.* Over the head the rings are 1.3 cm apart and the stem has up to 256 sides; the
    knuckles round it share its knobs (fading up each pole within a pole's radius) at 1.6 cm.
    A knuckle and the pole it runs on into are built as two tubes crossing over three rings,
    so the pole keeps 12-32 sides. Over the head's top the knobs push out from its centre, not
    sideways from the ring's: sideways they smeared into radial pleats round the cap. The
    bark's own furrows are three-quarters closed over on the head (they crossed the knobs as
    long pleats).
  - *Poles.* Displaced, on a near tube however thin: shallow flutes along them on the tube's
    own frame (none on rods under 3 cm, which are round), a faint raised girdle scar at each
    year's start, and rings of buckled bark ("elephant skin") over 2.5 radii where a pole
    leaves its knuckle, which swells 1.3 times there as a branch collar (laterals too).
  - *Stools.* The same knobs (5 cm cells) and fissures over a coppice stool's lobes, fading up
    the rods' first centimetres.
  - *Willow.* A pollard willow's withies grow unbranched for six years after a cut (osier and
    pollard willow, Rackham 2006), five to nine from each seat. Two bugs thinned them: the
    knuckles were shed as shaded limbs, taking every withy on them, and young poles self-thinned
    after five years; now the knuckles are never shed and poles not before a cutting cycle.
  - *Boles.* The pipe model sizes wood by the crown it carries, so a pollard's bole stopped
    thickening at its first cut (a willow of 80 years stood on a 12 cm stem). It now lays a ring
    a year (willow 3 mm of radius, others 1.5 mm; Read 2000), and the head swells 1.2 plus 0.05
    per cut (up to 1.5; knuckles 1.15 plus 0.04, up to 1.35) over that.
  - *Water shoots* start on the burr they break from: from the plain stem's radius they started
    inside it, so only those on small burrs showed.
  - *Hazel* has its own bark family (`BARK_LENTICELLED`): glossy, dense pale raised lenticels,
    the thin skin peeling in short paler strips (Vaucher 2003), and grey-brown, not the red-brown
    that read as plastic pipe.
  - *Cost* (LOD 0 bark triangles, bench trio, seed 1): willow pollard 42k to 116k (it is now a
    real pollard, with four times the withies), lapsed beech 887k to 902k, hazel stool 386k to
    660k (the stool's knobs, and more rods now that young poles are not thinned). LOD 1 and on
    keep the plain tubes.
- **Fourth review (16 Nov).** Willow passed; fixes for the rest:
  - *Withy tips* ended in combs of side twigs (the leaf twig spray of an ordinary shoot). A
    rod within six years of its cut now carries only its leaves on stubs, no twig spray.
  - *Collar folds* were even full rings, a corrugated hose. Each fold now wobbles in phase,
    runs part of the way round (a patch mask) and varies in depth, a soft fold above a sharp
    crease.
  - *Thick poles* carry branch scars: a low callus lip round a dark pit with a chevron of
    wrinkled bark above (beech's "Chinese moustache"; Vaucher 2003), in half-metre cells a
    quarter of the way round.
  - *Rods* (under 8 cm) swell a little at their nodes, alternate sides every 12-20 cm, the
    scar darker: hazel and willow rods are never straight pipes. Hazel's lenticels are fewer,
    larger and paler, so they still read as a speckle at a few metres.
  - *Head.* The smooth burl lumps are halved again (0.07 of the head's radius) and a burl on a
    pollard head gets its own cluster of finer knobs, so no smooth or waxy bulge is left.
  - *Hazel stool.* The pale pinwheels were not the saw faces: each stool knuckle closed with a
    bark dome, its bark converging to a point. A knuckle now ends in the first cut, sloping
    outward as coppice is cut, under a callus lip rolled over its rim (Shigo 1985), the face
    sunk and rotted dark with moss in its hollow. Sawn stub faces follow the tube's own end.
    Coppice rods get an algae film up their foot and pale crust lichen patches.
  - *Pollard knuckles* whose poles are all gone close in a broad dome on eight rings, knobbed
    like the head: on three, the last fan of long triangles shaded as a dark pinwheel.
  - *Cost* (LOD 0 bark): willow 112k, beech 905k, hazel 668k.
- **Not yet.** Stands do not cut anything yet: coppice-with-standards (a hazel understory
  coppiced under oak) and pollard rows along rivers are the natural next step.

### 5m. Next round (research, 15 Nov; not built)

Sources checked: Li et al., Stressful Tree Modeling: Breaking Branches with Strands (SIGGRAPH
2025; strand stress decides where limbs snap, matching what 5j fakes with load weights); Li et
al., Interactive Invigoration (TOG 2024; strands, for junctions); Unreal 5.7's Procedural
Vegetation Editor (experimental; growth graphs feeding Nanite assemblies: trees built of shared
instanced parts, so twigs, cones and fruits cost little each). The gaps against KCD2 and Star
Citizen's forests are now small visible stories, not the growth model:

1. **Small geometry in the crown.** Mistletoe balls (Viscum on lime, poplar, apple; evergreen,
   so it reads in winter), witches' brooms (birch, hornbeam), and the reproductive parts as real
   geometry: cones (pine, spruce, larch), catkins (hazel, birch, alder: late winter), seed keys
   (ash, maple, hornbeam) hanging in winter, alder cones. Shared instanced parts as UE's
   assemblies do; the far meshes leave them out.
2. **Wood pasture and coppice stands.** Stands from 5l: coppice-with-standards (hazel stools
   under scattered oaks, cut in coupes of different ages), pollard rows along rivers and lanes
   (willow, black poplar), lapsed beech and oak pollards in wood pasture.
3. **Bark stories.** Lightning scars (a spiral strip down the stem), frost cracks with callus
   ribs, deer and squirrel bark stripping (a pale band at browse height), resin runs on conifer
   wounds, dead lower branches kept on shaded conifers.
4. **Hard places.** Exposed roots over banks and rocks (roots draped over a stone or a cut bank),
   wind-flagged and krummholz trees at the treeline and on coasts.

### 5n. Crown parts (15 Nov; Paulo picked "all three": crown details first)

As built (`fruit_cover`, `mistletoe`, `witches_brooms`; surface "crown_parts", vertex colour,
two-sided, no snow): small shapes placed many times, the way Unreal 5.7's vegetation assemblies
instance parts, merged into one surface. LOD 0 carries all of it; the coarser LODs keep only the
mistletoe and brooms, which read from far.

- **Cones** of spiral scales (137.5 degrees), ovoid or cylindrical, open or closed: Scots pine
  (1-3 at upper shoot tips, 4-7 cm, bent back, grey-tipped scales), Norway spruce (pendant 10-16 cm
  at the top), larch (2-3.5 cm upright along the shoots, kept for years), hemlock (2 cm), white pine
  (10-16 cm, slender), alder's woody fruit cones (1.5 cm, in clusters).
- **Catkins**: hazel (1-3, 2.5 cm closed from August, 6 cm yellow and hanging from late January to
  April), birch (from late summer, opening in April), alder (3-5, purple, opening February).
- **Keys**: ash (bunches of 10-28 twisted keys, buff in winter, hanging August to April), maples
  (paired keys, spread wider on sycamore; fall by late autumn, a fifth left).
- **Berries**: holly and yew (female trees only, by seed), haws, rowan and elder clusters, each in
  its months.
- **Mistletoe** (Viscum album; Phoradendron in North America, darker, broader leaves): on hosts in
  the upper outer crown, a ball of yellow-green stems forking every year (4-8 years old), paired
  strap leaves at the ends, white berries from late October to February. Host chance per tree:
  lime 0.5, hawthorn 0.35, willow 0.3, rowan 0.25, maples, birch, alder, red oak 0.15.
- **Witches' brooms** (Taphrina betulina) on 45% of birches: a swelling on a limb and 50-120 thin
  dark shoots from it.
- **Budget**: about 360 clusters at most per tree, so a 20 m crown does not carry tens of
  thousands. Cost: lime with three mistletoe balls 11k triangles; hazel in catkin 75k; ash with
  keys 53k; spruce cones 9k (against millions of bark and leaf triangles).
- Fixed with it: a birch's thin shoots and leaf twigs took the trunk's white and read as frosted
  sprays; they are dark red-brown now, turning white from 1-3 cm.
- **Modelled, round 2 (16 Nov, held to the pollards' standard).** A 3x close-up audit found the
  parts painted or schematic:
  - *Catkins* were smooth tubes, sausages. Now a thin axis curving under its weight, clothed in
    spiral bract scales (137.5 degrees, a scale per fifth of the radius), each a small peaked
    shield; closed they lie flat and overlap, open they stand at about 35 degrees with a tuft of
    pollen-yellow anthers under each (Proctor and Yeo 1973).
  - *Witches' brooms* were straight rays from one point, a sea urchin. Now 16-34 shoots from a
    lumpy dark gall of overlapping knobs, each a run of 3-6 cm internodes kinked at every bud,
    branching twice and turning upward: a bird's-nest of twigs (Spanos and Woodward 1994).
  - *Ash keys* hung from one point as a skirt. Now a panicle: the stalk branches into three to
    five sub-stalks, the keys hang along them on their own stalks, each with its seed swelling.
  - *Berries* floated round a point. Each now hangs on its own stalk from the cluster's (a
    corymb on rowan and elder) and carries the dark star of its calyx at the far end.
  - *Cost* (LOD 0 crown parts, seed 1): hazel in flower 75k to 255k (closed in autumn 98k),
    rowan in fruit 384k, ash with keys 281k, birch with brooms 82k. LOD 1 on keeps only
    mistletoe and brooms, as before.
- **Rowan, round 3 (16 Nov, after the docs review declined a close-up).** Real rowan (Raspe et
  al. 2000, *Sorbus aucuparia*, J. Ecol.; Rich et al. 2010):
  - *Corymb*: a flat-topped dome of 90-160 berries (elder 150-240, smaller) on a red stalk that
    branches three times (peduncle, 5-7 rays, branchlets, pedicels), each stalk bowing under the
    weight; berries sit on a sunflower spiral over the dome, each with its dark calyx star. At
    most 80 heads a tree. Rowan in fruit, crown parts 675k (was 384k), elder 859k.
  - *Leaflets*: lanceolate, about 3.5:1 (half-width 0.15 of the length; ash 0.19, elder 0.24),
    a three-step lens instead of a diamond, so pinnate near leaves double (rowan 7.1M to 14.2M
    near triangles; LOD 1 on unchanged).
  - *Leaflet texture* (16 Nov, second docs review): leaflets were flat untextured quads. The
    blade is now a quarter wider than the leaflet and an RGBA texture cuts it (alpha scissor
    with alpha to coverage): a smooth lanceolate outline, widest below the middle with an
    acuminate tip, and about 22 serrate teeth a side on the upper three quarters, each rising
    slowly toward the apex and dropping back sharply (Ellis et al., Manual of Leaf Architecture,
    2009). Teeth as geometry had cost 64M triangles; as alpha, no triangles. RGB carries a pale
    raised midrib and about 14 looping secondaries, the lamina darker between them. Ash and
    elder share it. Raster leaves stay opaque on every other species.
    Under the path tracer the cut first shaded half of each near leaflet black: alpha to
    coverage put the material in the transparent class, peeled as a layer without its
    indirect light. The tracer now treats alpha to coverage as the alpha test it is (any-hit
    at the threshold, opaque shading); spruce sprays render unchanged.
  - *Undersides*: the flutter shader mixes back faces (FRONT_FACING, which the path tracer
    follows) toward a paler grey-green: rowan 0.5, white willow 0.75, lime and sycamore 0.35,
    beech, alder and elder 0.12, others 0.2. A crown seen from below is mostly undersides.
  - *Bark*: rowan takes the lenticelled family (hazel's), grey with pale lenticels; plain smooth
    bark read as bare grey tubes at half a metre.
  - *Winter buds* on every shoot tip under 1 cm (days 200 to 105, half size in early autumn):
    per species length, width and colour (beech a long copper spindle, rowan dark purple and
    conical, ash black, oak clustered), at most two per tip; oak 750k, rowan 254k.

### 5o. Deadwood and bark from the floor (16 Nov; vegetation's asks)

- **Sticks** (`FORM_STICKS`): fine woody debris breaks into pieces as it falls and as it lies, most
  of them under 1.5 m (Harmon et al. 1986; Woodall and Monleon 2008, FIA fine woody debris tallies
  by length). A stick is now a 0.35-1.6 m piece of a twig or small branch, snapped at its far end
  with a fracture face (5j), kinked 6-16 degrees at each node (the zigzag a twig grows bud to bud),
  litter-stained brown rather than weathered grey, with less moss (thin wood dries between
  showers). Side shoots standing more than 15 cm up broke off.
- **Resting on two points**: a laid limb or stick tips about its level line until its underside
  rests on two points of the ground, then sinks 15% of its radius; the end radius tapers to the
  next internode's (a tip above the ground read as floating).
- **Logs settle** (`_settle_log()`): walking out along the bole, each internode that ends above
  or in the ground turns about its start, with the wood beyond it, toward lying on the ground, by
  at most 4-7 degrees per internode while sound and 12-30 once soft (Fraver et al. 2002: decay
  class by ground contact and collapse). A log thrown with its root plate touches down two
  fifths to three fifths of the way out (sooner once rotten) and lies along the ground beyond,
  instead of bridging from the plate to its top.
- **Root plate**: the underside torn in blocks (head-sized relief), 22-32 soil clods per metre of
  plate radius held in the root mat (smooth subdivided lumps in the soil's colours), 14-29
  soil-stained stones from grit to head size, 14 torn root stubs per metre of radius standing out
  of the face with fracture ends, and the laterals out of the soil by most of their girth
  (Coutts 1983, 1986; Schaetzl et al. 1989 on pit and mound).
- **Smooth bark films**: beech funnels up to a fifth of the rain down its stem (Levia and Frost
  2003). The water runs in a few wandering tracks that stay dark and carry green algae
  (Desmococcus, Trentepohlia); between them the bark is mottled by algae low on the stem and pale
  crust lichen higher up (Barkman 1958). Vertex colour on stems over 4 cm of smooth or
  lenticelled bark, wetter in a stand; holds at every LOD and under path tracing.
- **Round 2 (docs review)**: the plate's underside is a 4 cm grid with block and crumb relief
  plus ~130 crumbs per metre of radius (soil aggregates), clods and stones are shared-vertex
  spheres (stones quantised into facets), roots swell and pinch and carry soil crusts, and a
  mat of ~110 fine roots per metre of radius hangs from the face. Smooth bark's texture carries
  crust lichen colonies: pale Pertusaria/Lecanora discs with dark rims and Graphis script.
  Shed pieces whose parent is gone are built like hangers (the fork code had pulled some
  sticks' rings onto the old parent's axis). A lapsed pollard's thinned poles end in short
  snapped stubs. Pinnate leaves have a grooved rachis ribbon.
  Bare wood under lost bark: grain sampled at 0.3 tiles along (it had been stretched over a
  hand's length and smeared into streaks), finer fibres, rot staining, spalting zone lines
  (Rayner and Boddy 1988) and bark beetle galleries in its colour and relief. Pollard heads:
  burrs 2.5-4.5 cm instead of 3.5-7 cm and the bark's furrows half closed rather than three
  quarters (the head had read as melted wax); coppice stool fissures and creases lighter.

- **Round 3 (docs, the lying log at 40 cm)**: moss on lying wood gets modelled shoots, tufts of
  four to seven tapered blades 0.6-2 cm long rising from the shell where the moss is thick
  (about 0.36 tufts per square centimetre on stout wood; LOD 0 and the path tracer, as the
  shell). Bare wood: fibres drawn in the shader, latewood ridges left by weathering, silver on
  the ridges and brown in the grooves, and a peel edge from noise at three scales stretched
  along the grain. Beech bark: lichen as lobed films, faint lenticels and folds.
- **Round 5 (docs: "a flat olive wall", "planks")**: the wall was the log's own beech bark, its
  albedo texture nearly one tone: crust colonies covered most of it and, past 1, clipped flat.
  Smooth bark's texture is now stored with headroom (the material's albedo, 1.35, gives it back),
  colonies are smaller and fewer, and grey mottling at two scales (about 4 and 1.5 cm) shows
  between them (contrast in the frame 8.5 to 12.8 grey levels). Smooth bark on dead wood keeps
  its grey; lying wood carries a quarter of the crust lichen (moss and algae win in the litter's
  damp); the moss film ahead of the cushions lies in drifts, with a few centimetres of full tint
  only at the shell's edge. Bare wood: the grain meanders a centimetre or two over half a metre,
  ridges run a hand to a forearm long, knots are 1.5-4.5 cm.

Round 6 (16 Nov, after docs declined the round 5 frames: no checks visible, the peel edge a
decal's edge, moss as flat round discs):
- Drying checks are geometry (`_grow_checks`): each check gets two walls of split wood from its
  lips down to a V floor below the stem's trough, a centimetre a step along the same wandering
  line, buff inside and dark at the floor, in the "wood" surface. The stem's 2.5 cm grid alone
  could only sink a soft groove.
- The peel edge on logs is a step (`_split_peel_edge`): vertices sink the full bark thickness
  or not at all (0.3 cm + 1.5% of the radius, a beech log's 5-10 mm), each triangle the edge
  crosses is cut along it into a bark piece and a wood piece, and a wall of red-brown phloem
  joins them. Cut points are shared by neighbouring triangles; parts keep their index ranges
  (LODs), new vertices take their part's lichen UV2 (`Builder.extra_part`). PT and raster
  match; the walls carry their own normals, so no terminator slits.
- Moss margins are ragged (weight noise at 13 and 31 per metre, plus or minus 0.13) and the
  shell is thin at its margin, thickening inward (15% of the cushion depth at the edge).
- Beech bark (17 Nov): the big soft blotches that read as cloud noise are toned down; the
  elephant-skin creases run across the stem in patches (noise stretched round the stem, its
  zero lines as sharp folds; isotropic noise drew wandering contour loops); faint lenticel
  striations across the stem in dashes; a millimetre grain so the skin reads matt and crisp at a
  hand's distance. PT keeps every feature. The log waits for the showcase forest for docs.

### 5p. Bark stories (16 Nov; Paulo's third pick)

What a living stem records of its life, each a few per stand, all modelled on the stem's own
tubes (bark pushed out or sunk, normals from the same field), so they hold under path tracing.
`bark_wounds` (0-4, default 1) scales how often each appears; 0 turns them off. Living and
newly dead standing trees only.

- **Lightning scars**: a strip of bark blown off down the stem, a few centimetres to a hand
  wide, spiralling with the grain from high in the crown to the ground, broken where the bark
  held, often with a split down the wood (Taylor 1974, *Lightning effects on the forest
  complex*; Minko 1975). Tall trees, oaks and conifers most.
- **Frost cracks**: a radial split up the lower stem of thin-barked broadleaves (oak, lime,
  beech, ash, maples, horse chestnut), opened again each hard winter, the callus either side
  building into frost ribs, raised lips a few centimetres high (Kubler 1988, *Frost cracks in
  stems of trees*; Shigo 1989). Stems over about 25 cm across.
- **Deer stripping**: red and sika deer peel bark upward with their lower incisors from smooth
  pole-stage stems at 0.5-1.8 m, leaving bare patches whose top edges end in torn tongues
  (Gill 1992, *A review of damage by mammals in north temperate forests*; Welch and Scott 1998).
  Ash, beech, spruce, rowan, willow, sycamore, lime, larch; stems under about 30 cm across.
- **Squirrel stripping**: grey squirrels strip patches on the upper stem and main limbs of beech,
  sycamore, oak, birch and larch of 10-40 years, at the crown base (Mayle et al. 2004, Forestry
  Commission Practice Note 4).
- **Callus**: every wound older than a couple of years is rimmed by woundwood rolls growing in
  from its edges, about 3 mm a year in width (Shigo 1989; Neely 1988, *Closure of wounds*).
- **Resin runs** (conifers): resin wells from a wound's edges and runs down the bark in drips
  and beads, amber when fresh, crusted whitish when old. Modelled as small glossy tubes and
  beads on a "resin" surface (LOD 0 and path tracing only).
- **Dead lower branches** on shaded conifers already come from `dead_branch_keep` (spruce 0.7,
  fir 0.5).

### 5q. Hard places (research, 16 Nov; on a card)

The last item of 5m. Sources: Pirk et al., *Plastic Trees* (SIGGRAPH 2012: a grown tree adapts
its skeleton to obstacles and light after the fact, bending and pruning branches); Palubicki et
al. 2009 (self-organizing trees: buds compete for space and light, so a crown grows away from
an obstacle); Li et al., *Rhizomorph* (2023: roots grown with soil and obstacles). In nature:
roots over rock follow the stone surface and stay thick where exposed (they turn bark-covered,
grey and ridged); trees on a cut bank or stream bank show their laterals arching down the face.
Wind-flagged trees keep branches only on the lee side (ice and sand abrasion kill windward
buds); krummholz is the low mat form at the tree line, a skirt of layered branches under the
snow line and a few flagged stems above it.

Plan (TreeGenerator only): an obstacle input (RockField3D.get_stones and the terrain height
and slope at the root collar) lets surface roots drape over stones and down banks; a
`prevailing_wind` direction and `exposure` turn on bud death on the windward side and, near the
tree line, the krummholz skirt. Stands at the tree line and on coasts set exposure from height
and distance to the sea.

As built, part 1 (16 Nov): `wind_exposure` (0-1) and `krummholz` (0-1); the wind blows toward
the mesh's +X and the mesh carries meta `tree_wind_flag` (Vector2: exposure, krummholz), the
species meta `wind_flag`, so placement turns +X down the prevailing wind instead of a random yaw.
- Flagging is done on the grown tree (`_wind_flag`, as Plastic Trees adapt a grown skeleton):
  above the shelter height (0.6 m, 1 m for krummholz) each limb facing the wind is killed (a dead
  stub 40 cm long stays) with a chance rising with exposure and height, and the rest are turned
  downwind about the stem by up to 80% of their bearing. Killing buds during growth instead
  starved the crown and left a bare whip leader standing 30 m over a low crown.
- Exposed stems stay short: the leader's shoots shorten with exposure above 1 m (a spruce at
  exposure 0.9 is 14.6 m at the age a sheltered one is 33.9 m).
- Krummholz: shoots above a lumpy dome (0.9-5 m high by krummholz, falling to a third at the
  rim 2.5-9 m out) run on level under the snow's surface, the leader is killed back at it, the
  crown cone is off, branch buds live down to 2 cm over the ground (layering), more basal stems.
- Forms FORM_FLAGGED (exposure 0.6-0.95 over the variants) and FORM_KRUMMHOLZ (0.8-1) in
  STAND_SUBALPINE past its closed forest: flagged spruce at -2.3 to -0.8 C annual mean, mats at
  -3.2 to -1.7 C.
- Open: the krummholz pine still reads as a table on a thick stem (redone in 5s).

As built, part 2 (16 Nov): `ground_slope` (radians, the ground falling toward +Z) and
`bank_erosion` (0-1); meta `tree_ground_slope` (Vector2: slope, erosion) so placement turns +Z
downhill and picks the variant nearest the terrain's slope. The collar and roots are built for
level ground and then sheared onto the slope near the soil (`_bank_shear`, fading out over the
lower stem; normals and tangents by the shear's Jacobian), every surface alike. Downhill roots
run 40-90% longer down the face, uphill ones a third shorter into it; erosion lifts the downhill
roots off the slope in an arch fixed at the stem and the tip. Bench: `--set=ground_slope:R` tilts
the ground plane to match. Open: root tips are blunt on flat ground too.

As built, part 3 (16 Nov): `root_stone` (metres, 0 none) sets a boulder of that half length
beside the stem on its +Z side, a fifth sunk, its inner edge under the stem's foot; the root
nearest +Z is turned onto it, lengthened to run past it and straightened. The boulder is a
RockGenerator mesh (glacial family, a 128-cell grid: about 43k triangles and 0.25 s a stone;
its close-up mode carried finer grain but cost 5 s and 230k triangles, drawn at every distance
since the soil surface has no LODs), found by class name so the vegetation
module keeps no build dependency on rocks; without the rocks module a noise ellipsoid stands in.
It is fitted to the planned extents (long axis along x, short axis up), coloured by vertex (the
rock's cavity term darkening the hollows, lichen crusts on upper faces, soil stain at the foot)
and drawn with the soil surface. The roots drape over it as roots grow over rock: near the soil
every surface is lifted by the rock's own top above the ground, read from a 32 x 32 grid over
its footprint (dilated one cell so a root never cuts a face), fading out above the root collar.
The drape runs before the bank shear, so a stone on a bank is sheared with the ground. The mesh
carries meta `tree_root_stone` (PackedFloat32Array: centre xyz, half extents xyz, mesh space
before any bank shear) so rocks and the floor keep clear of it.

Bank forms, part 4 (16 Nov), as the vegetation side asked (placement e015a841 picks the variant
nearest the ground's slope and turns +Z downhill; a level variant stays randomly turned):
FORM_BANK steps its variants through slopes 0, 0.35, 0.6 and 0.85 rad (0, 20, 34, 49 degrees),
erosion 0.3-0.9 rising with the slope, and one steep variant in eight over a boulder (0.35-0.6
m). Bank species are generated with at least four variants so every slope step exists.
- STAND_RIPARIAN: bank alder (density 0.5, within 12 m of water) and bank white willow (0.3,
  within 8 m), open-grown, up to canopy shade 0.55.
- STAND_BEECH: ravine sycamore on gorge sides past 25 degrees (density 0.4), after the
  Tilio-Acerion ravine woods of sycamore, ash and lime on steep stony slopes (Ellenberg 1988).

### 5r. Branch-scar chevrons (17 Nov; Paulo's pick, first of three)

**What it is.** On smooth bark (beech, hornbeam) and on birch every lost limb leaves a dark
chevron below its scar, the "Chinese moustache" of arborists (Mattheck and Breloer, *The Body
Language of Trees*, 1994; Mattheck, Bethge and Weber, *The Body Language of Trees: Encyclopedia of Visual Tree Assessment*, 2015). It is the
branch bark ridge (Shigo 1989), the fold of bark pushed up in the crotch: once the limb dies and
is shed, girth growth splits the fold in two arms and pulls them apart round the stem and down
its sides. The longer since the limb went, the longer and flatter the arms; a fresh one is a
steep inverted V close under the scar. On beech the fold is a grey-black, slightly raised and
creased strip; on silver and paper birch it is a black inverted V on the white bark. A forest
bole carries one from each self-pruned limb up its clear length.

**Why geometry.** The stem's bark grid is 2.5 cm round and 5-10 cm along at best, so an arm a
centimetre or two wide drawn in vertex colour blurs into a smudge. Per-tree marks also rule out
the shared bark texture. Each chevron is a small mesh laid on the stem, which is how decals are
done when the surface is not a single texture (mesh decals, as in UE and HDRP).

**As built.**
- `bark_chevrons` (0-2, default 1). Beech, American beech, silver and paper birch; red maple and
  rowan at half the density. Living, standing trees only.
- Planned from the main stem's shed laterals (scar and arms) and dead ones (arms only, under the
  stub), then filled up the clear bole below the crown to about 1.8 a metre on beech and 4 on
  birch, older and larger low down, never overlapping.
- Arm length: the scar's radius x (1.5 + 5 x age) plus a quarter of the stem's radius x age (age
  0-1 from the limb's year), at most 0.42 x pi x stem radius. Angle: 60 to 28 degrees below
  horizontal with age (birch 65 to 42), drooping further along the arm.
- The eye: an oval stretched round the stem, sunk centre, callus rim. The arms: seven columns
  across, edges tucked 0.5 mm under the stem surface so no edge floats at a grazing view, a fold
  0.7 mm proud on beech, creases across every 1.8 cm on beech.
- Laid on the stem's own triangles: a ray from the centreline out through triangles bucketed by
  (s, az). The recorded 2 cm bins are sparse where rings are 10 cm apart, and reading between
  them sank stretches of an arm under the bark.
- Bark material. UV continues from the stem vertex under the scar, so the bark texture carries
  on across the fold. Colour and lichen weight are taken from the bark underneath, darkened to
  x0.34 on beech and x0.11 on birch. Lichen is held off the fold by half. Skin is copied from
  the stem.
- LOD: a 2 cm part, kept at LOD 1 and dropped at LOD 2.

**Round 2 (17 Nov, docs review).** The first cut read as a thin drawn boomerang.
- The scar is now a knob: an oval dome about a fifth of its width high, with a dark pit at the
  top and its foot tucked under the bark. Every mark is drawn for a limb at least 1.2 cm thick,
  since smaller ones were lost in the bark texture.
- Beech arms are 3.6 cm wide or more and darken to x0.45 down the middle. They fade softly
  across their width and over the outer three quarters of their length.
- Colour: the mark's body takes the colour under the scar, and its edges take the bark's own,
  so no halo forms and the moss tint does not patch it.
- Birch arms are 1.5x longer and broader.
- Birch lenticels: each cell's dash can sit anywhere and is read from neighbouring cells too.
  Lengths run from a few millimetres to a hand, thick and thin, and denser in bands up the
  stem. A dash per cell at a jittered spot still read as a grid.
- Not chevron faults: the strand under the bench chevron is a beech crust-lichen rim (it goes
  with `lichen_cover` 0). The vertical line on the shaded side is the sun's terminator on the
  stem; it moves with the sun azimuth.

**Cost.** Forest beech: +10.7k triangles (+0.7%), about +70 ms to generate. Forest birch: +9.5k
triangles. Open-grown trees keep their low limbs and gain few or none.

**Check.** Raster against PT at 100% crop on a forest beech at 1 m: the arms, eye and creases
hold under PT.

### 5s. Krummholz mats as layered skirts (17 Nov; Paulo's pick, second of three)

**Problem.** The 5q mat read as a flat table of thick limbs on one stem, about a metre up,
with sparse needles hanging under it. Branch-collision deflection could also swing a 3 m
limb up 30 degrees, out of the mat.

**What a mat is.** Spruce and fir mats at the tree line (Marr 1977; Holtmeier 2009; Korner
2012) are cushions 0.5-1.5 m high, the height of the winter snowpack. They are domed over
their oldest stems and longest downwind, where snow lies deepest. Tree islands migrate to the
lee (Marr 1977; Benedict 1984). Limbs pressed to the ground under snow root and send up new
stems (layering; Cooper 1911; Laberge et al. 2000), so a mat stands on many stems. Everything
above the snow is killed back each winter by abrasion and drying (Hadley and Smith 1986). The
outside is a dense skin of short needled shoots over a frame of old wood. At the tree line,
needles stay 8-12 years (Reich et al. 1996).

**As built** (`krummholz` 0-1, unchanged API):
- **The dome** (`_krumm_cap`):
  - Height 0.75-3 m by `krummholz`, lumpy.
  - Falls to 12% at the rim, which is 2.5-9 m out.
  - Stretched 1.6x downwind (+X) and shortened 0.65x upwind with `wind_exposure`.
- **Limbs (order 2 and under)** run 30% of the cap height below the surface. When they reach
  that level they turn outward along the dome's own slope instead of running level.
- **At the surface:** stems' leaders and shoots of order 3 and up stop. Each tip lives on as a
  spur for up to 12 years and pushes again each year.
- **Layering:** from year 6, each year has a 50% chance (times `krummholz`) that a limb lying
  within 10 cm of the ground roots and sends up a stem. It roots at least 0.5 m from the root
  and 0.6 m from other layer roots, up to 20 times. A layered stem grows as a stem: apical
  control and extension of order 0, trunk gravitropism. Its first bud is fed by the limb.
- **Growth:**
  - Shoots are dwarfed to 0.55x length.
  - Twig droop is cut by 85%, since twigs lie flat under snow.
  - The mat grows over 1.9x the preset's years.
  - The pipe exponent is raised 0.6, so limbs are a few centimetres thick rather than logs.
  - Needles stay 4 more years, with 1.9x as many per shoot.
  - Every live shoot in the top 35% of the cap carries needles (the skin).
- **Collision deflection** on a mat swings limbs about the vertical only.

**Tree line by the growing season** (with vegetation's alpine plants, eec6cbff). Trees stop
where the growing season's mean falls to 6.4 C (Paulsen and Korner 2014), whatever the annual
mean, so an oceanic mountain gets its tree line in the right place.
- Every species gets `season_temperature_range` (7.2, 100) with a 0.8 C fade. This mirrors the
  alpine plants' (-100, 6.4) with 0.8 C, so the two cross-fade with no overlap and no bare
  band.
- Flagged spruce: 6.4-8.2 C.
- Mats: 5.2-7 C, among the alpine plants past the tree line (the tree species line).
- The subalpine belt's annual-mean cold end is gone, since the season rule sets it.
- Region bench check: no tree under 5.2 C, and nothing but mats under 6.4 C.

**Cost.** Mat variant at krummholz 0.9: 1.8-1.9M triangles against 0.57M before (more
needles and more nodes), and generation takes 2.8 s instead of 1.2 s. This is on the PC queue
for optimisation (needle count is the lever).

**Check.** Raster against PT at 960x540: the needle skin and frame hold under PT.

**Open.** From up close the mat is still porous, with ground showing between the sprays. A
denser skin would need either more shoots per node or a needle-card shell.

### 5t. Storm breaks and windthrow (17 Nov; Paulo's pick, third of three)

Living rigged trees break and are thrown at run time when the wind (`TreeWind3D`) loads them
past what their wood or roots hold; what breaks falls as PhysX bodies.

**Research.**
- **When wood breaks.** A part fails when the bending moment at its base passes the stem's
  strength, MOR x pi r^3 / 4 for a round section, less about 15% for knots (GALES, Gardiner et
  al. 2000; HWIND, Peltola et al. 1999). Green-wood MOR per species from the Wood Handbook
  (FPL-GTR-282, 2021): 33 MPa (willow) to 70 MPa (box).
- **When roots let go.** Overturning resistance is about C_reg x stem mass (Nicoll et al. 2006,
  Can. J. For. Res. 36:1871): about 145 N m/kg for spruce on free-draining mineral soil, less on
  wet or shallow soil, more for tap-rooted species. The crown's weight leaning out adds about
  15% to the moment (HWIND).
- **How strong trees are.** Trees build their wood to the loads they meet (Mattheck 1991,
  uniform stress; thigmomorphogenesis). Across sizes and species they fail at much the same wind:
  gusts of about 42 m/s (Virot et al. 2016, Phys. Rev. E 93:023001). Conifers, tall and with
  stiffer foliage (less streamlining), go first, which matches the big European storms (Lothar
  1999, Gudrun 2005).
- **How a stem breaks.** Green wood fails as a greenstick (van Casteren et al. 2012): the
  tension side tears long splinters, the compression side crumples and holds longest, so a
  snapped top folds over on its last fibres before it falls or hangs (a hanger).
- **How a tree is thrown.** The root plate hinges about its leeward edge and stands up on end,
  leaving a pit (5k; Schaetzl et al. 1989).
- **Blast.** NVIDIA Blast breaks a support graph of chunks joined by bonds, each bond with its
  own health, under damage programs (radial falloff here). The fork's `PhysXDestructible3D`
  takes prefractured assets with several surfaces per chunk and a health per bond
  (`PhysXBlastAuthoring.build_prefractured`, from the PhysX thread for this), so a limb chunk
  draws bark, wood and leaves and a twig joint breaks long before a trunk joint. A tree breaks
  at its branch joints, so its chunks follow the rig's bones rather than Voronoi cuts.
- **A fallen crown.** A windthrown broadleaf comes down on its crown and its lower limbs snap or
  bury under it until it lies on wood that holds; a conifer comes down on its whorls, which bend
  and break (Mitchell 2013, Forestry 86:147, on windthrow).

**As built.**
- **Strength.** `TreeGenerator.storm_kit` (on with `rigged`) stores per mesh the species' MOR,
  C_reg, root plate size and a unit fracture cap (meta `tree_storm`, `tree_fracture_cap`) and
  unskinned twins of every surface material (`tree_static_materials`; pieces broken off by
  touch use them too, so lichen bark keeps its shader). Each bone's strength is the wood's
  (MOR x pi r^3 / 4 x 0.85), held within 1-1.6 times a design load: 2.1 times the moment of a
  15 m/s wind (turbulence 0.2) at its peak (3 sigma gust plus the crest of the bone's own
  resonant sway, the solver's own terms), scaled by MOR / 50 MPa. The roots' anchorage is
  C_reg x 0.75 x stem mass held within 1-1.2 times the same design at the foot, scaled by C_reg /
  150. The band is there because the pipe model gives thin leaders far too weak and old stems
  far too heavy. Each part of each tree is 65-115% as strong as clear wood (knots, rot), each
  tree's roots 70-130%.
- **Load.** The solver adds up the wind's moment on each bone (its carried drag about its
  pivot) and on the foot while it poses the tree; only when gusts can pass `storm_min_wind`
  (12 m/s) and only for bones of at least `storm_min_radius` (3 cm).
- **Calibrated.** Worst of five trees over 30 s at turbulence 0.25: first breaks at 18-20 m/s
  mean for spruce, fir, pine and birch, 25 m/s for oak, beech and ash. Throws at 30-35 m/s on
  normal soil, 23-27 m/s with `storm_anchorage` 0.6 (wet soil). Nothing breaks at 10 m/s.
- **A break.** The part above the failed bone becomes a rigid body. It carries every surface,
  its wood's capsules, and its mass and centre of mass from the rig. It hinges on the leeward
  edge of the break, the compression side, and folds downwind. It tears free after 30-75
  degrees, or about a third of parts under 300 kg stay hung on their fibres. The wind pushes it
  as it falls: the drag of its crown with Vogel streamlining, on the wind relative to the part,
  for 8 s. Once asleep it is frozen to a static body.
- **The stub.** A limb tears out at its collar and goes whole. A stem snaps at a segment
  joint: that segment is pressed flat onto the joint's plane, so the stem keeps its girth up to
  the break (collapsed to a point it pinched into a cone). Fracture caps (5j's break, unit size,
  torn on the windward side) close the stub, following the bone below it, and the part's butt.
  The stub's colliders are rebuilt without the broken wood.
- **A throw.** The whole tree turns on a hinge at its root plate's leeward edge (three
  quarters of the plate's radius out, 10 cm under the old surface) with the plate's soil mass,
  and lies down with the plate on end. The plate (5k's roots, soil and torn root ends, some
  80-100k triangles) is built when a tree is thrown (`TreeGenerator.build_storm_plate`), in
  about 0.2 s, not stored on every tree.
- **For terrain and plants.** Signal `storm_damage(position, radius, uprooted, pit)`; a throw's
  pit has a world centre, radius (0.9 of the plate's) and depth (1.6 plate thicknesses,
  0.35-0.95 m), as `tree_root_pit`.
- **Bench.** `docs/bench/trees --storm` (storm.gd): a clip of a storm on a few trees, or checks
  with `--test`.

### 5u. Gorse and broom (17 Nov; shrubs agreed with vegetation, Atlantic heath first)

Woody shrubs over about 50 cm belong to the trees (forest-floor split); vegetation places them.
Gorse and broom come first, then gum rockrose, kermes oak and lentisk, then the dwarf fan palm.

**Research.**
- **Gorse (Ulex europaeus).** A shrub to 2-2.5 m (4 m left to grow), erect to prostrate; main
  stems erect or spreading, densely branched in younger parts but bare at the base when old (NZ
  Flora, Webb et al. 1988; Trees and Shrubs Online, Sutton 2022). Adult leaves are rigid,
  spine-like phyllodes 5-12 mm; the shoots end in rigid, deeply furrowed spines 15-30 mm
  (primary to 4 cm), branched, with secondary spines to 12 mm at their base; young twigs and
  spines glaucous green with grey hairs. Seedlings have three hairy leaflets. Flowers solitary
  in the spine axils, clear to golden yellow, 13-20 mm, pea-shaped (banner 14-21 x 10-15 mm),
  with a hairy greenish-yellow calyx of 11-16 mm nearly as long. Pods 10-20 mm, villous, dark
  brown to black. In flower almost all year with a spring peak (TSO); in Britain the flush is
  February to May. Active life 15-20 years; old plants lose vigour, grow top-heavy and keel over
  under their own weight (BSBI Fermanagh); flowers from 2-3 years; resprouts after cutting and
  fire. Frequent on maritime cliff tops and grey dunes.
- **Broom (Cytisus scoparius).** A much-branched shrub to 2-2.5 m (3 m), erect, of straight
  green twigs, about five-angled, silky when young; deciduous, but the green branches keep an
  evergreen look in winter (Bean). Leaves trifoliate and stalked low on the shoot and on older
  twigs, 1-2 leaflets and stalkless near the tip; leaflets 4-16 mm, elliptic to obovate; often
  shed by summer, so the plant stands leafless most of the year (HerbiGuide). Flowers solitary
  or paired from the joints of year-old shoots, golden yellow, 16-25 mm, calyx glabrous and a
  quarter the corolla's length; May to June in Britain. Pods black, oblong, 15-60 mm, hairy on
  the margins. Lives 10-15, up to 20 years; with age its branches lean outward and thin,
  opening the centre (GOERT factsheet). Seeds from 2-3 years; regrows from the root crown.
- **Cushions.** On exposed coasts salt wind kills buds on the windward and upper surface, so
  heath shrubs grow as dense, clipped cushions (the krummholz mechanism, 5q-5s, at heath scale):
  western gorse and kermes oak under 50 cm.

**Plan.**
- `PRESET_GORSE`, `PRESET_BROOM` in the growth model: many stems from the base, short
  internodes, high basitony; gorse dense and rounded, broom erect and fastigiate.
- **Spines as geometry.** Gorse's foliage is spines: each leaf slot carries a furrowed
  three-sided spine tapering to a point, glaucous green, with 1-3 short side spines near its
  base on the primary ones. Seedlings and the youngest age class keep a few trifoliate leaves.
- **Green stems.** Broom's twigs stay green and five-angled for years, browning only on old
  wood at the base; gorse's young shoots are green too. A twig colour per species blends
  into the bark by twig age.
- **Flowers and pods as geometry**, from the season dictionary (`day_of_year`): pea flowers (a
  rounded banner over a boat of wings and keel, calyx below) along the shoots; gorse's flush
  February to May with a few open all year, broom's May to June along the year-old switches;
  then pods (gorse June to August, broom July to September, black, twisted open after).
- **Age classes**: young (3-4 years, dense to the ground), mature (8-10), old (15-20: bare grey
  dead stems in the centre, stems leaning out, gorse top-heavy).
- **Forms**: a single bush; a thicket (several plants grown into one, wider than tall, flat
  top); a wind-pruned cushion under 50 cm (gorse).
- **For vegetation**: one static call per species returning a `PlantSpecies` with its meshes,
  footprint, canopy radius and far colour and no climate or terrain rules (`apply_scrub_rules`
  and placement stay with vegetation).

**As built.**
- `PRESET_GORSE` (31) and `PRESET_BROOM` (32): many basal stems with a wide spread; gorse with
  weak negative lateral gravitropism and pipe exponent 2.8 so it rounds out instead of making a
  small tree, broom erect with steep 28 degree branching.
- Gorse foliage is spines only: each leaf slot draws a three-faced prism tapering to a browned
  point, a third of them with 1-2 side spines. Broom keeps three-lobed leaves on green twigs.
  Twigs blend green into bark by radius (gorse 2.5-6 mm, broom 6-16 mm).
- Pea flowers (calyx, two wings, a three-station banner) and pods on the shoot tips, kept by
  every LOD. `get_bloom(preset, day)`: gorse ramps up 20-60, down 120-160 with 4% all year;
  broom 120-178. Pods follow (gorse 140-250, broom 165-290), ripening black and twisting open.
  `get_leaf_season` adds `flowering` and `flower_color` for both.
- Forms: `FORM_THICKET` (2.5 times the stems, envelope-clipped top 1.3 m gorse / 1.7 m broom)
  and `FORM_OLD` (1.5-1.7 times the years, shaded centre shed and kept dead, stems leaning out).
  The cushion is the krummholz cap at heath scale (0.47-0.54 m).
- `TreeGenerator.make_shrub_species(preset, ShrubForm, variants, season)` for vegetation:
  `SHRUB_YOUNG`, `SHRUB_MATURE`, `SHRUB_OLD`, `SHRUB_THICKET`, `SHRUB_CUSHION`, rules reset,
  far colour gold by bloom. Bench: `-- --shrubs [--test]`.
- **Cost (cloud, lavapipe build):** gorse is heavy because every spine is geometry: young 96k
  triangles, mature ~2.0M, cushion 1.7M, thicket 2.5M, old 3.6M; broom mature 108k, old 500k.
  Generation 0.5-6 s per plant. On the PC queue: GPU cost and a spine LOD (cards past ~15 m).
- Raster vs path-traced 100% crop (gorse in flower, April): spines and flowers both present;
  PT is darker inside the bush from occlusion, no detail lost.

### 5v. Gum rockrose, kermes oak and lentisk (17 Nov; Mediterranean scrub with vegetation)

**Research** (Flora iberica, Castroviejo et al. 1986-, vols. 2 Cistaceae, 2 Fagaceae, 9
Anacardiaceae; Blondel et al. 2010; Correia and Ascensao 2017 on Cistus ladanifer's summer
leaves; Gratani and Varone 2004 on kermes oak and lentisk leaf traits; Jordano 1989 on lentisk
fruiting):
- **Gum rockrose** (Cistus ladanifer): 1-2.5 m, few erect, little-branched stems from the base,
  open and leggy, sticky with labdanum; short-lived (15-20 years) and fire-seeded, old plants lanky
  with dead stems. Leaves opposite and sessile, linear-lanceolate 4-8 (10) cm by 0.6-2 cm,
  three-veined, dark glossy green and varnished above, grey felted below. Flowers solitary at the
  tips of short side shoots, 5-10 cm, five crumpled white petals, often a maroon blotch at each
  base, a boss of yellow stamens; April to June, each lasting a day. Capsule globose, 10-celled,
  1-1.5 cm, woody, standing grey-brown through summer. Summer: the big spring leaves fold along
  the midrib, droop, dull and partly fall; small summer leaves stay at the tips.
- **Kermes oak** (Quercus coccifera): 0.5-2 m, rarely a small tree; a dense, intricately branched
  mound that suckers into clonal thickets; browsing and fire keep it as a hard cushion 30-50 cm
  (garrigue "coussin"). Leaves 1.5-4 cm, oval, stiff and leathery, the margin wavy and spiny
  toothed (holly-like, 3-6 spines a side), glossy bright green both sides, glabrous; petiole
  1-4 mm. Acorns ripen in the second autumn (September-November), 1.5-3 cm, in a cup of stiff,
  spreading, spine-tipped scales. Bark grey, smooth.
- **Lentisk** (Pistacia lentiscus): 1-3 (5) m, a dense, rounded, many-stemmed evergreen mound,
  sea-pruned to a flat-topped hummock on cliffs. Leaves paripinnate, no end leaflet: 2-7 (usually
  3-5) pairs of elliptic, leathery, glossy dark leaflets 1-5 cm on a narrowly winged rachis, young
  shoots and rachis reddish. Dioecious; tiny reddish flowers in dense axillary spikes March-May;
  female plants carry clusters of 4-5 mm drupes, red in late summer, black when ripe
  (October-January). Smells of resin (mastic).

**Plan.**
- `PRESET_GUM_ROCKROSE`, `PRESET_KERMES_OAK`, `PRESET_LENTISK`; `apply_scrub_rules` stays
  vegetation's, `make_shrub_species` serves all three with the five forms (the cushion for kermes
  oak and lentisk; rockrose has no cushion and gives its young form).
- Leaves as geometry: rockrose long, narrow, folded along the midrib, glossy; kermes oak small
  pointed-lobed blades (the spiny teeth in the outline) with a wavy margin; lentisk paripinnate
  without the end leaflet, elliptic leaflets on a red rachis.
- Rockrose flowers (five crumpled petals with the blotch, yellow stamen boss) and standing
  capsules from `get_bloom`; summer leaf fold by day of year.
- Kermes oak acorns in spiny cups, autumn; lentisk drupe clusters red to black on female plants.

**As built.**
- `PRESET_GUM_ROCKROSE` (33): four erect stems, opposite leaves 6.5 cm by 1.4 cm, dark and
  glossy; flowers 7 cm across as geometry (five crumpled petals on a 5 x 5 grid each, the maroon
  blotch on seven plants in ten, a yellow stamen crown, three sepals), `get_bloom` 90-175;
  ten-ridged capsules after, green to brown to grey and split. Summer fold days 175-300: leaves
  shrink 40%, grey and a third fall. Old plants are leggier, not taller. No cushion: its
  `SHRUB_CUSHION` is the young plant.
- `PRESET_KERMES_OAK` (34): dense, zigzag, suckering mound leafy to the ground; pointed-lobed
  leaves 2.8 cm (the spiny teeth in the outline); acorns in cups ringed by spreading spines,
  green to brown, days 180-345.
- `PRESET_LENTISK` (35): dense rounded mound; paripinnate leaves (four pairs, no end leaflet) on
  a winged red rachis, entire elliptic leaflets from their own alpha cut; female plants (half,
  by seed) carry drupe clusters, red then black, days 210-45.
- Far colours: rockrose whitens by bloom x 0.3. Thicket tops 1.8 / 1.4 / 2.0 m. Cushions
  0.48 m (kermes oak), 0.54 m (lentisk).
- Cost (mature): rockrose 217k triangles, kermes oak 620k, lentisk 650k; old forms 0.5-1.5M.
  Generation 0.6-1.4 s.
- Crown parts' normals were right-handed while Godot's front faces wind clockwise, so with
  double-sided shading every cone, berry, key, catkin and flower was lit from behind in raster
  (white rockrose flowers drew grey, gorse flowers flat). `close_part` now takes (c - a) x
  (b - a). The path tracer was unaffected.
- Shape pass: kermes oak and lentisk with 12-14 stems fanned to 80 degrees, weak trunk
  gravitropism, no light floor and leaves kept three years, so the mounds are leafy to the
  ground; rockrose with six stems fanned to 38 degrees. Rockrose flowers on 4.5 cm stalks over
  the shoot's last leaves. Lentisk leaflets 2 cm on a rachis 0.42 leaflet a pair.
- Bank trees in one-variant stands (6411eda3): one slope variant, not four.
- Docs review (rockrose): flowers rebuilt as five separate petals on narrow claws, cupped and
  crumpled with waved margins, a bold maroon blotch to 40% of each petal, 5-10 cm across and
  tilted up to 40 degrees; a flowering bush mixes open flowers (a fifth of the tips at the
  peak), buds (more early in the season) and spent flowers (sepals round the green ovary).
  Lentisk denser: 7 cm internodes, 24 leaves an internode kept four years, stems fanned 60
  degrees.

### 5w. Dwarf fan palm (17 Nov; Mediterranean scrub with vegetation)

**Research** (Flora iberica vol. 18, Arecaceae; Merlo et al. 1993 on its growth in the wild;
Herrera 1989 on its fruit; Riffle and Craft 2003, An Encyclopedia of Cultivated Palms; Tomlinson
1990, The Structural Biology of Palms, for the leaf and its plication):
- Chamaerops humilis, Europe's only native palm, on the Algarve's coastal limestone and sandy
  scrub. A clump of several to many stems suckering from the base; trunks 0-2 m in the wild
  (often stemless where grazed, burnt or wind-cut), 15-25 cm across, clad in the persistent
  bases of old leaves and their brown fibre; dead leaves hang a while beneath the crown.
- Leaves palmate, 10-20 to a crown: the blade 40-80 cm across, a fan of three quarters of a
  circle split into 10-30 stiff, induplicate (V-folded, the fold up) segments, divided for two
  thirds or more of their length, the tips split in two; green to glaucous grey-green. Petiole
  20-100 cm, armed along both edges with straight, forward-pointing spines up to 2 cm.
- Dioecious: short, dense yellow inflorescences among the leaf bases, March to June; fruits
  1.5-4 cm, yellow-green then orange-brown, ripe September to November.

**Plan.** `PRESET_DWARF_FAN_PALM`, outside the shoot growth model: stems and petioles laid as
nodes (so bark, wind bones, colliders and LODs work as for every tree), each blade a fan of
V-folded segment blades on the leaves surface, spines as geometry along the petioles, leaf-base
stubs and fibre on the trunks, a skirt of dead leaves on old stems, flowers and fruit among the
leaf bases. Forms through `make_shrub_species`: young (one stemless crown), mature (a clump of a
few short trunks), old (taller trunks, more skirt), thicket (many stems), cushion (stemless,
short petioles, under 60 cm).

**As built.** `_grow_palm` lays the clump: stems on a ring 0.16 m x sqrt(stems) across, trunks
in 15 cm nodes (none on young plants and cushions; mature clumps mix stemless crowns with trunks
to about a metre, old ones to 2.5 m), leaning out at the foot. Each crown has 6-19 live leaves
on golden-angle rolls, the young upright in the middle and the older spread near level, and old
stems 3-7 dead ones hanging down; each petiole is three nodes, so the wind bones bend it.
`_palm_geometry` draws the blade as 14-22 segments free almost from the stalk tip (a small
pleated hastula joins them; a first try fused them to a third of the radius and the leaves read
as saw blades), each V-folded with the margins up and the two halves shaded apart so the crease
shows, stiff, the tips bending a little, the outer ones shorter. Dead leaves half close and hang
straw-brown against the trunk just below the crown; crowns too low for that have shed them.
Petioles are green tubes on the leaves surface (the bark skips them), rewound since _tube winds
for the bark's later flip (inside-out faces sparkled under the path tracer), with forward
spines on their lower two thirds; trunks carry rings of leaf-base stubs and hanging fibre. LODs: a fan's
parts are sized so stochastic pruning keeps them whole and unscaled (grown about its base a fan
reached metres out); spines are LOD 0 only. Flowers and dates (crown parts, kept by the coarser LODs): dioecious by seed;
one to four short branched inflorescences a crown between the leaf bases, March to June
(`get_bloom`), sheathed in a bract, male plants' rachillae crowded with 3-4 mm yellow flowers
(octahedra, 8 triangles), packed into short golden lumps, female plants' fewer and greener; female plants then carry clusters of
smooth globose to oblong dates 1.6-2.8 cm, green from June, yellow, then red-brown by November,
the stalks bowing under them, a few left into January. The trunk is dark fibre: four to six
strands a leaf-base stub hang or lie across it as the sheaths' net. Segments are 2-3.5 cm wide,
16-23 a leaf, the tips drooping (docs, 17 Nov). Mature 1.3 m,
39k triangles (46k in flower); thicket 150k (on the optimisation list: segments could merge in far LODs).

### 5x. Next round after the shrubs (research, 17 Nov; on a card)

The heath and scrub list is done (5u-5w). What the trees still lack, in order of what a scene
would show first:

1. **Mediterranean trees over the scrub.** The scrub has no canopy of its own. The Algarve and
   Iberian coast species (European Atlas of Forest Tree Species, 2016: chapters on Pinus pinea,
   P. pinaster, Quercus suber, Q. ilex, Olea europaea, Ceratonia siliqua; Flora iberica):
   - *Stone pine*: a flat umbrella crown on a clear, often leaning stem, the lower limbs shed
     early; long needles in pairs, big glossy cones three years ripening; deep red-brown plates.
   - *Maritime pine*: tall, open, crooked on dunes and cliffs; thick deeply furrowed bark; long
     stiff needles, clustered cones that stay shut for years.
   - *Cork oak*: a broad, open evergreen crown of grey-green leaves; the cork bark, thick and
     corky-ridged, and on harvested trees the stripped stem in red-brown, darkening over the
     nine years to the next cut, the year painted on (a story the bark builder can tell).
   - *Holm oak*: a dense, dark, rounded evergreen crown; small holly-like leaves (spiny when
     young or browsed), grey-felted beneath; smooth then finely checkered grey bark.
   - *Wild olive* and *carob*: low, gnarled, many-stemmed small trees on limestone, the olive
     silver-backed and twisted, the carob dark glossy pinnate with long brown pods.
   Built as presets through the shoot growth model (as the scrub); stone pine needs a new
   crown habit (apical dominance lost early: the flat umbrella), cork oak a new bark kind.
2. **Triangle diet for shrubs and far LODs.** Thicket forms reach 150k (palm) and more (gorse
   spines); the open white willow is 10M. Merging a palm fan's segments and a gorse shoot's
   spines in the coarser LODs, and capping per-form budgets, measured in the cloud (triangles
   and memory; GPU cost stays on the PC queue). Helps scene's forest stay under 15 GB.
3. **Coppice cutting in stands.** 5l's "not yet": coppice-with-standards (hazel stools under
   scattered oak standards) cut in coupes of different ages, so a stand shows fresh-cut
   stools, young regrowth and full-grown poles side by side.

#### 5x.1 Mediterranean trees: plan per species

Same route as the scrub: presets through the shoot model, `make_tree_species` forms, habitats
from the Atlas chapters, crown parts in season. What each needs beyond parameters:

- **Stone pine (Pinus pinea).** Young trees are globular; from 20-30 years the leader loses
  dominance and the upper limbs grow out and up to an even height, so the crown flattens into an
  umbrella 10-20 m across on a clear, often leaning bole; lower limbs die and are shed early
  (Mutke et al. 2005, crown architecture of stone pine; Atlas 2016). Needles 10-20 cm in pairs,
  tufted at the shoot tips. Cones 8-15 cm, glossy chestnut, ripening over three years (all three
  ages on a tree). Bark: deep fissures between long flat orange-red plates. New: an
  apical-control parameter that drops with age (the umbrella), flat-topped light from above.
- **Maritime pine (P. pinaster).** Tall (to 30 m), open, often crooked and wind-leaning on dunes
  and cliffs; the thickest, most deeply furrowed bark of the European pines, dark red-brown
  plates in black fissures; needles 15-25 cm, stiff; cones 10-20 cm in whorls of 2-5, persisting
  shut (serotinous) for years. Existing pine plumbing plus the coastal flagging (5q).
- **Cork oak (Quercus suber).** A broad, open, irregular crown on a short stem with heavy
  sinuous limbs; leaves 3-7 cm, dark above, grey-felted below, lightly toothed. The bark is the
  story: virgin cork 5-10 cm thick, deeply ridged, pale grey; harvested every 9-12 years from
  the stem and main limbs to 2-3 m (Portuguese law: 9 years minimum), the stripped surface
  first rust-red, darkening to brown-black, with the harvest year's last digit painted in white
  (Pereira 2007, Cork: Biology, Production and Uses). New: a cork bark kind (relief from the
  bark builder's furrow model, much deeper and rounder) and a `cork_stripped_years` input that
  sets the strip height, colour and a thin regrown layer.
- **Holm oak (Q. ilex).** Dense, dark, rounded evergreen crown, often multi-stemmed from
  coppice; leaves 2-7 cm, very variable: spiny-toothed and holly-like on young and browsed
  shoots, entire on the upper crown, grey-felted beneath (Atlas 2016). Acorns in scaly cups.
  Bark: fine square checks, dark grey. New: leaf margin by height and browse (the kermes oak's
  spiny leaf already exists).
- **Wild olive (Olea europaea var. sylvestris).** A shrub or small tree, 2-8 m, many-stemmed,
  the old stems fluted and twisted with hollows; narrow leaves 3-6 cm, dark grey-green above,
  silver-scaled beneath, so the crown flickers silver in wind; small black drupes in autumn.
  New: a twisted, fluted old-stem relief (the bark builder's buttress flutes, wound around).
- **Carob (Ceratonia siliqua).** A broad, dense, dark evergreen dome to 10 m; paripinnate
  leaves of 2-5 pairs of round, leathery, glossy leaflets (the lentisk's leaf plumbing); pods
  10-25 cm, flat, dark brown, hanging all year from old wood (cauliflory). Female, male or
  hermaphrodite trees.

Order: stone pine and cork oak first (they define the Algarve skyline and carry the two new
stories), then holm oak, wild olive, maritime pine and carob. Each: research note, build, the
five forms, raster and PT frames, docs.

#### 5x.2 Triangle diet: measured (17 Nov, cloud, LOD 0, day 200)

| Species | Young | Mature | Old | Thicket | Cushion |
|---|---|---|---|---|---|
| Gorse | 127k | 2.53M | 4.14M | 3.00M | 2.24M |
| Broom | 45k | 350k | 823k | 349k | 244k |
| Gum rockrose | 28k | 286k | 398k | 353k | 28k |
| Kermes oak | 55k | 930k | 2.05M | 1.11M | 449k |
| Lentisk | 74k | 1.27M | 2.14M | 1.61M | 489k |
| Dwarf fan palm | 19k | 44k | 70k | 154k | 21k |

Where it goes (mature, by surface): gorse needles (spines) 602k, bark 206k, crown parts 1.72M
(the summer pods: the flower and pod budget is 60,000 a bush); kermes oak bark 536k, leaves
131k, crown parts 261k (acorns); lentisk bark 929k, leaves 159k, crown parts 178k (drupes).
So the bulk is many small repeated parts and thin twigs, not leaves. Techniques:
- **Instanced crown parts** (Unreal 5.7 PVE assemblies; SpeedTree's mesh fronds): one pod,
  flower, acorn or drupe mesh per species and season, placed by transforms, instead of merged
  copies. In Godot a MultiMesh per part kind, skinned to the wind bones by the instance's bone
  index (the fork's PT reads MultiMesh BLAS instances; the known limit is per-instance colour
  past 12 bones). Memory falls by the repeat count (gorse pods: 1.7M to a few thousand plus
  transforms); triangles drawn stay, so the PC bench decides draw cost.
- **Thin-twig bark**: 3 sides under 4 mm and collinear rings merged (shrub twigs are straight
  between nodes); the far LODs already drop them.
- **Spines in the near range only**: gorse spines and the palm's petiole spines go to the near
  mesh (`near_leaf_range`), the far mesh keeps a needle tuft per shoot (Cook 2007 pruning with
  area kept).
- **Palm fans in far LODs**: segments merged in threes (one blade per three, its width summed).
- **Per-form budgets**: a ceiling per form (e.g. 400k mature, 150k cushion), the generator
  thinning crown parts first.
Measured in the cloud (triangles, memory, generation ms); GPU cost on the PC queue.

As built, step 1 (17 Nov): gorse and broom flowers and pods had no LODs at all (the crown parts
before `big_end` stay whole), so a mature gorse drew its 1.7M crown-part triangles at every
distance. Each flower and pod is now its own part, sized negative, and the crown parts' LODs
thin those stochastically as leaves do (survivors grown by 1/sqrt(keep) about their base, Cook
et al. 2007); other parts before `big_end` stay whole. Mature gorse, day 200: 1.72M, then 858k,
258k, 69k; broom 250k, then 108k, 32k, 9k. LOD 0 is unchanged (a row at 5-30 m renders pixel
for pixel the same with `--lod_threshold=0`), and the path tracer reads LOD 0. At 30 m and fov
20 the first level swaps in over a few dozen pixels at the crown's edge, the gold kept. Also: a
palm's promoted far mesh kept LODs no coarser than its new base, which the renderer rejected
with an error; those are dropped. `--shrubs` takes `--gap=` and `--lod_threshold=`.

Step 2 (17 Nov): where the rest goes. Bark of a mature lentisk by tube radius: under 1 mm 696k
triangles (58,000 leaf twigs of 12 each), 1-2 mm 77k, 2-3 mm 72k, 3-5 mm 43k, 5-10 mm 22k,
over 1 cm 19k; kermes oak 309k of 537k under 1 mm. Those leaf twigs are a fraction of a pixel
past the near range, so a far mesh with its near mesh split off (8d) leaves them out
(`FAR_TWIG_RADIUS`, 1 mm) and drops their vertices; the near mesh keeps every one. Far bark,
mature: lentisk 929k to 233k, kermes oak 537k to 227k, broom 74k to 38k, gum rockrose 83k to
68k; broadleaf trees' far meshes lose the same sub-millimetre twigs where they have them.
Path-traced check as 8d (a mature lentisk at 60 m, 16 spp, 960x540, 75 degree lens): far vs
near mesh, crown box mean 111.06 vs 111.07, a few pixels differing at the crown's edge (the
near mesh is its own build of the variant). And every LOD level is now checked against the base
it simplifies just before the surface is made: a base that lost its plain tubes, far twigs or
LOD 0 leaves after the levels were cut left levels no smaller than it, which the renderer
rejected with "index_count >= index_array_len" (14 times per scrub and heath setup); none now
over all six shrubs, five forms, two seasons. `--shrubs --near` shows the near mesh.

Step 3 (17 Nov): gorse splits too. Its spines are needles (`PARAM_NEEDLES`), which kept it out
of 8d's split, but they thin stochastically as leaves do; a scrub shrub now splits whatever its
leaves, and a far mesh's flowers and pods start at their first thinned level as its leaves do.
Mature gorse far mesh, day 200: bark 206k to 70k, spines 603k to 301k, flowers and pods 1.72M to
858k (2.53M to 1.23M); broom's flowers and pods 250k to 108k. The near mesh is unchanged (3.21M
in April). PT at 60 m in flower, far vs near: crown box mean 129.42 vs 129.38, 20 pixels differ.

#### 5x.3 Coppice cutting in stands

Coppice-with-standards (Rackham 2006, Woodlands; Buckley 1992, Ecology and Management of
Coppice Woodlands; Harmer and Howe 2003, The Silviculture and Management of Coppice
Woodlands): hazel (or ash, sweet chestnut, hornbeam) stools cut to the ground every 7-15 years
(hazel), the wood divided into coupes (compartments, often 0.5-2 ha) cut one a year in turn, so
neighbouring coupes differ by years; 20-50 oak standards a hectare over the underwood, of
several ages (a few felled at each cut). In a fresh coupe: stools of cut stubs with sawn faces
and the brash (cut twigs) in heaps or rows, ground flora flowering in the new light (bluebells,
wood anemone: vegetation's), standards suddenly in full light. Two to five years: dense
regrowth of straight rods 1-4 m. Mature coupe: poles 6-10 m, closed canopy, a dark floor.
Lapsed coppice: overstood stools of thick, leaning poles. Plan: `STAND_COPPICE_STANDARDS` takes
a coupe map (age by cell from a Voronoi of coupes with a cutting order) and places stools of
`FORM_COPPICE` at that cell's years since cut (5l's cut model), standards of `FORM_STAND` oak
at a set density with ages mixed, brash heaps and cut faces in fresh coupes (deadwood plumbing,
5o), and hands vegetation the coupe light map.

### 5y. Stone pine (17 Nov; Paulo's pick, Mediterranean trees first)

Research (Gymnosperm Database, Pinus pinea, 2026; Surovy, Ribeiro and Santos Pereira 2011,
Observations on 3-dimensional crown growth of stone pine, Agroforest Syst 82; Mutke et al. 2012
on cone yield and crown form):
- 12-25 m (to 35). Conic only its first two years, globose and shrubby young, then domed, and
  with age a very broad, low-rounded umbrella. Main limbs rise at 30-60 degrees with tips swept
  up to vertical; the leader gives up dominance, so the crown is many equal limbs ending at one
  height. The form carries the heavy cones on many branches, in full light, on top.
- Shoots uninodal (one whorl a year). Needles in pairs, 10-18 cm (to 28), mid-green, 1.5 mm
  thick, kept 2-4 years: dense tufts at the shoot tips, bare wood behind. Juvenile needles
  single, glaucous, 1.5-4 cm, for 3-10 years (saplings).
- Cones 8-12 cm (to 15), 5-11 cm broad, broad ovoid to globose, on short stout stalks; green,
  then shiny chestnut-brown, ripe in April three years after pollination (the longest of any
  pine), so a tree carries three cohorts: small first-year conelets, green second-year cones
  and full-size ripe or opening cones; open on ripening or within a year.
- Bark thick, plated, deeply fissured, red-brown to orange with blackish plate edges.
- Hardy to about -12 C; coastal sands and dunes, Iberia's south and west, Italy, Turkey.

How the generator gets there: the existing pine plumbing (whorled phyllotaxis, needles, plated
bark, cones as crown parts, resin) with an apical dominance that falls hard with age
(`PARAM_APICAL_DOMINANCE_OLD` well under Scots pine's), upturned limb tips (negative
gravitropism), heavy self-pruning of the lower crown (shed ratio), long needles in tip tufts,
the three cone cohorts by day of year, an orange-red tint on the plated bark, and a warm,
dry habitat. Then the five forms, raster and PT frames, docs.

As built (17 Nov): `PRESET_STONE_PINE`. Parameters alone gave a vase of limbs from the ground
(open-grown, every low lateral stays lit) or a sparse candelabra. What made the umbrella:
- a strong leader young (lambda 0.78) falling to 0.32 over 35 years, laterals near the leader's
  vigor (apical control 0.95), wide branch angle (65 degrees), little upward bend (0.03), no
  crown cone, 0.35 m internodes, five buds a whorl, 160 needles of 14 cm an internode;
- a crown lift in `_shed`: open-grown too, limbs off the stem below 0.42 of the tree's height
  (ramping in from year 8 to 35) die, as they do under the dome in the real tree.
Mature open-grown (60 years): 15.5 m tall, crown 15.1 m across, crown base 11.9 m; 4.0M
triangles. Stand-grown: 21.6 m, narrow, crown base 15.8 m. Cones: three cohorts by a hash per
tip (conelets 1-2 cm, green second-year cones 3.5-5 cm, third-year 8-12 cm turning chestnut
from February, opening from April), on the sunlit top. Plated bark in red-brown. Raster vs PT
at 100% crop: the same limbs and needle tufts, PT's crown a little darker and fuller. Deadwood
classes and logs come from the shared plumbing.

Docs review fixes (18 Nov): the crown was pale and lacy, the limbs a fan of thin straight
branches, the trunk slender, the bark plates cartoonish, the cones dots.
- Crown: four buds a whorl, 220 needles of 14 cm an internode kept three years, darker green
  (0.07, 0.16, 0.05) and a little self-shadowing, so the top is one dark flat mass.
- Limbs: more branch noise (13) and less upward bend, so the limbs wander; pipe exponent 2.05
  and a root flare give a stout bole (about 0.9 m across at 60 years).
- Bark: its own kind, `BARK_PINEA`: long vertical plates (6 x 2 cells, warped like the plated
  kind), grey-brown faces and orange-red fissure floors carried in the material's headroom
  (3.0), so the plate faces stay grey; lichen cut to 0.08. Fixing it found that the bark
  headroom was passed linear to an sRGB albedo: smooth bark (beech, hornbeam) drew about 1.5
  times too bright; now encoded.
- Cones: three cohorts at the shoot tips, set back behind the needle tufts and drooping; the
  third-year cones 9-13 cm, green to chestnut by day of year, open from April.
- Limb count: still more limbs than the real tree's handful. Thinning the weaker scaffolds in
  `_shed` destabilised the form (the leader took over or the crown collapsed); left for a
  growth rule that splits the leader into a few limbs.
Docs round 2 (18 Nov): the plates were pale slabs in a grid of orange steps lined up across the
trunk (bricks), the grid showed at range, and the crown edge was still lacy.
- Bark: the plate cells are stretched along the stem (`bark_voronoi` p_ax 1.8, 5 x 3 cells a
  tile); walls to the cell above or below stay shut half the time (p_vmerge 0.5), so plates run
  one to three cells long and end at different heights from their neighbours; side walls open
  wide and deep, the few cross breaks stay narrow. Fissure floors darker red-brown, plate depth
  4.5 mm, and some plate faces warmer where the outer layers flaked. Noise zero lines (as on
  the furrowed kind) were tried first: straight stripes that repeat with the tile.
- Crown: five buds a whorl again and needles at the long end (17 cm, 260 an internode): the
  tufts close up into one cushion.
- Limbs: the leader split (5z) replaces the fan.
Mature open-grown: 15.9 m tall, crown 15.6 m across, crown base 11.1 m; 4.3M triangles (3.0M
needles, thinned by the LODs).

### 5z. Leader split (decurrent crowns)

Docs held the stone pine's site shot on its limbs: a fan of many equal limbs from one point
reads as an acacia. The real tree's leader gives up at a few metres and a handful of thick
limbs take the crown, each dividing again higher up (Gymnosperm Database, Pinus pinea; Mutke et
al. 2005 on crown architecture; the same decurrent habit in Quercus suber and Q. ilex, Natividade
1950). Lowering apical dominance alone does not do it: in the Borchert-Honda allocation every
whorl lateral gets an equal share, which is the fan; and thinning the weaker scaffolds in
`_shed` destabilised the form (the leader took over or the crown collapsed).

As built (18 Nov): `ForkRule` per preset (`_fork_rule`). When a stem's tip passes
`fork_height` (the trunk) or `fork_every` years after its own fork (the limbs), its terminal
bud dies and `stems` buds of the stem's own order open around it, `angle` off its axis; they
grow as stems (no apical control penalty) with their own weak upward bend (`gravitropism`), up
to `levels` forks. Nodes carry `stem_level` and `stem_born` along their axis. The crown lift
spares the forked stems. Cork oak and holm oak use the same rule.

Round 2 (18 Nov, docs: above the fork 20-25 thin limbs rose in a fan, each division as thick as
the last; the outer wings were pom-pom tufts on long bare limbs): two fork levels, the second
twelve years after the first, and the crown lift moved into the rule (`lift`, a share of the
height, open-grown only) and raised for the stone pine to 0.55, so the stems stay clear of side
limbs until near the crown and the long, low wing limbs go. With the stems doing the
structure, side shoots get apical control 0.95 again and needles are kept four years: the
cushion runs to the edge. Stone pine: 15.9 m, crown 13.1 m across, base 9.8 m; 6.9M triangles.
Cork oak: 3 m, four stems, ten years, two levels, 45 degrees, lift 0.22.

Round 3 (18 Nov, docs: the split still read as a fan, children need to taper and thin): side
shoots off a forked stem get a share of their vigor (`side`: stone pine 0.5, cork oak 0.7),
so the stems stay thick and dominant and their side shoots thin; stone pine 15.7 m, crown 13.3
m across, base 12.8 m, 6.1M triangles.
 Stand: `STAND_STONE_PINE` (parkland, 13), open-grown umbrellas
about 11 m apart with saplings, snags and logs; not part of `make_woodland`. Its scrub comes
from vegetation's `make_scrub` (lentisk, kermes oak, gum rockrose, dwarf fan palm). Not yet:
juvenile needles on saplings.

### 5aa. Cork oak (18 Nov; Mediterranean trees, second)

Research (Natividade, *Subericultura*, 1950; Pereira, *Cork: Biology, Production and Uses*,
2007; Gymnosperm/Trees and Shrubs Online, Quercus suber; APCOR and Amorim on harvesting):
- Evergreen oak, 10-20 m (to 25), a short bole of 2-4 m dividing into a few thick, sinuous,
  spreading limbs; open-grown crowns broad, rounded and irregular, often wider than tall. In the
  montado the crown is pruned up and opened, so the limbs read clearly under it.
- Leaves 3-7 cm, ovate to oblong, shallowly toothed, dark glossy green above, grey felted
  below, kept 1-2 years. Acorns ripen in their first autumn, 2-4.5 cm, in a scaly cup.
- Bark: virgin cork, the tree's own, grey to buff, very thick (to 20-30 cm on old unworked
  trees), deeply and irregularly fissured into spongy ridges, soft and dull.
- Worked trees: first stripping at about 25 years (a girth of 70 cm at breast height), then
  every 9 years or more (Portuguese law). The cork comes off the bole and the bases of the main
  limbs up to a set height (at most 2-3 times the girth), cut along a near-horizontal ring and
  a few vertical lines. The stripped stem is the phellogen: bright orange to cinnamon red
  fresh, darkening in weeks to deep red-brown, then brown-black over the first year as new
  cork forms. Reproduction cork then thickens 3-5 mm a year: smooth and finely, regularly
  cracked, browner than virgin cork, with a step at the line where it meets the unworked
  bark above. The last digit of the year is painted in white on the trunk.

How the generator gets there:
- `PRESET_CORK_OAK`: broadleaf evergreen (two leaf years), decurrent: the leader split (5z) at
  about 3 m into three or four limbs, low apical control, wide angles; a heavy, low crown.
- `BARK_CORK`: deep, wide, irregular spongy ridges, pale grey-buff, the deepest relief of any
  kind.
- Stripping, `cork_stripped_years` (0 = never worked, otherwise years since the last harvest):
  the peel machinery of 5f with its stepped edge (5o), on the bole and limb bases below the
  harvest height, the step the cork's thickness; the bare surface in the bark shader drawn as
  phellogen and reproduction cork by age instead of wood. The painted digit is a decal on the
  bole facing the track; later.
- A montado stand (open-grown, grazed, 40-80 trees a hectare) after the tree passes docs.

As built, first pass (18 Nov): `PRESET_CORK_OAK` (38) with the leader split above, two leaf
years on the last three years of shoots, shed ratio 0.1 and almost no dead wood kept (worked
trees are cleaned), leaves 7 cm, toothed (six shallow teeth a side), grey felted below
(underside 0.6). `BARK_CORK`: Voronoi blocks (4 x 3 a tile, a third of the walls never split),
widely warped, cracks 0.3 cell wide and 7 cm deep, rounded lumpy tops, warm red-brown fissure
floors under grey-buff ridges (headroom 1.3). Furrow lines (the oak's pattern) were tried
first: too smooth, a few pits. Open-grown 17.7 m, crown 20.6 m across, base 9.2 m; 16.5M
triangles (an open oak is 24M). Close up the displaced cork tore steep plate walls into jagged
spikes: blocks are now 3 x 2 a tile (13 x 20 cm) with rounded shoulders, 5 cm deep.

Stripping (18 Nov): `cork_stripped_years` (-1 picks one of eight stages of the cycle by seed;
30 or more, unworked; trees under 25 and dead wood never). The bole is stripped from 4 cm over
the soil to twice the girth, at most a metre over the fork, so the limb bases are bared too; the
cut wavers a hand's breadth. It reuses the peel machinery (5f) with the stepped edge (5o): the
stripped vertices sink past the deepest fissure of the displaced bark (smooth stem) and lose
3-5 cm on plain tubes, less as the new cork grows back 4 mm a year. The bark shader draws the
stripped stem instead of wood: bright orange (fresh), red-brown (weeks), brown-black (a year),
then cork brown with fine cracks as it thickens (`bark_stripped_age`). Not yet: the painted
year digit, acorns, the montado stand.

### 5ab. Holm oak (18 Nov; Mediterranean trees, third)

Research (Rameau, Mansion, Dume et al., *Flore forestiere francaise 3: region mediterraneenne*,
2008; de Rigo and Caudullo in the *European Atlas of Forest Tree Species*, 2016, Quercus ilex):
- Evergreen oak of the whole Mediterranean, on limestone and acid soils alike, 10-25 m (often
  a multi-stemmed coppice or a shrub on poor ground). Open-grown, a short bole dividing low into
  a few limbs under a broad, dense, rounded dome; it casts the deepest shade of the
  Mediterranean trees and stays leafy deep inside (shade tolerant).
- Leaves 2-8 cm, very variable: adult leaves entire or nearly, leathery, dark glossy green
  above, grey-white felted below, kept 2-3 years; juvenile and sprout leaves spiny like holly.
- Bark dark grey to blackish, thin, finely cracked into small squarish plates.
- Acorns ripen in their first autumn, 1.5-3 cm, in a grey felted cup.

As built, first pass (18 Nov): `PRESET_HOLM_OAK` (39), the leader split at 2 m into three
limbs at 45 degrees with full-strength side shoots and no lift; shade-tolerant buds and a low
shed ratio (0.02) keep it leafy deep inside and down to a crown base of 2.5 m (with a higher
shed ratio and a lift the first try stood a vase of limbs under a cap, a stone pine's habit).
Three leaf years, 40 simple leaves of 6 cm an internode, felted below, `BARK_SCALY` for the
small plates. Open-grown 13.2 m, crown 18 m across; 10.5M triangles. Not yet: spiny juvenile
leaves on saplings and sprouts, acorns, coppice forms (garrigue).

### 5ac. Wild olive, maritime pine and carob (18 Nov; Mediterranean trees, last three)

Research (Rameau et al. 2008, as above; the *European Atlas of Forest Tree Species*, 2016:
Olea europaea, Pinus pinaster, Ceratonia siliqua):
- **Wild olive** (oleaster, *Olea europaea* var. *sylvestris*): a shrub or small tree of
  maquis and dry slopes, 2-10 m, often several crooked stems from one stool; dense, rounded,
  grey-green crown. Leaves opposite, narrowly lanceolate, 3-6 cm, dark grey-green above and
  silvery with scales below. Bark grey, smooth when young, fissured, twisted and hollowing when
  old.
- **Maritime pine** (*Pinus pinaster*): the tallest Mediterranean pine, 20-35 m, on sandy acid
  soils (the Landes, Portugal's coastal pinewoods). A long straight or slightly bowed clear bole
  and an open, irregular crown held high; it self-prunes like Scots pine. Needles in pairs,
  10-25 cm, the stoutest of the European pines, grey-green. Bark very thick, deeply furrowed
  between long purplish red-brown plates. Tapped for resin for centuries.
- **Carob** (*Ceratonia siliqua*): evergreen, 5-15 m, a short thick trunk dividing low under a
  broad, dense, rounded crown. Leaves paripinnate, 10-20 cm, of 2-5 pairs of rounded, leathery,
  glossy dark leaflets, paler below. Bark grey-brown, rough, finely cracked.

As built (18 Nov):
- `PRESET_WILD_OLIVE` (40): three crooked stems (stem spread 20 degrees, direction noise 22),
  opposite buds, a low shed ratio and shade-tolerant buds for a dense crown down low; 30
  narrow 5 cm leaves an internode, silvery below; `BARK_FURROWED`. Open-grown 7.8 m, crown
  9.4 m across.
- `PRESET_MARITIME_PINE` (41): built on the Scots pine growth rules (whorls of five, the same
  apical dominance, self-pruning), grown 45 years with 40 cm shoots, a pipe exponent of 2.0
  for a stout bole, and 18 cm needles. No leader split; open-grown it still self-prunes onto
  a clear bole (the fork rule's lift at 0.25). First tries with weaker leader growth and
  higher apical control grew a bush under a bare leader, then a pole; the Scots pine rules
  gave the right habit at once. `BARK_PLATED`, resin on wounds, pine cones. Open-grown 30.9 m,
  crown base 9.4 m, crown 13.6 m across; forest-grown crown base 18.9 m.
- `PRESET_CAROB` (42): the leader split at 2 m into three limbs at 50 degrees, low shed and
  shade-tolerant buds for a dense dome; six paripinnate 16 cm leaves an internode with
  rounded leaflets, dark and glossy, paler below; `BARK_SCALY`. Open-grown 11.5 m, crown 15 m
  across.

Docs review (18 Nov): holm oak and carob passed. Fixes for the rest:
- Wild olive: leaves paler silver grey-green, and the stems gnarled at the base: a wild olive
  always grows at least half veteran (fluted, burred, hollowing), restored after generation.
- Maritime pine: the Scots pine rules read as a spindly mast of thin whorls, crown small and
  see-through. Weaker apical dominance (0.68) and control (0.8) let the laterals fill a broad
  crown, with 240 needles a shoot over four years. Tried and dropped: a leader split high up
  (16-24 m) and lifting the crown above 30% of the height; both starved the crown to a tuft on
  a pole, and the result is sensitive to the lift (0.32 already thins it).
- Cork oak crown: too open over bare twiggy limbs. Shade-tolerant buds, shed ratio 0.04, four
  leaf years and 34 leaves a shoot give a broad, dense, dark dome on four limbs.
- Cork bark: read as a regular honeycomb of pointed scales. Tried: cells stretched along the
  stem (stripes with dark slots), noise zero lines as furrows (smooth with a few lines, then a
  grid of pits; the three-octave fbm's zero lines close into small loops). As built: an
  irregular crack network of 5 by 3 cells a tile (10 by 13 cm) with a strong domain warp, a
  third of all walls and half of those up and down never split so ridges run on, wide soft
  valleys (crack 0.35 of a cell) and rounded shoulders (square root of the profile), pale grey
  ridges over warm fissures.

Cork oak round 4 (18 Nov; r3 bark read as wet clay, the crown as a eucalyptus vase): the crown
takes the holm oak's dome rules (apical control 0.95, shedding 0.02, internodes 0.3 m, leader
growth 0.5) on the one four-way split, limbs at 62 degrees and side branches at 0.8 vigor:
15 m, a broad, low, dense dome. Fissures 4 cm deep again, the relief blurred half as much (two
passes of 5 texels), spongy noise 3 mm; fewer, thinner lichen crusts.

Cork oak round 5 (18 Nov; r4 bark read as fish-scale tiles in a diagonal lattice with rows of
dashes, and the split pinched into a neck): the lattice was the 0.4 m bark tile repeating five
times round a 2 m bole, whatever the pattern. Cork now has its own 1.2 m tile at 1024 texels
(the moss layer scales to match), and its ridges are the furrowed family's: zero lines of two
noises stretched along the stem, 9 and 6 cycles round a tile, so a few dozen irregular ridges
5-8 cm wide merge and split round the trunk between dark furrows 6 cm deep, with lumpy, pitted
tops. The Voronoi pillows and the cross breaks (the dashes) are gone. At the split, the stems
of a leader's fork (stem_level above their parent's) now enter as a fork crotch whatever their
share, their bases swollen to 0.7 of the bole and easing back over a bole radius and a half,
and the bole's last two radii narrow to 0.6 under a low cap: the bole flows into the limbs.

Cork oak round 6 (18 Nov; r5 read as waxy clay, with green lichen decals, a smooth root flare,
a collar of tape at the split and leopard blotches on the limbs): cork's texture gains its
spongy mass (Pereira 2007): crumbs a few millimetres across, open pores, matte throughout, and
weathering from pale grey to ochre. The main furrows run the whole tile (one noise cycle along
it), and only they darken: the side family's partings, dark too, broke the ridges into blotches.
No lichen on cork. The collar was the leader's end: like any shoot tip its last internode tapered
to 0.6, a pinch hidden under the stems' bases; a split leader now keeps its girth to the split
and runs on half a metre as a blunt cone the stems leave from, the saddles of the crotch, with
the stems' bases swollen to 0.8 of the bole and starting narrower inside it.

Cork oak round 7 (18 Nov; r6 kept a collar at the split: the bole's displaced ridges stood
through the stems' furrows as teeth over a ledge, and the ridges read thin and stringy): the
displaced relief fades out within 1.6 bole radii of the split, on the bole and on its stems, so
the two smooth surfaces meet in the saddle curves of a crotch (the normal map keeps the bark);
the stems rise just inside the bole's surface and take the bole's tone. The cork has fewer,
fatter ridges (six noise cycles a tile), fissures twice as wide and 8 cm deep, and broad shallow
cross breaks that cut the ridges into blocks without darkening (dark, they read as dashes).

Maritime pine round 3 (18 Nov): no excurrent crown cone (it kept the top a sparse spire over a
wide lowest whorl, the floating disc); the bole is lifted clear to 42% of the height; apical
control 0.9, shedding 0.06 and few kept dead branches give a dense, irregular, rounded crown in
the top 60%.

Cork round 8 (18 Nov; docs declined r7: each stem a separate tube in the bole with a sleeve
rim and the bark changing across it, pale blotches on the bole, fissures as thin pencil lines,
a root flap). The crotch is now swept: over the split the bole runs on as one surface, the
outline of the smooth union of the bole and its stems (blended skeletal surfaces, Bloomenthal
and Wyvill 1990; Quilez's polynomial smooth minimum, so the blend acts only where surfaces meet)
cut by each bole ring along its own radii, from 0.6 bole radii under the split to 1.1 over it,
where the outline lies on the stems. Its v runs over the surface side by side and its UVs turn
into each stem's own (the stems' tubes framed from their inner side, so the crotch knows their
u), except down the valley between two stems; the stems start on the bole's axis and are drawn
1.5% inside the crotch until its top ring. No displaced relief on the crotch (the normal map
keeps it). The cork itself: fissures are cut to a width in metres from the first-order distance
to the noise's zero lines (|n| / |grad n|; Taubin 1994), not a threshold on the noise, which gave
hairlines where the noise was steep: main fissures 3-5 cm across and 4.5-9.5 cm deep with
rounded shoulders, a second family splitting half the ridges, cross fissures every 15-25 cm
cutting them into blocks across their width; the whole fissure darkens (red-brown walls, dark
floor); each block swells from its fissures to a rounded crown 3 cm proud (flat between the grooves, it read as carved plaster); ridge-top mottling at a few centimetres and the ochre weathering faint (strong, it read
as leopard spots). Buttress roots flatten as they reach the soil and slide under it.

Maritime pine round 4 (18 Nov): the crown read as a see-through fan of bare limbs with tufts at the tips. Shedding 0.02 and six years of needles keep shoots inside the crown, a lighter self-shadow (0.03) lets them live, and a pipe exponent of 1.85 gives a stout bole under the heavy crown.

Round 3 (18 Nov): the leader splits once into four limbs (a second split at ten years gave
sixteen, the broom of thin pale limbs the docs saw), their side branches held to 0.35 of their
vigor, the fork stems at 55 degrees; no dead branches kept (the twig bundle on the bole). Cork
colour darker grey-buff (0.36, 0.34, 0.31), so the limbs read as cork. The cork relief is a
blurred pillow per cell (three box passes, about a centimetre): a pillow shaped from the
distance to straight cell walls has sharp creases along its medial axis, the faceted pyramids
read as scales. Long wandering fissures from stretched noise cut across the network; cells 3 by 2
to a tile, side walls merge 45%, vertical 75%; fissures 2 cm deep, paler and warmer. The near
tube keeps 5 cm rings (longer rings made diamonds) and samples a further blurred height.

Not yet: olive and carob fruit, cultivated olives (grafted, pruned open vase), resin-tapping
scars on maritime pines, Mediterranean stands (montado, maritime pine plantation).

### 5ad. Hedge shrubs and cut hedges (18 Nov; farmland and hedgerows, for vegetation)

Asked by vegetation for Paulo's farmland pick: the woody hedge shrubs as shrub species
(`make_shrub_species`, like gorse and broom), with a cut field-hedge form. Vegetation places
them along field edges and builds the margin flora itself.

Research:
- **Cutting** (Hedgelink, *Hedge management cycle*; Defra, *Hedgerow Survey Handbook*, 2007;
  Staley et al. 2015 on cutting regimes): most field hedges are flailed every one to three
  years in winter, into an A-shape (a trapezoid section: wide base, narrower top) or a box.
  Good practice is at least 1.5 m tall and 1.5 m wide; typical managed hedges stand 1.8-2.5 m
  and 1.5-3 m wide at the base, a top of 0.6-1 m on an A. Each cut stub breaks into several
  short shoots, so a cut hedge is a dense twiggy shell 30-50 cm deep around older stems, leafy
  to the ground when not over-trimmed; over-trimmed or old ones go gappy at the base.
- **Laying** (National Hedgelaying Society, Midland style; BTCV *Hedging*): stems cut three
  quarters through at the base (pleachers) and laid along the line at about 35 degrees, held by
  stakes every 45 cm and woven binders; the stools and pleachers send up straight regrowth. Every
  10-15 years, between flailings. Not in this round: it needs a pleacher model in the growth.
- **Hawthorn** (*Crataegus monogyna*): the hedge plant (often half or more of a hedge). Leaves
  2-4 cm, deeply 3-5 lobed. May blossom: white flowers 8-15 mm, five petals, pink to purple
  anthers, in flat corymbs of 5-25, smothering the hedge for two to three weeks in May. Haws
  deep red, 8-10 mm, September into winter. Thorns 1-2 cm.
- **Blackthorn** (*Prunus spinosa*): suckers into thickets 1-4 m; dark, almost black bark;
  side shoots stiff, at wide angles, ending in thorns. White flowers 10-15 mm, singly or in
  pairs, crowded along the bare twigs in March-April before the leaves (white hedges in early
  spring). Leaves 2-4 cm, elliptic, finely toothed, dull. Sloes 10-15 mm, blue-black under a
  waxy bloom, September to November, often left on into winter.
- **Hazel** (*Corylus avellana*): already a preset (coppice); in hedges cut like the rest;
  catkins in February.
- **Field maple** (*Acer campestre*): a small tree 8-15 m, often a hedge shrub when cut; leaves
  5-8 cm, five rounded lobes; twigs often with corky wings; butter-yellow in autumn; keys in
  pairs held almost straight out.
- **Dog rose** (*Rosa canina*): arching prickly canes 1-3 m from the base, scrambling up to 5 m
  through a hedge. Leaves pinnate, five to seven toothed leaflets. Flowers 4-6 cm, five pale
  pink to white petals, June; hips oval, scarlet, 1.5-2 cm, from September, kept into winter.
- **Standards**: oaks left uncut in the hedge line grow open (short bole, wide crown): the
  existing open-grown oak.

Plan: presets `PRESET_BLACKTHORN`, `PRESET_FIELD_MAPLE`, `PRESET_DOG_ROSE`; hawthorn and hazel
as they are. `SHRUB_HEDGE` (`FORM_HEDGE`): a two-metre segment of hedge grown from several
stems inside an envelope cut every year, A-shaped or boxed by variant, its profile kept in the
mesh meta for placement end to end along a line. Blossom and fruit by day of year in the crown
parts: hawthorn May blossom and haws, blackthorn flowers on bare wood and sloes, dog rose
flowers and hips.

As built: `make_shrub_species(preset, SHRUB_HEDGE)` takes the five hedge shrubs. A segment
runs along local X from 0 to 2 m (`hedge_segment_length` meta, spacing 2 m, no random yaw).
The envelope is the clip AABB of section 7, narrowed with height on even variants (the A's
batter, top 0.6-1 m wide). The cut: a terminal shoot that would leave the envelope stops and
its parent node gets two new terminal buds turned back along the face (`envelope_sprouts`),
for up to four cuts in a row; dormant buds live eight years, shedding is off and leaves stay
two years longer, so the shell is leafy to the ground. Seven or more stems are planted along
the segment; the pipe exponent is at least 2.5 so the thousands of shell twigs do not sum to
tree-sized stems. Hawthorn and blackthorn flower from every young leafy shoot (spurs), not only
the shoot ends: two corymbs of 24-48 flowers per hawthorn shoot, 8-16 flowers along each
blackthorn shoot, each small flower five triangles with its boss coloured at the centre (a
third of the full flower); dog rose keeps the full five-petal flower with a raised boss.
Blossom days: blackthorn 70-120, hawthorn 118-158, dog rose 150-196. Sloes days 240-20, hips
240-45, haws as before. A 2 m segment is 200-310k triangles at LOD 0 in flower, 70-250k out of
it. Laid hedges are left for a later round.

Round 2 (docs, 18 Nov: a thin leafy shell over bare stems, an upright box for the A, a flat lid
of long whips, blossom as scattered dots, oversized hazel and rose leaves): each year after the
second, buds wake at an eighth of the shoots within half a metre of a face, pointing out to the
nearer face (level near the foot), so the hedge fills to an opaque twiggy mass and the A widens
at the foot. Regrowth internodes are 0.6 of the bush's, leaves 0.65 (hazel, field maple) or
0.85 of it. Hawthorn corymbs are tighter (2.4 cm across), 16-32 cream flowers. A segment is now
160-540k triangles at LOD 0.

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

Fixes 13 Nov: the basal hollow opens as a pointed arch, widest at the soil, with a broad callus
roll centred just inside the rim; past the rim the wall is red-brown rotten wood with no moss.
Features take their bearing and push from the ring's centre, not the vertex normal (the relief's
normals swing by tens of degrees, so edge vertices fell in or out at random as pale blades).
Rebuilt normals take one winding sign per part: flipped per vertex against the old normal, the
steep side walls flipped at random and shaded as dark and sky-blue patches under the path
tracer. Tangents are squared to the new normals.

### 7b. As built

`TreeGenerator.get_collision_capsules()` (also the mesh meta `tree_colliders`): wood thicker
than `collider_radius` (8 cm) in capsules of up to 2 m. `TreeWind3D` gives instances within
`collision_range` a static body of those capsules on `collision_layer`, through the physics
server (PhysX when that is the server). Nodes in `touch_group` push the bones they reach out
of their sphere; each bone yields by `touch_stiffness` over its own stiffness, so twigs and
shrub stems give way and trunks do not, and springs back on its own frequency, more damped than
in free sway (leaves rub). A body faster than `break_speed` that bends a branch thinner than
`break_radius` past about 35 degrees snaps it: the bones collapse on the tree and the branch,
cut from the tree's mesh, becomes a RigidBody3D thrown with half the body's speed. Grass under
the tree still reads the trample field as before.

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

### 2e. Branch collars (12 Nov; Paulo picked "Collar flare and bark ridge")

Paulo's close-up of an open oak's base: limbs met the trunk as two solids with a hard crease, a
thick limb ended in a flat cut, and limbs crossed near the base. Most limbs over 5 cm had in
fact never been welded: their tube left the three modelled parent segments through an open end
and 2d skipped them.

Research. SpeedTree's weld (weld offset, spread up and down, blend, light-seam reduction that
bends normals toward the parent, extra weld segments); Galbraith et al. 2004 (BlobTree trees:
implicit blend, base pulled back by 1.75 r); Lim et al. 2020 (skin surfaces at joints); the
anatomy, Shigo 1985 (the branch collar: the limb's wood runs down the parent before turning
out, the trunk collar wraps it, and a raised branch bark ridge sits in the crotch); Pirk et al.
2012 (Plastic Trees: branches bend away from obstacles). Built SpeedTree's version, shaped by
Shigo's anatomy, since it stays a tube (no implicit polygonizer, no extra triangles):
- **Every limb welds** up to 0.6 of its parent's radius; above that it is a fork of near-equal
  stems, a crotch (welded, an oak's low fork turned into a blocky hood). The parent is up to six
  segments (two before the junction, three after) tested as a union of capsules. Each vertex
  marches out of the union and bisects to the bark. A weld fails only past the far end of the
  modelled parent. On the open oak no limb under that ratio is skipped.
- **Steep limbs** (under 30 degrees off the parent) are not slid: on the bark they lay almost
  tangent and showed through the furrows as long slivers. They stay crotches, with normals
  blended.
- **One seam ring.** Only the outermost slid ring of each line down the tube is the seam. The
  rings inside go back into the parent (collapsed onto the seam they showed as discs through
  furrows). The rings after it are spaced evenly out to the first ring well clear, so the top of
  a steep limb does not fold back in a crescent.
- **Collar flare.** The seam moves down the parent's axis, which keeps it on the bark: by 0.1 r
  on top of the limb, up to 1 r underneath. The rings after it follow less and less over 1.5
  limb radii, a teardrop drawn down the bark. The spread is set per line of the tube and
  smoothed round it; set per vertex, neighbouring lines met the bark a ring apart and the edge
  came out in teeth.
- **Branch bark ridge.** In the crotch (top of the limb) the bark rises in a low ridge, 0.1 r
  high, just above the seam.
- **Shading.** Normals come from the welded faces and turn to the parent's bark at the seam.
  The tangent frame follows (Gram-Schmidt), so the bark normal map is not skewed there.
- **Limbs ending in a flat cut.** A limb over 3 cm that a collision would cut now goes back to
  its parent instead (no tree self-prunes a living limb into a sawn stub). Cut limbs keep a
  rounded end of three rings (wound wood) instead of an open tube, and any kept cut over 1.5 cm
  gets the splintered rim of a broken end.
- **Crossing limbs.** Before cutting, a limb that runs into thicker wood is swung away from it,
  rotating the whole limb about its base by up to 30 degrees in three tries (Plastic Trees'
  response to obstacles). Only if that fails is it cut. `get_collision_deflect_count()`.
- **Pale flaps** (12 Nov, after the merge): in sun, limbs running close along a trunk showed
  pale, torn-edged flaps (sycamore most). Normals were the cause, not texture: points the
  parent's capsule model calls well inside the parent took the parent's normal, but the model is
  not the mesh everywhere and some of those points are in view. Points more than 0.05 r inside
  the model now keep the tube's own normal. The seam also goes on the parent polygon's inner
  circle (r cos(pi / sides)) rather than 0.98 r, since between vertices the faceted tube lies up
  to 4% inside the round radius.
- **Cost.** Limbs the old collision test cut (it also caught limbs overlapping their own parent
  inside the trunk) now stay or bend away, so trees keep wood and leaves they had lost: oak
  variant 1 went from 58,172 to 66,253 internodes and 16.6M to 25.7M near leaf vertices. The
  European woodland (one variant, far leaves) went from 81.1M to 88.5M vertices (bark 29.8M to
  33.1M with the end caps and collar rings, leaves 42.8M to 46.0M). In the cloud, `--stands
  --test` now runs out of memory in the near-leaves check after all other checks pass; the
  check passes alone on variant 0. `--test` passes in chunks of eight presets (`--presets=`).
- **Bench.** `--view=bole` (oak base, Paulo's angle) and `--view=junction` (the lowest thick
  limb, 2 m off). Close-up views now keep sun shadows within 40 m: the 300 m cascades turned
  leaf shadow flecks into blocky pale speckle that looked like a mesh fault.
- **Docs-shot review (12 Nov afternoon).**
  - Veteran burls grew with the radius under them, so a burl placed on the root flare
    ballooned. They now sit above the flare (from the flare height plus 25 cm) and scale with
    the stem's own girth, capped at 1.15 of it.
  - Twigs under the collision radius were never tested, so thin shoots ran straight through
    thick limbs near the base. Wood from 4 mm is now tested against limbs of 5 cm and up, and
    deflects or is cut like the rest.
  - An axis keeps a lateral's girth up to the fork where the lateral outgrew it (a leader
    overtaken), in the tube and in weld()'s parent model alike.
  - What read as sawn stubs: on the sycamore bole, limbs heading at the camera whose shaded
    underside crease looks like an open end (no open geometry there). On the oak, near-equal
    forks, which weld() leaves alone: the fork's first ring sits on the parent's axis, tilted,
    and where the parent narrows past the fork the ring's far side stands proud as an open
    rim. A rounded start cap and a start sunk down the parent's axis were both tried; the
    first made bulbs, the second a visible inner ring. Fixed as branching-tube meshes do it
    (Lluch et al. 2004, one mesh for the branching structure; SpeedTree's fork splits): the
    fork's base runs up inside the parent with rings parallel to the parent's, no wider than
    0.9 of it and shifted toward the side the fork leaves on, then turns out through the
    crotch. The collar's rings near the parent's axis are dropped for these.
  - Beech (many stems from one base): limbs on a fork's first internodes welded onto the fork's
    axis model, but the fork's base now runs inside its own parent, so the slide and spread
    tore their bases into flaps with toothed edges. Those limbs stay plain crotches. The relief
    tube (up to 128 sides) now smooths the collar's spread over the same arc as the plain
    tube (passes scale with sides squared); two passes there left its edge in teeth.
  - Veteran features (docs review of the oak pair): rot holes stay below the first fork (one
    cut into the crotch read as a missing polygon); hollows deepen gradually from the rim to a
    dark floor under a rolled callus lip (woundwood, Shigo 1985) instead of a flat-sided slot;
    burls are (1 - e^2)^2 with broad lumps, no crease at their edge and no crumpled facets.
  - Root collar: the flare ended in a skirt standing on the ground, its rim visible, and on a
    leaning stem the base ring rose out of the soil on its high side. The base now sinks by
    the lean and splays over the last ~20 cm, and bark relief fades out at the soil line
    (plates and furrows cut the ground plane in a ragged line). Buttress lobes (Mattheck's
    buttress form) were tried and taken out: lobes that spread below the ground broke through
    it in slivers, and lobes held at the ground line still did. A real fix wants the lobes as
    their own geometry that dives steeply under the surface.
  - The basal hollow folded through itself in pale blades: it was sized by the flare's swollen
    radius, roughened finely, and the low forks dented the same bearing again. It is now the
    main stem's alone, no deeper than half the stem's own radius, with broad roughness.
  - Bench `--view=veteran`: the lower 4 m of the bole facing its basal hollow (or largest
    feature; `VAZ` env fixes the azimuth for a pair), lit by a low sun from the camera's side.
  - Bench: `--set=prop:true` was parsed as 0 (float("true")), so "weld on" shots were weld off.
    `--junction-side=-1` views the junction from the limb's other side. The junction view
    picks a true limb (under half the trunk's girth, spreading, starting inside the trunk).

### 2g. Lighter near bark (15 Nov; Paulo: "all of them if they are worthwhile")

Where an open oak's 4.4M LOD 0 bark triangles go: 2.35M are the leaf twigs (196,000 bent
three-sided twigs of three rings, 12 triangles each, the least a bent tube can have), 0.84M the
displaced relief tubes, the rest limbs. So the near bark was trimmed where it showed nothing:
- **Relief only where it shows.** For each bark family the displaced tube's grid (2.5 cm by
  5 cm) is checked against its own height map: the 90th percentile error of the grid against
  that of a smooth tube. Smooth and papery bark (0.6 mm deep) and flaking bark (3.5 mm) gain
  under a millimetre; scaly bark gains 1.6 mm (its scales are smaller than the grid), so only
  its thickest stems keep relief. Where the gain times the stem's relief scale is under 1 mm,
  LOD 0 gets a smooth tube instead, with sides for 1 mm of chord error and rings every 10 cm.
  The normal map carries those barks' detail as before.
- **Rings along furrowed bark** every 10 cm instead of 5 where no moss shell lies on the stem
  (furrowed ridges run along the stem: the grid's error is the same). Under a moss shell the
  bark and shell share one grid, so those keep 5 cm.
- **Thin shoots** (under 1 cm radius) drop rings where the axis and radius stay within a quarter
  of the radius of a straight run (Douglas and Peucker 1973, as hair strands are).
Result over the 31 species, open and stand-grown: bark triangles 91.2M to about 86M (-6%);
beech -10%, sycamore -17 to -21%, red maple -18 to -22%, silver fir in a stand -24%; oaks,
limes and pines unchanged (their relief is real). Raster and path-traced crops of beech, spruce
and a moss-free oak trunk match the old ones (mean pixel difference under 1.5 of 255).
The leaf twigs are left as they are: thinner geometry would straighten them off their leaves.

### 2h. Lichen on bark (15 Nov; Paulo: "all of them if they are worthwhile")

Research. Placement after Desbenoit, Galin and Akkouche, "Simulating and modeling lichen growth"
(Eurographics 2004): growth follows the scarcer of light and moisture. Ecology from Barkman,
"Phytosociology and ecology of cryptogamic epiphytes" (1958), and Gilbert, "Lichens" (2000):
crusts first on young smooth bark and twigs, leafy rosettes on older bark, lichen higher up the
stem and on the lit side where moss holds the wet, shaded foot, yellow-orange Xanthoria on
nutrient-rich, lit bark (ash, elder, poplar). Game trees usually bake lichen into the bark
texture instead of placing it by the stem's light and moisture.

As built:
- **Where.** A weight per bark vertex: rise up the stem (none in the lowest 15 cm, full by
  1.4 m), light from the crown (stand shade dims it), young thin stems favoured, patch noise so
  it comes in colonies, and less where moss lies. Species carry an amount (smooth-barked ash,
  beech, rowan, birch most; pines least), a share of leafy rosettes and a share of orange. Dead
  trees scale it by decay class (class 1 a little more, class 4 and 5 little), fallen logs half.
  `lichen_cover` scales it all (1 by default, 0 turns it off).
- **What.** One 512 px texture tiled every 0.6 m on its own UV set (a whole number of repeats
  round the stem, an offset per stem): growth order of grey-green crusts with domain-warped,
  rough edges, leafy rosettes, a colony id (only small colonies may be orange) and marks for the
  dark prothallus line, fruit discs and cracked areolae. The crust order is stored as its rank
  over the tile, so a weight covers that share of it. Each colony starts at its own weight and
  then spreads outwards, so a thin cover is a few grown patches, not every colony's first speck.
  Crusts keep to the bark's ridges and stop at the furrows (the relief rides in the roughness
  map's free blue channel). Each colony gets its own tone; bark shows through thin crust;
  roughness goes towards 0.9.
- **First look at an oak close-up** (bench bole view, 1.5 m) showed the lichen as even pale
  specks: 22 small colonies per tile at a thin cover were all their cores. Fixed as above (9
  larger crusts and 7 rosettes per tile, ages per colony, ridge keeping).
- **Cost.** No extra geometry. A second UV set on bark and one texture fetch. Frame time on the
  bench with and without it is within the software renderer's noise.
- Raster and path-traced crops of ash and beech match. Making them match found a fork-wide bug:
  path tracing read `source_color` vec3/vec4 defaults of custom shaders as linear, so their
  colours came out too bright; they are now converted to linear as raster does.

### 2f. Clear trunks on open-grown oaks (12 Nov; Paulo picked "Mix of both")

Open-grown oaks came out as a broom of stems forking about a metre up: grown alone in full light
from the first year, the young tree kept every low limb, and the strongest of them grew into
co-leaders. Real parkland oaks mostly have a clear trunk of 3 to 5 m under a spreading crown,
because they did not grow up in the open. Vera (*Grazing Ecology and Forest History*, 2000)
shows pedunculate oak regenerating inside thorny scrub (blackthorn, hawthorn) in grazed land: the
scrub shields the sapling from browsing and shades its low limbs, which die and drop, and the
crown spreads once the leader clears it. Rackham (*Woodlands*, 2006) describes the same history
for wood-pasture oaks, along with a browse line. Oaks that came up without that shelter fork low.

As built: `_nurse_scrub_height()` gives about two in three open-grown pedunculate oaks (by seed)
a scrub of 3 to 4.5 m, and 0 for the rest. While the tree grows (no forest shade), the shadow
grid gets shade below that height, kept off the stem's own line. Once the leader is 0.5 m above
the scrub, `_shed()` drops the limbs leaving the stem below it, as a stand drops its shaded limbs.
The leader's own continuation is never shed. Limbs from above the scrub that bend down under
their own weight can still sweep low, as on real open oaks. Stand-grown oaks are unchanged, since
their stand already clears the bole. The other oaks (white and red) and lime are left as they are
until they are looked at.

## 8b. Woodland memory (12 Nov)

The showcase scene ran out of memory (about 14 GB) generating the European woodland before
anything was placed. Measured per species with `make_woodland(1)`: open-grown broadleaves
carry 10-20M vertices each. Their LOD 0 is every leaf as geometry, and index LODs keep that
vertex buffer resident even for far trees. Stand-grown trees carry 0.1-4M.

Fixed (waste and bugs, no change in look):
- **Shrubs under a canopy** were grown as open-field domes, for example a 14 m wide-crowned
  holly with leaves to the ground (9.9M vertices). They now grow in the stand's shade
  (FORM_UNDERSTORY, "(understory)"), drawn up and thin, as an understory is. Hedgerows keep the
  open form. The beech stand's shrub layer went from 23.4M to 3.4M vertices.
- **Open-grown forms** in a stand go only where the canopy leaves real light (shade under 0.3),
  whatever the species tolerates.
- **Leaf blades** had three coincident vertices at the base and at the tip, so four zero-area
  triangles and four spare vertices per blade (half of a two-step leaflet). Under flutter the
  tip pair also split into a sliver. The ends are now one vertex each: -22% vertices and -19%
  triangles over the woodland.
- **Deadwood order**: snags and logs are placed before the shrub layer, so a dense understory
  no longer leaves them no room.

European woodland, one variant, unique meshes: 117.5M vertices before the blade fix, 92.0M
after; generation 31 s. The open forms are still about 70% of that.

How the established systems hold this at scale:
- **UE5 Nanite foliage / assemblies** (Witcher 4 demo, 2025): a tree is a trunk plus
  micro-instanced parts (leaves, twigs), about 12 unique parts in 2,160 instances. One tree on
  disk went from 3.5 GB to 29 MB. Far clusters become voxels. One bone per part, skinned in
  compute.
- **AMD and Coburg, Real-Time GPU Tree Generation** (HPG 2025): no stored trees. Work graphs
  with mesh nodes generate them every frame (34.8 GiB to 51 KiB in their demo).
- **Streaming**: classic engines keep separate LOD meshes, so far LODs never hold LOD 0
  vertices, and they stream by distance.

What the next step is (design change, on a card): LOD 0 leaf geometry kept only for trees in a
near range and generated on demand, as `near_meshes` already does for needles. The far meshes
would carry only the leaf twins (LOD 1 and coarser). Species would also be generated when a
tile in range first needs them, and freed when none does.

## 8c. Leaf detail near, on demand (12 Nov; Paulo's pick)

`TreeGenerator.near_leaf_range` (metres, 0 = off; 60 for the showcase). Species with lobed or
compound leaves (oak, red and white oak, hawthorn, sycamore, sugar and red maple, ash, horse
chestnut, rowan, elder) are generated with each leaf as its simple twin, the lens of the same
area that LOD 1 and coarser already drew (section 2b). Their PlantSpecies get
`near_mesh_source` (a copy of the generator as set up for that species) and `near_mesh_key`.
VegetationField3D (vegetation, 9cd277f3) calls `request_near_mesh(key, variant)` for cells
within 1.2 x the range; the copy grows that variant again on a low-priority worker thread, one
at a time per species, and returns null until it is done. The near mesh is exactly the mesh the
generator makes without the split (bench check: vertices, indices, UVs, every LOD, rig). Only
the field's MultiMeshes hold it, so it is freed when the field frees its last near cell (beyond
1.5 x the range). The species holds the source alive.

Why no look change: lobes and leaflets are 1 to 3 cm, a fifth of a pixel at 60 m in 1080p
(about 12 px per metre at 75 degrees) and half a pixel in 4K. Raster at that distance already
draws LOD 1 or coarser (the twins) at Godot's 1 px LOD threshold beyond about 35 m in 1080p and
70 m in 4K; the path tracer draws LOD 0, now the lens of the same area, where it can't resolve
the lobes. Near meshes share the far meshes' materials (same shaders and batches).

Two things the worker thread exposed, both fixed: climber leaves read each ivy leaf back from
the RenderingServer (a wait on the render thread per leaf; now read once per climber), and the
leaf flutter material read the base shader back. The near build takes its far mesh's materials
instead, so a near build never waits on the main thread.

Shots (oak, sycamore, rowan; 1080p pixel density): at 60 m and 100 m with Godot's default LOD
threshold the far and full trees are the same image (crown luminance 97.5 vs 97.6, sky seen
through 0.389 vs 0.390). Forcing LOD 0 everywhere, a stand-in for what the path tracer sees, the
lens crowns are about 2% darker at 60 m (98.6 vs 100.7) and 1% at 100 m: the same small step the
raster LOD 0 to LOD 1 switch already makes there. A denoised path-traced pair goes on the PC list.

Measured: open oak leaves 14.9M to 4.3M vertices per variant. European woodland, one variant:
103.2M to 81.1M vertices (leaves 64.8M to 42.8M; bark, needles and moss unchanged). A near oak
takes about 6 s to build on one cloud core (the PC is several times faster), so a tree coming
closer at walking pace has it before the 60 m crossfade; at speed the far mesh, which looks the
same there, stays a few seconds longer.

Twin area (12 Nov): the twin was sized by the lens formula, not by what the species blade
covers. Measured on one leaf per species, an ash or horse chestnut twin was 16% larger than its
leaflets, an oak twin 5% smaller than its lobed blade. The twin is now scaled to the species
blade's measured area (`_twin_area_scale`, once per species). Forced LOD 0 at 60 m: oak 104.8 vs
105.0, rowan 128.8 vs 129.1 (was 1-2% apart), sycamore 92.5 vs 94.5 (was 3%), whose remaining
step is the lens against the fanned palm's shading, not area.

Simple-leaved species (beech, birch, willow, alder, lime) are unchanged: their LOD 0 leaves are
already their only form. Next possible step (needs a card): far meshes that start at LOD 1 for
every broadleaf, the stochastically pruned, area-preserving level (Cook et al. 2007), which
raster already draws beyond the near range; it changes what the path tracer sees at distance, so
it needs a path-traced comparison first.

## 8d. Far meshes from LOD 1 (16 Nov; Paulo's pick, after hard places)

`TreeGenerator.far_lod1` (on by default; works with `near_leaf_range`). Every broadleaf, simple
leaves too, now splits: its far meshes start at LOD 1, half the leaves with each survivor grown
by 1/sqrt(0.5) about its base so the crown's leaf area stays (Cook et al. 2007), and vertices
only LOD 0 used are dropped (`_promote_lod1`). LOD 0 lives only in the near mesh, built on demand
within the near range as before. Coarser levels are unchanged.

Path-traced comparison first, as the card asked (one tree at 60 m, the near range's edge, 16 spp,
960x540, 75 degree lens; crown box luminance and share of sky seen through it):
- beech: 96.8 vs 96.5, sky through 0.484 vs 0.482;
- oak: 74.4 vs 74.1, sky through 0.322 vs 0.317.
Under half a percent, inside the noise, and the images match by eye. Beyond 60 m the step only
shrinks. Raster beyond the near range already drew LOD 1 or coarser.

Measured: leaf vertices of one far variant, beech 8.19M to 3.35M, oak 3.95M to 1.61M (59% less);
oak far against full 1.61M vs 13.6M. The stand bench's wind LOD error fell from 1.55 to 1.35 px
p99 with the lighter meshes (still over its 1 px bar, a check failing since before 5q). A
denoised PT pair at 60 and 100 m goes on the PC list.

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

**Worked stands (16 Nov; Paulo picked crown details, then stands, then bark stories).** Three
managed woods that read as made by people (Rackham 2006, *Woodlands*; Peterken 1993; Read 2000,
*Veteran Trees*):
- *Coppice with standards* (`STAND_COPPICE_STANDARDS`): open-grown oak standards (short bole,
  spreading crown; the drawn-up forest form read as lollipops) at a third of a high forest's
  density and 2.2x its spacing (ash too on wet ground), over hazel coppice in coupes:
  two hazel forms (`FORM_COPPICE` cut 4 years to a cycle ago, `FORM_COPPICE_YOUNG` 1-3 years)
  in 40 m patches that alternate, so the wood shows cut and grown panels side by side. Some
  hawthorn. Deadwood a third of an unmanaged oakwood's: the wood is worked.
- *Wood pasture* (`STAND_WOOD_PASTURE`): ancient beech and oak pollards (`FORM_POLLARD`, cut at
  2.2-2.7 m, lapsed 40-100 years, 140-200 years old, veteran 0.6-0.9; older beech pollards
  died back to a few shoots on a bare head). Grown open, not under forest shade (which drew
  their poles into 22 m whips).
- *Lapsed pollards regrow* (16 Nov). Cutting rejuvenates (Read 2000; Lonsdale 2013): once a
  pollard is a cycle past its last cut, its shoots grow at the internode length of a tree aged
  from that cut plus ten years, not of a 160-year-old (0.985^160, a tenth). At the next cycle
  after lapsing the head self-thins to its 5-8 strongest poles; the rest die back to stubs. A
  beech lapsed 60 years stood 10 m tall and 8 m across on 415k leaf triangles; now 16 m and
  15 m across, its poles grown into limbs. Worked pollards and coppice keep their tuned rods. and open-grown oaks, at 3x a
  forest's spacing (a tree every 20-30 m; at forest spacing the crowns closed into a wood), with open-grown hawthorn and holly; grazed, so no saplings; fallen limbs and an
  occasional log.
- *Pollard row* (`STAND_POLLARD_ROW`): working willow pollards 5 m apart along a ditch (cut at
  2.0-2.4 m every 5-8 years), a few open alders. Placed by the scene along its lines, like a
  hedgerow.
Each new form is its own cached variant (`form` in the cache key). Bench: `--stands --plot=10`,
`11`, `12`; `--stands --test` checks pollards in pasture and rows, no pasture saplings, both
coupe ages and coppice stools outnumbering standards more than 4 to 1.

**Foliage through the crown.** Open-grown trees keep leaves down to the lower branches: a weaker,
deeper shadow (strength 0.08 over 4 cells) and a third of the old shedding. Short shoots (spurs)
on wood under 30 years and 12 cm thick carry leaves wherever Beer-Lambert light exp(-shadow)
stays above 2%; they are added to the mesh after growth, since letting them shade during growth
collapsed crowns to poles. Twigs leave in all directions and some fork; leaf normals follow a
spherical-like inclination (Campbell 1986). Holly, yew and box form the evergreen understorey
(holly kept to January above -1.5 C, Iversen 1944).

**Leaf area budget.** One internode stands for a twig system, which put an open oak near leaf area
index 17. Whole twigs are now dropped at random to at most LAI 5.5, the temperate broadleaf mean
(Breda 2003; Asner et al. 2003); leaf size stays true. Open oak: 442k leaves, 7.7M triangles
(was 1.49M, 23.6M). `get_leaf_area_index()` reports it. The hero mesh is still millions of
triangles; LODs and leaf clusters are stage 2.

**Stand-grown form, second pass.** Stand trees poured all vigor into the leader (beech 39 m,
sugar maple 36 m at 50 years) with a crown a few metres deep. `leader_growth` caps the metres any
shoot adds in a year (0.6-0.7 for beech, oak and maples, 0.75-0.9 for white pine, spruce, larch
and birch). Capped, they turned broom-shaped instead: low limbs of the open sapling climbed to the
canopy, because the stand was modelled at its final spacing from the first year. The stand now
starts dense and thins as it grows, spacing a quarter of the top height (Hart-Becking index 20-30%;
Wilson 1946) up to the mature spacing, so lower limbs are shaded out early: forest beech clear
bole 0.6 m to 5 m, sugar maple 13 m (`get_bole_height()`). Leaves are now thinned first where they
crowd, to 1.5 m2 of leaf per m3 (Lalic and Mihailovic 2004), then to the crown's LAI, and by twigs
and leaves in equal parts, so the dense top shell thins before the sparse inner crown. Conifer
buds no longer die of shade (crowded young whorls under the tip left a bare band); they go only
by shedding. `get_foliage_profile(bins)` gives foliage reach per height slice for checks. Stand
crowns then held leaves only in their top few metres: buds were dying of shade a few metres under
the top (light below 0.05 in the linear model). Buds now die only in full darkness, as shade-
tolerant buds live on a few percent of open light (Larcher 2003), and weak branches still go by
shedding: forest beech crown depth 2.6 m to 8 m, open oak foliage from 2.8 m.

**LODs (stage 2, first part).** Each surface carries three index LODs that Godot picks per
instance by screen error (0.05, 0.2, 0.8 m): bark drops tubes under 1, 3 and 8 cm; leaves are
stochastically pruned to 50, 15 and 4%, survivors enlarged by 1/sqrt(keep) about their base so
the leaf area holds (Cook et al. 2007), each level a subset of the one before. Real leaves at
every level, never cards; the path tracer uses LOD 0. Open oak 7.4M triangles, then 3.0M, 1.1M,
0.3M; at 60, 250 and 600 m the LOD and full meshes look the same (lod_oak_60_250_600m.jpg).
Enlarged copies add about 70% to the leaf vertex buffer. Needles: a spruce is 9.8M needle
triangles and 53M vertices with LOD copies. Paulo's pick (11 Nov): real needles near, sprays
far. `needle_sprays` draws each shoot as two crossed sprays with a generated needle image
(SpeedTree-style fronds), hashed alpha with alpha to coverage (Wyman and McGuire, I3D 2017) and
coverage-preserving mips (Castano 2010): a 0.5 alpha cut made far sprays vanish once mips
averaged below it. Spruce 53M to 7M vertices; at 55 m as full as real needles. Stand and
woodland species always use sprays; `generate()` keeps real needles for a tree near the camera.
Under the path tracer the sprays want opacity micromaps (PC check).

**Eastern North America** (Paulo's pick, 11 Nov). Sugar and red maple, northern red and white
oak, eastern white pine, eastern hemlock, American beech, paper and yellow birch, witch-hazel and
mountain laurel. Each starts from its nearest European relative's growth rules and changes habit,
leaf size, bark and colour. Climate envelopes from Silvics of North America (Burns and Honkala
1990) where it gives figures, otherwise the USGS percentile atlas (Thompson et al. 1999). Stands
follow the SAF forest cover types (Eyre 1980) and Braun's forest regions (1950): northern
hardwood (sugar maple, beech, yellow birch, hemlock on moist ground), eastern oak (white and red
oak, red maple, mountain laurel thickets) and pine-hemlock. `make_woodland(variants,
REGION_NORTH_AMERICA)` joins them; the bench region map takes `--na`. Birch crowns were also too
wide (radius 12.8 m on a 23 m tree): `lateral_extension` 0.65 brings that to 7.

**Conifer cones.** Open-grown conifers spread too wide (spruce 39 m across). Scaling bud vigor
on branches starved the crown and it collapsed to a pole; scaling shoot length alone
(`lateral_extension`) narrowed it but left the young upper branches as stubs, so the top half
looked bare. What holds now is `crown_cone`: a branch reaches at most that many times its depth
below the leader's tip (Kantola and Makela 2004: spruce branch length grows with depth in the
crown, the crown keeping its shape). Shoots that would leave the cone wait for the leader, so
upper branches grow at full rate but stay short. Spruce and fir 0.3, larch and hemlock 0.35,
Scots and white pine 0.45, with a lighter self-shadow so the denser crown keeps its buds. Open
spruce: 33.6 m tall, crown radius 8.7 m, branches to the top; forest forms crown 3 m, base near
20 m. `lateral_extension` stays for broadleaves (birch 0.65). The bench renders with 8x MSAA:
needles are millimetres wide, and without multisampling they missed every sample and conifers
read as sticks; games run TAA or DLSS, which integrate that coverage.

## References

- Luft, T. An Ivy Generator. 2007. graphics.uni-konstanz.de/~luft/ivy_generator.
- Hädrich, T., Benes, B., Deussen, O., Pirk, S. Interactive Modeling and Authoring of Climbing Plants. Computer Graphics Forum 36(2), 2017.
- Benes, B., Millán, E. U. Virtual Climbing Plants Competing for Space. Computer Animation 2002.
- Harmon, M. E. et al. Ecology of Coarse Woody Debris in Temperate Ecosystems. Advances in Ecological Research 15, 1986.

- Lefebvre, S., Neyret, F. Synthesizing Bark. EGWR 2002.
- Wang, X., Wang, L., Liu, L., Hu, S., Guo, B. Interactive Modeling of Tree Bark. Pacific Graphics 2003.
- Kratt, J., Spicker, M., Guayaquil, A., Fiser, M., Pirk, S., Deussen, O., Hart, J. C., Benes, B. Woodification: User-Controlled Cambial Growth Modeling. Computer Graphics Forum 34(2), 2015.

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
- Campbell, *Extinction coefficients for radiation in plant canopies calculated using an
  ellipsoidal inclination angle distribution*, Agric. For. Meteorol. 1986. Iversen, *Viscum,
  Hedera and Ilex as climate indicators*, 1944. Breda, *Ground-based measurements of leaf area
  index*, J. Exp. Bot. 2003. Asner, Scurlock, Hicke, *Global synthesis of leaf area index
  observations*, Global Ecol. Biogeogr. 2003. Larcher, *Physiological Plant Ecology*, 2003.
- Burns and Honkala (tech. coords.), *Silvics of North America*, USDA Agriculture Handbook 654,
  1990. Thompson, Anderson, Bartlein, *Atlas of relations between climatic parameters and
  distributions of important trees and shrubs in North America*, USGS Professional Paper 1650,
  1999. Eyre (ed.), *Forest Cover Types of the United States and Canada*, SAF, 1980. Braun,
  *Deciduous Forests of Eastern North America*, 1950.
