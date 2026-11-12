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
- **Bone LOD**: a bone shorter than the error over 0.15 rad of bend rides its parent.
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
(a, b, radius in mesh space; transform by the placement).

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
  again, lifted by up to 1.8 cm of moss (hand-sized hummocks) where the weight is high and kept
  4 mm under the bark where it fades, so the edge needs no alpha; it enters furrows just ahead
  of the plates. Quads wholly under the bark are dropped. Its own surface "moss": a baked turf
  texture (shoots of about 3 mm in cushions of about 4 cm, domain-warped), matte.
- Aspect is not used: placed trees turn about their axis, so a north side baked into the mesh
  would point anywhere.
- Cost: oak open-grown 56k moss triangles on 4.2M of bark; stand-grown oak 19k on 0.8M; a
  class 3 oak log 190k on 0.18M.

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
- **Every limb welds.** The parent is up to six segments (two before the junction, three after)
  tested as a union of capsules. Each vertex marches out of the union and bisects to the bark.
  A weld fails only past the far end of the modelled parent. On the open oak no limb is skipped.
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
