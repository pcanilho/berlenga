# Mud: deep, sticky, drying ground under wheels and boots

Research note, 16 Nov 2026. Written before any code. The ask: do we have mud, and if not, mud that
deforms the way MudRunner and SnowRunner's does, researched across papers, engines, games and itch.io.
It doubles as a gap check for sand and snow: where a better method turned up, it is listed at the end.

## What we already have

Mud exists today only as wet soil, not as its own layer.

- **`GroundSurface3D`** (docs/ideas/ground-deform.md) blends soil to mud with moisture from the weather
  map or pore water: Bekker sinkage from sandy loam (n 0.7, kc 5.27, kphi 1515) to soft mud, rim share
  0.4 to 0.95 (mud squeezes up as a lip and barely compacts), repose 40 to 25 degrees, slow creep
  (Coussot 1997). Checks: a boot sinks 8.0 cm in mud; a pit keeps 63 degree walls after 10 s and creeps
  to 25 after 15 minutes. Turned ground is darker and glossier when wet.
- **Field.** 32 m around the camera at 6.25 cm, on the GPU (`gpu_simulation`), with a tile cache.
  Loose depth tops out at 0.3 m, so a rut can never be axle deep.
- **Terramechanics** (`soft_soil` on `PhysXVehicle3D`, `tire_pressure`, `ground_terramechanics.*`):
  Bekker static sinkage, Lyasko slip sinkage, compaction and bulldozing resistance, Janosi-Hanamoto
  traction, with soil shear values from sandy loam (c 1.72 kPa, phi 29) to clayey soil at 38% water
  (c 4.14 kPa, phi 13; Wong table 2.2). So a wheel in wet soil already slips and digs.
- **Water.** Pore water and drainage (`water_coupling`), the wet look (`ground_wet_map`, Lagarde 2013:
  darkening and a standing film on saturated ground), runoff routing that stops in hollows
  (`runoff_channels`), MPM grains (`PhysXGroundGrains3D`, kick()), contact wetness and puddles on the
  weather side.
- **Snow** has the same field plus a bow wave in front of ploughing bodies and crusts.

So the base is good: mass-conserving prints, real soil mechanics, pore water. What MudRunner has and we
do not:

1. Deep mud: a soft layer up to a metre over a firm base, so trucks sink to the axles and drag their
   bellies, and the depth is a property of the place (valley floors, tracks, river banks).
2. Mud that flows: rut walls and berms that sag back over seconds to minutes, ruts that fill and close.
3. Water in ruts: puddles in the tracks, drawn as water.
4. Mud on things: tyres clog and lose grip, then fling clods off at speed; bodies and boots cake;
   rain and water wash it; it dries lighter.
5. Splashes and clods: spray from spinning wheels, clods landing as splats on the ground and on bodies.
6. Drying: wet to dry over hours, then a cracked crust.
7. Fine detail: tread prints and sharp rut edges, which a 6.25 cm texel smudges.

## How others do it

### Games

- **Spintires / MudRunner / SnowRunner / Expeditions** (Saber; Pavel Zagrebelnyy, "Mud and Water of
  Spintires: MudRunner", Gamasutra blog, Nov 2017; 80.lv interview 2019; Saber's map modding docs). The
  quality bar, and the best documented:
  - Levels up to 1 x 1 km in 16 m blocks. Only blocks near the truck are active (plant, mud and water
    simulation); only blocks in view are drawn.
  - Per block a 25 x 25 RGBA texture (packed height, grass and dirt mix, a tint that also marks softer
    ground, and a "mud here" mask, the only channel that changes at run time), and only for blocks that
    have mud a 128 x 128 RGBA mud texture (12.5 cm): R mud height over the base terrain, GB a "mud
    offset" that slides mud aside from the wheels, A a tracks blend.
  - Physics on the CPU, drawing on the GPU, from different data. Each frame the CPU builds primitives
    from wheel and chassis positions, sizes, speeds and penetration and draws them into the mud texture;
    the GPU reads it. The author calls it "pure empirics". The wheel forces approximate mud when there
    is mud or the wheel spins fast; there is no published soil model.
  - Mud depth is painted (the editor's Mud brush; "Extrudes" adds hidden depth that only counts when
    the ground is wet, the "Extrudes To Wetness" setting), with marks for which vehicle class bogs.
  - Particles are decorative, simulated on a CPU thread; they float on the mud and can stick to wheels.
    Mud chunks are rigid bodies that break under wheels. Both, and the chassis, leave mud decals on the
    terrain and change per-vertex mud data on nearby assets (the trucks' dirt).
  - Wheel tracks are a separate trapezoid strip mesh along each wheel path with parallax from a height
    pass. That trick does not survive path tracing (no geometry), so we do not copy it.
  - Water: two swapped RGBA textures, the GPU spreading foam, height and "water-mud" to neighbours; a
    domain texture lets mud show through.
  - Lesson: the feel is sinkage, slip and drag on a deep, painted soft layer, with heavy cosmetic
    dressing (clods, decals, dirt on trucks). Our soil model is already more physical than theirs; we
    lack the depth, the flow, and the dressing.
- **BeamNG.drive.** No built-in deformation. The free "Terrain Deformation" mod (BrokenBrainCells, beta,
  Apr 2026) edits terrain blocks under tyres and buckets; collision reloads about a second later for the
  whole map. Its work in progress attaches dirt chunks to vehicles as real objects that should wash off
  in water. Shows the demand, not a method.
- **Red Dead Redemption 2** (Rockstar, 2018). Deep snow and mud with displaced trails, and mud and
  snow accumulating on horses, clothes and wagons as layered dirt masks. No talk published; the look
  reference for mud on bodies.
- **Off-road racers** (DiRT Rally, Forza Horizon, Gran Turismo): dirt on the car is a per-panel
  accumulation mask driven by surface and speed, washed by water. Cosmetic only.

### Engines

- **Unreal Engine.** No soil in Chaos. Ground prints are Runtime Virtual Texture plus Virtual
  Heightfield Mesh or Nanite displacement, sprays in Niagara. Fab plugins:
  - **TerraMech** (ZerstorenGames, UE 5.7/5.8, Oct 2026): a soil layer over any Landscape built only
    where vehicles drive, 16 m tiles of 12.5 cm cells; sinkage with spring-back, slip grip, compaction,
    bulldozing, soft tyres; five soils and eight profiles; ruts persist and harden; splatter, dust and
    water particles; server-authoritative for multiplayer. "Mud that clogs the tyres" is listed as next.
    SCM in a plugin: the same recipe as our stage 1 for sand.
  - **FluidNinja LIVE-2** (Andras Ketzer, Sep 2026, demo on itch.io): a real-time viscous 2D fluid in
    Niagara, shown with mud and blood. Not terrain, but a reference for the look of viscous mud spreading.
- **Unity / HDRP.** Terrain heights editable at run time; mud is render-texture stamps and tessellation.
  "Living Mud URP" (Asset Store, 2024) is a VFX and shader pack. No soil physics anywhere.
- **CryEngine.** Terrain layers with displacement and wetness; no deformation solver.
- **Frostbite.** Procedural virtual-textured terrain with displacement (Widmark, GDC 2012), craters
  baked into the height map. No mud flow published.
- **Godot addons.** Trail shaders (viewport textures) only. Interactive Surface FX (itch, assessed 10
  Nov) was not adopted. Nothing to adopt.
- **itch.io.** Rutted (ApeXPloit, Godot): a 2D off-road side-scroller with deep ruts, no method
  published. FluidNinja's demo (above). The rest is falling-sand voxels, as the sand note found.

### Papers

Mud as a material.

- **Bingham and Herschel-Bulkley rheology.** Mud is a yield-stress fluid: still under a yield stress,
  flowing above it (Coussot 1997, "Mudflow Rheology and Dynamics"). Yield stress and viscosity rise
  exponentially with the solid fraction: tau_y = a exp(b Cv) (O'Brien and Julien 1988, "Laboratory
  analysis of mudflow properties", J. Hydraulic Engineering), so a little water turns a firm track to
  slurry. Debris and mudflow models (FLO-2D, the depth-averaged Bingham models in geohazard work) run
  exactly this on a height field: a column flows only where rho g h sin(slope) exceeds tau_y, which
  leaves a rut wall standing when stiff and sagging when wet. This is the step up from our creep rate.
- **Graphics, viscoplastic.** Carlson, Mucha, Van Horn and Turk 2002, "Melting and Flowing" (SCA);
  Goktekin, Bargteil and O'Brien 2004, "A Method for Animating Viscoelastic Fluids" (SIGGRAPH); Yue,
  Smith, Batty, Zheng and Grinspun 2015, "Continuum Foam: A Material Point Method for Shear-Dependent
  Flows" (TOG; Herschel-Bulkley in MPM, the standard reference for mud-like MPM); "Fast Simulation of
  Viscous Lava Flow Using Green's Functions as a Smoothing Kernel" (SCA 2024), a viscous height-field
  flow cheap enough for interactive use.
- **Wet granular and sediment.** Tampubolon et al. 2017, "Multi-species simulation of porous sand and
  water mixtures" (SIGGRAPH); Gao et al. 2018, "Animating Fluid Sediment Mixture in Particle-Laden
  Flows" (SIGGRAPH); Su et al. 2023, "Real-time Height-field Simulation of Sand and Water Mixtures"
  (SIGGRAPH Asia; already the base of our sand stage 4). None is clay, but Su's saturation states map
  onto mud's water content.
- **Terramechanics in clay.** Wong, "Theory of Ground Vehicles", table 2.2: heavy clay, lean clay and
  clayey soil at two moistures each. In clay the friction angle is small and cohesion carries the load,
  so traction collapses as water rises; wet clay also clings (adhesion, Wong's ca term), adding drag
  that sand never has. Lyasko 2010 slip sinkage (built). Project Chrono SCM (built in spirit) and
  Chrono CRM (Unjhawala et al. 2025, arXiv 2507.05643, "A Physics-Based Continuum Model for
  Versatile, Scalable, and Fast Terramechanics Simulation"): continuum soil with active domains around
  vehicles, research scale but the same "simulate only near the wheels" layout.
- **Mud sticking and dripping.** Akinci, Akinci and Teschner 2013, "Versatile Surface Tension and
  Adhesion for SPH Fluids" (SIGGRAPH Asia): the adhesion force that makes a fluid cling to a solid.
  Useful for a close-up particle splash, too costly as the general answer. Tyre clogging is studied in
  agricultural engineering (tread fill reducing traction, self-cleaning with speed); a game needs only
  the fill fraction and the fling threshold.
- **Drying cracks.** Hirota, Tanoue and Kaneko 1998, "Generation of crack patterns with a physical
  model" (The Visual Computer); Federl and Prusinkiewicz 2004, "Finite element model of fracture
  formation on growing surfaces" (ICCS); Desbenoit, Galin and Akkouche 2005, "Modeling cracks and
  fractures" (The Visual Computer); Iben and O'Brien 2006, "Generating Surface Crack Patterns" (SCA).
  Physics of the pattern: polygon size grows with the drying layer's thickness ("Universal Scaling of
  Polygonal Desiccation Crack Patterns", arXiv 1807.06126); repeated wetting and drying turns T
  junctions into Y junctions and the polygons towards hexagons (Goehring et al. 2010, "Evolution of
  mud-crack patterns during repeated drying cycles", Soft Matter); plates curl up at their edges as the
  top dries first. A real-time version is a hierarchical Voronoi pattern whose cell size comes from the
  layer thickness and whose junction angles relax with each drying cycle, written as geometry.

Conclusion: no one ships mud flow; MudRunner ships depth, empirically shaped ruts, and dressing; the
papers give a depth-averaged Bingham flow for the ground, adhesion and clogging for wheels, and crack
patterns for drying. Our field already has the hard parts (mass, slumping, pore water, soil forces).

## What we would build (by stage)

### Stage 0: a finer near field (shared with sand and snow)

Tread prints, rut edges and crack plates need about 1.5 to 2 cm, not 6.25. A second, smaller field
nested in the 32 m one (8 m at 1.5625 cm, 512 squared) that follows the camera or the player's
vehicle, with the same GPU passes and the coarse field as its boundary; the patch mesh gains a matching
inner ring so the path tracer sees the geometry. This is the sand thread's pending offer; it serves all
three surfaces and comes first.

**Built.** GroundSurface3D `near_field` (on), `near_extent` 8 m, `near_resolution` 512 (1.5625 cm).
A second GroundDeformGPUHost runs the same passes on a field centred with the main one; every stamp
forwards to it, and direct presses update its CPU copy too. Slumping spreads like diffusion, so the near
field runs (texel ratio) squared slump passes, at most 16, each spending only its share of the owed time,
which keeps repose times equal to the main field's. Tiles entering the near field are seeded from the main
field's copy, so prints made far away arrive intact. The material blends the near map over the main one in
its outer 1 m (globals `ground_deform_near_map`, `ground_deform_near_rect`), the patch mesh gets a fine
ring at the near texel, and the path tracer reads the same map (rt params 70-73).

Found on the way:
- At 1.56 cm a print's rim showed as a square wall: the rim ring was half the footprint's equivalent
  radius. The ground's rim now spreads over at least one footing width (`GroundDeformField::rim_reach`,
  Prandtl's passive wedge beside a strip footing on undrained clay reaches about one width out), so the
  pushed soil rises as a broad lip. Snow keeps the old ring.
- The straight edge of the wet look came from pore water existing only on the patch's 0.25 m lattice.
  A far lattice (4 times coarser, as many cells, about 130 m) runs the same Green-Ampt balance from the
  water sources, an eighth of its rows per step, and copies the fine lattice where they overlap. The wet
  map carries both (fine block, far block, a row of rects); the fine block fades into the far one with a
  noisy edge, the terrain round the patch reads the far block in raster and path tracing (rt material
  bit `ground_terrain`), and cells entering the fine lattice start from the far one.
- Runoff and blown sand write their 12.5 cm and 25 cm cells as discs; the near field drew each as a
  round pit. Grid amounts now keep the main field's soft edge on the near field. Rills are still as wide
  as the runoff grid draws them: sharp rills need the runoff routed on the near field's own grid (sand
  thread).

Docs review fixes (prints frame):
- Prints were blocks. A box stamp marked `ground_sole` (meta on the body) is a boot: heel and forefoot
  ellipses in a smooth union, toe toward the longer half axis; the shank under the arch and chevron lug
  bars (7 mm, 45 mm apart: a finer tread aliases on the 1.56 cm grid) inside a solid rand raise the print's floor where the sole does not
  bear (`GroundDeformField::sole_sd()`, `sole_relief()`, the same in ground_deform.glsl).
- A rut changed shape at the near field's edge. The patch's coarse vertices sat on the main field's
  texel corners, where a sample is the mean of four texels: the whole main field was drawn with a 2x2 box
  blur, the fine ring without it. Inner vertices now sit on texel centres (half a detail step in, a whole
  number of fine steps so ripple cells still land on the ring's vertices); ruts and prints are sharper on
  the main field too. The hand-over band is 1 m, so a track entering the near field (which starts it
  fresh at its edge) does so where the main field still shows.
- Rims on the near field came out toothed: a body pressed once per frame (2.8 cm at 1.7 m/s) left one
  tooth per press, finer than the main field could show. Bodies are pressed once per finest texel
  (`GroundDeformField::stamp_step`).
- Second review: a hot white spot at every heel was the fresh-ground sheen on the print's curved wall;
  the sheen now stays on floors (raster and path tracer). A thin bright line along ruts was a lip
  peaked a texel wide at the wall's top; the ground's lip is now highest a little out from the wall.
  Tread shows with the sun above about 30 degrees: at 15 degrees a print's floor is all shadow.
- A moving body's pushed soil goes half to each side of its path, so a tyre throws up two even berms.
  A tyre narrower than two main texels still leaves rims that swap sides on the main field (aliasing of
  the rim ring); the bench tyre is now 25 cm wide.

Bench `--test=near` (6 checks, `--near=0` for A/B): a boot print's wall is 0.8 cm wide against 5.5 cm on
the main field, volume kept, dry sand still slumps to 35 degrees in 3 s, prints seeded on arrival. Slope
checks now measure on the finest field's texel; the print rounded off under flowing water is checked at
two thirds (the wider lip sits farther from the hole). GPU cost of the near field: PC queue.

### Stage 1: deep mud that flows (the MudRunner feel)

- **A mud layer.** Per lattice cell a soft-layer depth over the firm base (0 to about 1.2 m), from a
  `mud_map` the author paints (as MudRunner's brush) and from rules: wetness and pore water, flat low
  ground and valley floors from the terrain's flow accumulation, river banks and lake shores, tracks.
  Depth only counts when wet (MudRunner's hidden mud). The field's deepest dent follows it, so ruts can
  reach the axles.
- **Bingham flow** in place of the creep rate: per texel, a depth-averaged flux to each neighbour only
  where the driving stress (rho g h slope) passes the yield stress, at a Bingham viscosity, both from
  water content by O'Brien and Julien. Mass-conserving like the slumping it replaces; stiff mud keeps a
  rut, slurry closes it. GPU pass on the existing field.
- **Clay terramechanics.** Wong's clay values per moisture; adhesion drag on the wheel's contact arc;
  belly drag once the chassis meets the surface (a body in the `ground_deform` group already prints).
- **Field around vehicles.** MudRunner's active blocks: when a vehicle drives faster than the field can
  follow, a second field box follows it (or the field grows to 64 m on the GPU).
- Checks: a 4x4 sinks to its axles in 1 m of slurry and not in stiff mud; a rut in stiff mud keeps its
  walls for hours, one in slurry closes in under a minute; volume kept; traction against Wong's clay curves.

**Built.** GroundSurface3D `mud_layer` (on), `mud_map` and `mud_map_rect` (painted depth up to
`mud_max_depth`, 1.2 m), `data_map` and `mud_auto_depth` (0.5 m where a TerrainDataMap3D's wetness index is
high or within a few metres of water, on ground under about 10 degrees), counted only when wet between
`mud_wet_start` 0.35 and `mud_wet_full` 0.55. `get_mud_depth_at()`.
- The solid share runs from 0.56 where ground first turns to mud to 0.40 soaked; O'Brien and Julien's
  fits give the yield stress and viscosity (`mud_yield_stress()`, `mud_viscosity()`).
- Bingham flow rides on the field's slumping rather than a pass of its own: a wall stands up to the
  critical slope tau_y / (rho g h) for the mass that would move (about half the layer, at most 30 cm),
  and past it moves at the depth-averaged plug flow's diffusivity rho g h^3 / (3 mu). Mass is kept.
- Bearing and grip as clay: at the stiff end Wong's lean clay at 32% water (n 0.5, kc 13.19, kphi 692,
  c 4.14 kPa, phi 13 deg); slurry fitted so a boot sinks about 15 cm and a loaded tyre reaches the base.
  Nearly incompressible (97% of what is pressed squeezes up beside it); the deepest dent is the layer.
- Bekker on that clay gives a 1500 kg 4x4 on 26 cm tyres a thrust only just above its compaction
  resistance (about 0.16 of the load), and slip sinkage tips it over when floored: stiff mud is a crawl
  at part throttle, slurry bogs it to the axles. That is the MudRunner feel, from measured soil.
- Not yet: belly drag (the chassis rests on the collider at the undisturbed top, which carries it but
  does not drag), adhesion drag on the wheel, a second field box following a fast vehicle.

Bench `--test=mud` (10 checks): Bingham numbers, painted mud only when wet, boot 15 cm in slurry and
0.1 cm in stiff mud, a 25 cm rut in stiff mud keeps 70 degree walls for a minute while one in slurry
closes to 5 cm, volume kept, and with PhysX the 4x4 checks above.

### Stage 2: water in ruts, drying and cracks

- Ruts collect rain and runoff (the runoff routing already stops water in hollows): a water depth per
  cell, drawn as a thin water surface in the ruts (real geometry, shadowed, path traced), which also
  softens the mud under it.
- Drying by evaporation from the weather: albedo lightens, gloss goes, then a crust. Once the crust is
  thin and dry, desiccation cracks: a Voronoi pattern sized by the drying layer's thickness, opened as
  geometry in the near field, curled plate edges; rewetting closes them, and each cycle rounds the
  pattern towards hexagons (Goehring 2010).

#### Built (2a: standing water)

- **Technique.** Shallow water on the print field itself, by the virtual pipe model (O'Brien and Hodgins
  1995; Mei, Decaudin and Hu 2007; Št'ava et al. 2008), the method of most GPU terrain water (and of Fluid
  Flux in Unreal). Per texel a depth and four outflows; an outflow grows with the head difference to its
  neighbour, the four are scaled so no texel sends more than it holds, depths change by the net flow. Two
  compute passes per substep in `ground_deform.glsl` (`MODE_WATER_FLUX`, `MODE_WATER_DEPTH`), on the main
  field and the near field alike; substeps from the shallow-water wave speed for water up to 20 cm (at most
  4 a frame), light damping so pools settle in a second or two. The water rides with the field: tile moves,
  the GPU tile cache and the near field's seeding all carry it (the cache's spare half).
- **Sources and sinks.** Rain from the `WeatherMap3D`; soaking in at the cell's saturated conductivity
  (clay-rich soil 1e-6 m/s to sand 1e-4 m/s, Rawls et al. 1982; litter fast; puddled mud a third; pressed
  ground a third again, as tyres close the large pores, Horn et al. 1995), so heavy rain on clay ponds
  (Horton overland flow) and sand never does. Ground water seeps in (Darcy over 2 cm) wherever a print
  reaches below the water table: soaked ground has it at the surface, ground at mud's wetness a few
  decimetres down, a beach its sea's (`runoff_channels`). Evaporation by Penman's open-water wind function
  (as the weather map's film). `runoff_time_scale` speeds the sources and sinks up for benches.
- **Drawn as real geometry.** The ground map's channels are now the net depression (dent less rim) and the
  water depth; the patch's vertices rise to the water's level, so the water is the surface there, flat, in
  both renderers and in shadow. Its look (`ground_water_look()`, material.cpp and the raygen alike): water's
  F0 (0.02) and roughness 0.03, no relief, the bed under it soaked (Lagarde 2013), and rut water carries clay,
  so the bed fades out within about a centimetre (Beer-Lambert, e-folding 1 cm) to a darker soil colour.
  Clear water over gravel would want transmission; rut water is turbid, so this is the right default.
- **Coupling.** Standing water saturates the ground under it (the pore model), which softens the mud there.
- **Not chosen.** PhysX MPM (particles) for this: millimetre to decimetre films over 64 m2 would need millions
  of particles; it stays for splashes (stage 4). No GPU: no standing water (the CPU field keeps prints only).
- **Checks** (`--test=puddles`): a 12 cm rut in soaked soil fills to the 6 cm water table and stands level;
  a new print cut into the pool draws water in and the volume holds; rain gathers in a rut, the flat soaks its
  sheet up first after the rain, the rut empties within a day.

#### Built (2b: drying and cracks)

- **Drying.** Per lattice cell, clay-rich bare ground (not sand, litter, rock, snow) soaked by standing water or
  to saturation keeps the share of water its drying layer still holds; once the ground round it is no longer
  wet (moisture under `mud_wet_start`) it loses it to evaporation (Penman, as the standing water) at 40%
  porosity. The layer is the soft top the water worked: about 6 mm where the ground was only saturated, plus
  0.6 of the deepest water that stood on it (to 5 cm), thinner in sandier soil. So a dried puddle's thin rim dries
  first, into small plates and fine cracks, and its deep centre stays dark with water longer and cracks into
  large plates; a layer under about 1 cm (ground only saturated) shows none. `runoff_time_scale` speeds it up like the rest.
- **Cracks in generations** (first version, Voronoi at two overlapping scales, read as a drawn net: replaced).
  Past a third of its water lost the top cracks, fully open at four fifths, plates about four times the layer
  (Groisman and Kaplan 1994). Hierarchical domain division (Bohn, Pauchard and Couder 2005, PRE 71 046214/5):
  first-generation cracks meet at about 120 degrees (Voronoi cells of a jittered hexagonal lattice, Quilez's
  two-pass border distance); each domain splits across its long axis (a line through its seed towards its nearest
  neighbour, so square to the shared edge: T junctions); each half splits again square to that, the third
  generation free to end short and taper out. Widths share the clay's ~8% linear shrinkage (0.5, 0.3, 0.17 of it)
  and open in turn as the layer dries; depths 1, 0.75, 0.45 of the layer; widths wander along each crack and the
  whole pattern meanders (a smooth warp). Plate rims curl up to 35% of the layer over an eighth of a domain
  (Allen 1986). Wetting closes them and counts a cycle; with cycles the jitter and the later generations fade,
  so the plates go hexagonal with Y junctions (Goehring et al. 2010; Goehring 2013, "Evolving fracture patterns").
- **Drawn as geometry first.** A third block in the ground wet map (opening, plate size, regularity, crust or
  minus dampness) read by `ground_cracks()` and `ground_crack_height()` in material.cpp and the raygen alike. The
  height is box-filtered to a footprint: a crack narrower than it becomes a dip of the same volume. The patch's
  vertices take it filtered to 1.5 vertex spacings (`ground_deform_top()`: real walls and rims in the 1.6 cm near
  field, which PT shadows); the shading adds only what the mesh cannot carry: the slope difference between the
  pixel's footprint and the mesh's, and a slot's sky view w / (w + 2 d) on the unresolved share. The dry crust is
  pale (x1.7, 15% desaturated) and matte, the crack walls the darker soil under it; the drying layer is
  dark and a little glossy while damp.
- **Checks** (`--test=cracks`): soaked clay stays whole; dried, it cracks, plates 4x the layer and finer on the
  flat beside the pool; rain closes the cracks; the next drying cracks again, more regular; sand never cracks.
  `--view=ruts --water=mm --dry[=frames]` shoots it (fewer frames: part dried, centre still damp) and prints the
  deepest pool for `--cam`.

### Stage 3: mud on wheels and bodies

- **Tyres clog.** Per wheel a tread fill fraction, filled by slip in sticky mud, lowering traction
  toward a slick's; flung off when the tread's centripetal acceleration beats the mud's adhesion, which
  spawns clods (stage 4). Mud mass adds to the wheel.
- **Bodies cake.** Per mesh a mud coverage mask in texture space, filled by contact and landing spray,
  drying lighter over time, washed by water depth and rain. One material layer (wet mud over the base
  material, like the snow layer), the same in raster and path traced.
- Characters: boots and legs pick up mud the same way (this was "leg caking", not picked for the ground
  in November; offered again here).

#### Plan (3a: wheels and belly, physics)

- **Stickiness.** Clay sticks to rubber and steel most between its plastic and liquid limits and least dry
  or as slurry (Fountaine 1954, "Investigations into the mechanism of soil adhesion"; Ren et al. 2001 on
  soil adhesion to machines): per cell a stickiness 0..1 from moisture and clay share, and a tangential
  adhesion ca up to ~5 kPa, the ca of Wong's rubber-soil interface (Wong 2008, sec. 2.5).
- **Clogging.** Per wheel a tread fill 0..1: filled per revolution by slip in sticky soil, shed when the
  tread's centripetal acceleration w^2 r passes what adhesion holds on a lug-deep layer, ca / (rho t) (with ca
  5 kPa, 1700 kg/m3, 2 cm lugs: about 150 m/s2, so a wheel spun past ~7 m/s of tread speed cleans itself, the
  "spin it clean" every MudRunner player learns), washed in standing water, falling off slowly on firm
  ground. A full tread grips as a slick: its shear limit falls from A c + W tan phi (soil on soil through the
  lugs) to A ca + W tan delta (rubber on clay, delta about 0.6 phi). Mud mass adds to the wheel's inertia.
  The shed mass is kept for stage 4's clods.
- **Adhesion drag.** Sticky clay holds the tread as it leaves the rut: ca over the trailing face (width times
  sinkage) against the travel, added to compaction and bulldozing.
- **Belly drag (high-centring).** Once the chassis' underside meets the mud surface: a grid of points on the
  lowest face of its collision shapes; where one is below the surface, Bekker's plate pressure k z^n (plate
  the belly's width) lifts it (taking load off the wheels: high-centring), Mohr-Coulomb shear against its
  slide (A ca + N tan delta, smooth steel), bulldozing at its leading edge, and the belly presses a trough
  into the field. Damped so the stiff contact does not ring at 60 Hz.
- Settings on PhysXVehicle3D: `tyre_clogging`, `belly_drag` (both on). Checks: `--test=clog` (fills in sticky
  clay, grips less, cleans when spun fast, washes in water), `--test=belly` (a low car bogs and leaves a
  trough, more drag with than without).

#### Plan (3b: mud on the body, the look)

- **How others do it.** Saber (MudRunner, SnowRunner) keeps per-vertex mud data on the trucks, raised where
  chassis, wheels and particles touch mud. Codemasters (DiRT Rally) and Playground (Forza Horizon) blend authored
  per-car dirt masks by an accumulation level, dust then mud. BeamNG's "Grit" and "dynamic dirt" mods put
  triplanar dirt over any vehicle with a height gradient from the bottom up. Assetto Corsa's CSP dirties tyres
  by surface. The common core: an accumulation level per region thresholding a noise or mask, so mud spreads as
  splats and grows from where it lands, never as a uniform tint.
- **Ours.** No authored masks (imported cars and Tripo meshes have arbitrary UVs) and no per-vertex data (the path
  tracer would need it on every mesh): the mask is computed in the vehicle's own frame from a few numbers the
  vehicle updates each tick, so the same lines run in BaseMaterial3D's raster shader and in the path tracer.
  - Tyres: within each wheel's radius, the tread fill of 3a.
  - Wheel spray: round each wheel and up its arch, rearward, from the mud that wheel has thrown (3a's shed kg),
    accumulated and washed by standing water.
  - Belly line: from the chassis bottom up to the deepest the body has sat in mud, ragged.
  - Splats: each level thresholds an object-space noise (two octaves), so low levels are scattered flecks and
    high levels a cake; a little relief from the same noise (normal only, both renderers).
  - Wet mud dark and glossy, drying in minutes to a pale matte crust (the crack crust's tone).
- **Wiring.** BaseMaterial3D gets a Mud layer (`mud_enabled`, off per material, like Snow) reading instance
  uniforms at fixed indices; the path tracer reads the same slots from the global uniform buffer.
  PhysXVehicle3D `body_mud` (on) turns the layer on in copies of its meshes' materials and writes the uniforms.
  Check: raster and path traced at 100% crop of a car after a run through clay.

#### Built (3b)

- BaseMaterial3D `mud_enabled` (Mud group), `get_mud_functions_code()`: instance uniforms at fixed slots 0-10 (the
  vehicle's world-to-frame rows, four wheels' centre and radius, tread fill, spray, belly (bottom, line, wetness),
  clay colour), `vmud_at()` and `vmud_apply()`; the raygen has the same lines and reads the slots from the global
  uniform buffer at the geometry's `instance_uniforms_ofs` (that buffer is now a runtime array, it was declared 256
  long). Material bit: subsurface bit 31.
- PhysXVehicle3D `body_mud` (on): copies its meshes' BaseMaterial3D materials with Mud on, then each tick writes the
  uniforms. Spray grows by the shed clay (12 kg to full) and by ploughing sticky slurry, washes in standing water; the
  mud line is the deepest the belly or the mud at a sunk wheel reached on the body; wet 1 while mud lands or water
  stands, drying over 5 minutes. Getters: `get_wheel_mud_spray`, `get_body_mud_line`, `get_body_mud_wetness`.
- Bench: `--view=bodymud` (a dressed 4x4 crawls through stiff clay, then is floored); `--test=clog` checks the spray,
  the body material and the mud line of a car sunk in slurry. Raster and path traced match at 100% crop.
- Next for the look: splashes stretched along their flight from the wheel (streaks), and the clay's colour taken
  from the ground it came from.

### Stage 4: spray and clods

- GPU ballistic particles from each slipping wheel (speed from the wheel's surface speed, mass from
  the mud lifted): droplets and blobs as small meshes, not sprites, so path tracing keeps them. Landing
  writes a splat into the ground field and into a body's mud mask.
- Large clods as PhysX rigid bodies that break up, as MudRunner's chunks; MPM grains (built for sand)
  for wet mud ploughed by a blade or a bucket, with a cohesive Drucker-Prager setting.

#### Built (4a: mud spray on the grains node)

- **PhysX first.** Flow is gas (dust and smoke); it already carries the dust of dry ground (GroundDust3D far,
  PhysXGroundDust3D near). Liquid mud and clods are PhysX MPM: the sand roost's PhysXGroundGrains3D (Holz, Beer and
  Kuhlen 2009 hybrid: dug out of the field, flown as MPM grains, deposited where they land, volume kept) already
  takes every wheel's spray on any soft ground. Mud needs its own material on it: heavier, cohesive (the
  Drucker-Prager cohesion term), drawn as wet clods rather than sand clouds.
- **Routing.** Each spray carries the soil's stickiness; a grains node routes sticky sprays to a mud child it makes
  (`mud_spray`, on): clay, 1700 kg/m3, friction 20 degrees, cohesive, 3.5 cm particles drawn as clumps of two or three
  lumpy blobs in a wet clay material (real geometry, so path tracing keeps them). Landed clods go back into the field
  as the sand does: splats round the car.
- **Clods off the tyre.** The clay a clogged tread sheds (3a's shed kg) leaves from the rear of the tread, up and back
  at the tread speed (the rooster tail of a wheel spun clean), as a spray with its own volume (not dug: it came out
  of the tread).
- Checks (`--test=clog`): floored out of the clay, the shed clay flies as mud clods and lands, volume kept.
  `--view=bodymud --spray` frames them mid-throw; raster and path traced match. Sticky clay throws a quarter of
  sand's tread depth, and landed clods splat over 12 cm (stiff mud would otherwise keep each texel's heap as a spike).
- Next: clods of several sizes, wet clods sticking to the body they hit (they pass through it now: the grains have
  no colliders), slurry splashes in standing water.

#### Research (4b: clods sticking to bodies)

- **How others do it.** Saber raises per-vertex mud data on the truck where a particle touches it. Splatoon-style
  games paint hits into a texture in the mesh's UV space (render-target unwrap); Unity's skinned-mesh decals and
  CryEngine's mud-splatter decals project a decal at the hit. MSFS 2024 blends authored dirt and grime layers by a
  level. Our body mud is computed in the vehicle's frame with no UVs, so hits are best kept the same way.
- **Proposal.** The grains have no colliders; the mud child tests each live clod against every PhysXVehicle3D's
  chassis box (grown by the wheel radius). A clod inside it sticks: it is killed, its volume kept as a splat in that
  vehicle's frame (position, radius from volume, wetness). The vehicle keeps a ring of splats and publishes them as
  a small texture shared by all vehicles (one row of RGBA float texels per vehicle, its row index in a spare
  instance-uniform slot), read the same way in raster (a global sampler) and in the raygen (bindless). Each splat
  raises the mud level round it with a radial streak along the clod's flight. Clod sizes: three meshes (2, 3.5,
  6 cm) chosen by slot.
- Checks: clods thrown at a parked car stick, volume kept (dug = air + landed + stuck); raster vs PT crop.
- **Built.** As proposed, with these choices: the splat texture is 64 x 16 RGBA float (32 clods a vehicle, 16
  vehicles), published as the global `vehicle_mud_splats` and to the path tracer as rt_param 74; the row and count
  are instance-uniform slot 11. A clod landing within half a radius of a splat merges into it (it widens), so the
  ring is not filled by one spot. Splats have a ragged rim and a halo of droplets (the splat noise thresholds them),
  and wash off in standing water. Clod sizes: the MPM render already scales each slot 0.75 to 1.25 and tumbles it.
  `PhysXVehicle3D.add_mud_splat()`, `get_mud_splat_count()`; `PhysXGroundGrains3D.get_stuck_volume()`. Bench:
  `--test=clog` (a kick at a parked car's side sticks, volume kept), `--view=bodymud --splat`.
- **Fixed after review.** Splat outlines stair-stepped and the splats read as flat paint. The cover is now a filtered
  step (the analytic antialiasing of procedural textures, as in Quilez's "filtering procedurals" and shader-side
  `fwidth` edges): the field is splat + 1.2 level, and its edge ramp (0.03 at least, so it stays crisp where the field
  rises slowly) widens to the field's change across one and a half pixel footprints (the same footprint in raster and
  in the path tracer's cone), so outlines stay smooth at any distance. The finite differences now also move the level,
  so the relief has the splat's own edge: a dome deepest in the middle, a lip just inside the rim where the crown
  piles up, lumps, and a step at the edge. Wet clay is darker and glossier (roughness 0.18). Drip runs sag down the
  panel the clod hit, a few narrow runs of different lengths that thin to their ends (none from roof or floor hits).
  Bench: `--pair` saves the raster and path traced frames from one run, so both show the same clods.
- **Second review.** The runs read as syrup and the clay as plastic. Drips are now short and fat, since wet clay is
  viscous: in lanes about 3 cm wide across the panel, about half of them with a run hanging from the rim above it, 2
  to 6 cm long whatever the splat's size, wandering a little, lumpy and ending in a bead. The cake has aggregate (grit
  of a few mm and the odd pebble lump in its relief, fading with the footprint), a mottled tone over a few cm with a
  grit speckle, and a thick middle that stays wet (darker, glossier) after its thin edges dry; grit breaks up the wet
  sheen. Overlapping splats and runs join as an "or" of their shares of a full cake (a + b - ab), so no seam or ridge
  shows where they meet (a plain or smooth maximum leaves one). The relief's step follows the footprint (0.8 to 3 mm).
  #### Research (4c: splashes in standing water)
- **How others do it.** Saber keeps water as two swapped RGBA textures that the GPU spreads (height, foam,
  "water-mud" carried to neighbours), with decorative CPU particles on top. The standard real-time recipe is a height
  field for the bulk and particles where it breaks: Chentanez and Muller 2010 ("Real-time simulation of large bodies
  of water with small scale details", SCA) spawn spray particles where the field's surface is steep or fast and
  return them to it on landing; Holz, Beer and Kuhlen 2009 do the same for soil (our grains). Engineering wading
  studies (GPU SPH, e.g. the vehicle wading papers on arXiv; Particleworks with RecurDyn) show the bow wave and side
  sheets of a wheel and a body, a reference for shape, far too costly as the answer.
- **Ours.** The standing water is already a GPU virtual-pipe field (2a). A wheel in it pushes a bow wave into the
  field (a moving source, volume kept) and, past a Froude number of about 1 (speed over sqrt(g depth)), throws sheets
  off the tread and the sidewalls: water taken from the field becomes MPM fluid particles (the clear-water solver
  PhysX already runs, foam and spray on), coloured by the mud the field carries, and lands back into the field.
  Muddy water also lifts the body's spray level and washes the body. Rings in the field from the bow wave.
- **Dependency.** Sand owns the water passes and WaterCell for rills this week: build after they land, through
  the field's own source term, not a second solver.

#### Plan (4c)

- **Bow wave.** A tyre is an obstacle in the pipe model (as moving obstacles in virtual-pipe water, e.g. Mei, Decaudin
  and Hu 2007 and later game water such as From Dust): water cannot stand inside it, so where its disc dips below a
  column's water that water's head rises by the thickness it takes up, and the pipes drive the water out from under
  it. As it rolls on its front keeps raising the head ahead, which runs off as a bow wave, and the water closes in
  behind; the flux is limited by the depth, so volume is kept by construction. (A first try added the rate the
  submerged volume grows as a source, after Chentanez and Muller 2010; on a height field without the body's own volume
  it fed on its own hump and gained water.) Vehicles report wheels in water through GroundSurface3D.add_wader().
- **Sheets.** Past a Froude number of 1 at a wheel (speed over sqrt(g depth)), the tread and sidewalls throw sheets: a
  GroundSurface3D spray with a water share, which a grains node throws as PhysX MPM water (its clear-water child, foam
  and spray on, tinted by the mud the field carries), taken from the field there and returned to it where it lands, as
  the clods are.
- **Sheets: research.** Tyre spray studies (Weir, Strange and Wheeler 1978; Pilkington 1990; Kuemmel et al. 2000) split
  a tyre's water into a bow wave, side waves thrown out and up from the contact at 20 to 40 degrees, and tread pickup
  flung off the rear of the tread; the side waves take most of it once the speed is past the water's wave speed
  sqrt(g h). In height-field games and films the same split is made by turning height-field water into particles
  where it cannot stay a height field (O'Brien and Hodgins 1995, "Dynamic simulation of splashing fluids"; Thuerey,
  Muller-Fischer, Schirm and Gross 2007; Chentanez and Muller 2010; Unreal's Niagara water and Waterline Pro do it
  with authored sprites), and back where the particles land, keeping the volume.
- **Sheets: build.** Per wheel in water deeper than 5 mm with a Froude number over 1, a share of the water its
  contact sweeps (width x depth x speed, a third at Fr 2 rising to most of it, Weir's curves) is taken from the
  field under the tyre and thrown as two side sheets, plus tread pickup off a slipping tyre. GroundSurface3D
  add_water() queues signed discs that the GPU water pass adds or takes (the area counted in the field's own texels,
  so volume is exact; a take is held to half the depth read back there); the sprays carry the water's volume to the
  grains node, which throws it as a third child, MPM liquid (not Drucker-Prager), drawn as clear water with its spray
  and foam; drops landing on the field go back through add_water.
- **Built (sheets).** As planned, with: only the leading wheel of a track throws (a rear wheel meets water its front one
  has thrown); the take disc sits just ahead of the tread, sized to half the water it sweeps into that step by the
  read back's own sum over the disc (get_standing_water_volume); a GPU ledger (binding 14, copied after the packed
  field) counts what discs added, took and could not find, the shortfall paid back from the next water added, so the
  books close exactly (--test=splash: a 4x4 at 3.7 m/s in 2.4 cm ruts throws about 10 l, all of it lands back). The
  read back trails the GPU (15 Hz and the field's moves), so about half of what is asked is found and the rest is
  borrowed until the drops land; thin films landing on the dry flat soak in (the field keeps films over 0.02 mm).
  The liquid's drops are tracked through the solver's world transforms (their basis carries the drop size, which the
  grains' check took for dead slots). Drawn as the solver's clear water; thin flying sheets need the clear spray
  drops look (PhysX thread's card) to read, and muddy water a tint.
- **Body.** Muddy water lifts the arches' spray level and washes stuck clods (2a already washes them in standing
  water).
- **Built (body).** Water washes the body below its line (the highest water around the wheels, plus a few cm for the
  bow wave riding up), faster as it runs past (rate x (1 + speed/2)); above the line only splash, falling off over
  10 cm. Clods under it go and the belly's mud line drops to it. A wheel's own sheets rinse its arch.
  PhysXVehicle3D.get_mud_splat(); --test=splash checks a clod under the line washes and one on a door stays.
- Checks: a wheel driven through a puddle raises water ahead and lowers it behind, total volume kept to 1%; past
  Froude 1 water is thrown and lands back, volume kept; raster vs PT frame of a 4x4 through a rut puddle.

#### Research and build (4d: streaked spatter and local clay colour)

- **How others do it.** Forza Motorsport (2023) builds each car's dirt live, thrown by its own tyres and the track,
  so it gathers where the flow carries it; MSFS 2024 blends dirt and grime layers by level and region; BeamNG's dirt
  mods and Codemasters use authored directional masks. Real spatter is elongated along its flight, with a tail away
  from the source, and wet mud runs down in drips that dry in place.
- **Ours.** Stretch the splat noise along each wheel's spray direction in the vehicle frame (anisotropic sampling:
  the noise coordinate compressed along the flight from the wheel), add a tail streak and, while wet, short drip
  runs downward that freeze as the mud dries. Clay colour: the vehicle averages the ground's albedo (GroundSurface3D
  tint and material colour) at its wheels' contacts over the last metres driven and writes it into `mud_color`, so
  red laterite gives red mud and grey clay grey.

- **Built.** Spatter is drawn out along its flight: the splat noise is sampled with the direction out from the nearest
  wheel squeezed out of it (features nearly twice as long along it), plus a faint vertical grain where it ran. Clay
  colour: `GroundSurface3D.mud_color` (sRGB, a mid brown) or `mud_color_map` over `mud_map_rect`, read by
  `get_mud_color_at()`; a vehicle's treads pick it up where they turn in mud, blended over about 5 m of travel, into
  the body's `mud_color` (`PhysXVehicle3D.get_body_mud_color()`). Check: `--test=clog` drives through red laterite and
  the body's clay comes out red.

#### Research (4e: mud on boots and legs)

- **How others do it.** RDR2 layers dirt, mud, snow, blood and wet masks on characters and horses, raised by
  region from contact (no talk published). FFXV's character effects (Flagrum wiki) keep per-region dirt and wetness.
  CryEngine's Dirt Layer and UE's forum recipes paint a mask by overlap. Arkham Origins' deformable snow (GDC 2014)
  pairs it with the ground's prints.
- **Ours.** The same BaseMaterial3D Mud layer: a mud line rising from the soles by how deep the boot sank (the boot
  prints already know, meta `ground_sole`) and spatter up the shins from walking speed. Skinned meshes need a stable
  frame: the vertex's rest position (baked into CUSTOM0 at import), read by raster and by the path tracer's captured
  vertices, so the mud does not slide as the legs move. Open question: the path tracer reads per-vertex custom data
  only for some meshes (a known limit on MultiMeshes), to check before building.
- **Plan (4e).** Instead of rest positions (a mesh attribute every character would need, and per-vertex data the
  tracer reads only for some meshes), the mud is laid in each shin's own bone space, as skeletal decals are attached
  to a bone (UE's skinned decals, the bone-space blood masks of fighting games): per leg the sole and knee points from
  the skeleton make a frame (along the shin, across it towards the character's facing), the fragment is taken into
  the nearer leg's frame, and the level is a mud line measured up the shin from the sole, ragged in that frame, with
  flecks above it from walking speed. The frame moves with the bones, so the mud stays on the leg through the walk
  cycle; the shader is the vehicle's Mud layer with a legs mode (mud_body.w), so raster and path tracer share it.
  A GroundLegMud3D node on the character finds the foot and lower leg bones, raises each leg's line by how deep its
  foot sank in mud (the field's dent and mud depth there) and splashes with speed, wets and dries it as the body
  mud does, washes it below standing water, and feeds the uniforms of the character's meshes (Mud turned on in copies
  of their materials). Checks: walking through mud raises the line to about the sinkage, a walk on dry ground leaves
  none, standing in water washes it; raster vs PT of a leg mid-stride.
- **Built (4e).** GroundLegMud3D (modules/ground) as planned, with sole and knee marker nodes as an alternative to
  bones. A planted foot (sole within 5 cm of the surface) on sticky or muddy soil raises its line to the deeper of
  the slurry's depth, the Bekker sinkage of a boot (40 kg on 0.03 m2) and the sole's own depth under the surface,
  plus 2 cm; flecks follow walking speed (full at 4 m/s) times the soil's stickiness. Water over 3 cm deposits
  nothing (held 0.5 s, as the field's read back can miss a frame while it recentres) and washes the leg below its
  surface at the body's rate. Bench `--test=legs` (dry ground clean, 15 cm slurry coats to 17 cm, running throws
  more flecks than walking, the Mud layer is on in legs mode with the local clay, wading 14 cm of water rinses an
  8.7 cm line to 1.2 cm) and `--view=legs --pair` (raster and PT keep the same line, lumps and grit at 100% crop).
  Limit: the shader's coverage is a cylinder round the shin plus a boot block 14 cm long, so very wide trousers
  or long boots stay partly clean; a docs image needs a real rigged character.

### Next round (picked 18 Nov: all three, in order)

- **Muddy splash water.** Natural water is never clear: suspended sediment both absorbs and scatters, so turbid
  water turns brown and opaque within a few cm (USGS Water Science School, "Water color"; turbidity and total
  suspended solids, Fondriest). Build: a suspended sediment channel in the standing water field, stirred up by
  waders, feet and landing spray in proportion to the excess shear on the clay bed (as the body wash), carried by
  the water flux and settling out at a Stokes rate for silt (clear in minutes). The water's look reads it as
  Beer-Lambert absorption plus a single-scatter albedo of the local clay colour, so raster and the tracer agree
  (the same Transmission path every water body uses; one water technique). Sheets and drops thrown carry the
  concentration where they were taken; landing on a body they leave a thin stain splat (the 4b splat with a
  low volume and no relief). Check: a stirred puddle browns and clears again; sheets show against the sky.
- **Built (muddy water).** `muddy_water` (on): a third vec4 per water texel holds the clay (kg/m2) and its
  concentration as the substep began; the pipes carry it upwind at the water's rate (exact, as the depth), tyres on a
  clay bed (conductivity under ~3e-5 m/s) churn 2 kg/m2 as they cross a texel, `stir_water` discs add more (boots:
  0.1 kg a metre walked), and it settles at 0.1 mm/s; a texel that dries drops it. Read back per texel
  (`get_water_mud_at`), drawn through the blit's A channel (negative under muddy water, where the water hides the
  turned ground's gloss). `ground_water_look` gains the concentration: Beer-Lambert in it (unchanged at none) and a
  slurry albedo with the pore water's darkening undone. Tyre sheets take the concentration where they are taken;
  the water spray child keeps the mean of what is in the air, lands it and tints its liquid material (attenuation
  over 0.03 / (3 + C) m, clay albedo and less transmission past some tens of kg/m3); a vehicle's sheets stain its
  arch toward a film set by the clay instead of rinsing it. Bench `--test=muddy` (clear seepage, a tyre churns
  ~15 kg/m3 into 14 cm, stirring matches mass over volume, landing water brings its clay, settling follows
  exp(-w t / h), sand stays clear) and `--view=wade --pair --cam=2.0,1.3,-4.2,0.8,0.0,-2.0` (`--prop=muddy_water:false`
  for the before). Not yet: the clay settled on the bed is not added back to it (grams), and the cache drops it.
- **Mud tracked onto firm ground.** Construction stormwater rules size tracking pads at about 15 m because a tyre
  sheds most of its carried mud over the first few tens of metres (Minnesota PCA vehicle tracking BMPs; NCDOT
  RP2023-13 on mud on highways); light-vehicle tyre studies on unpaved roads measure the material thrown around the
  tyre (SAE 2024-01-5032). Build: the clogged tread (get_wheel_mud) and the leg mud lose a share each revolution
  or step on firm ground, printed as a thin mud layer with the tread or sole pattern, decaying with distance;
  printed mud is a mud depth in the field (so it dries and cracks as any other) or, off the field, a decal.
- **Built (tracked mud).** `tracked_mud` (on): `track_mud` lays a film as a deposit whose fresh tint value is a band
  from 4 up (share in sixteenths, wetness in the fraction; 2 to 4 stays the sand's mineral band), on both the CPU and
  GPU field. Out of sticky soil, a vehicle presses part of what its tread sheds under the tyre, `1 - tread/8 m/s`
  of it (at least a fifth), the rest still flies as clods; boots print heel and forefoot at each footfall on firm
  ground, film `1 mm * line / 15 cm`, and lose 15% of their line a step. Presses keep the film on top (it was the
  fresh-dug tint before); digs and standing water take it. The tint is mixed toward the wet clay or its dried pale
  tone by its share; it dries with the ground's freshness clock. `get_tracked_mud_at` reads the nearest texel.
  Bench `--test=tracked` (prints, fainter as the boots clean, dries in place), `--test=clog` (a rolling tread
  leaves film along each wheel's path), `--view=tracked`. Not yet: tread or sole pattern in the print; mud depth
  for thick dumps (it is a tint over the ground, not relief).
- **Ruts kept across the world.** MudRunner and SnowRunner keep their deformation in the map's save; ours lives
  in a session tile cache. Build: write touched tiles (height delta, mud depth, water, crack state; 16-bit,
  zstd) to user:// as they leave the cache and on save, read them back when the field recentres onto them, with
  an LRU budget. Matters most for the open-world game's scale.
- **Built (ruts kept).** `keep_ruts` (on), `ruts_path` (user://ground_ruts): as the main or near field moves, a
  leaving tile with prints is written in the cache's own layout (two half pairs a texel: dent and rim, tint and
  water) as one zstd file named by field texel and world tile; a tile flattened since loses its file. Arriving tiles
  with a file that the GPU cache does not hold are read the step after the field stands: seeded into the cache and
  pulled in place (GroundDeformGPUHost::pull_cached), so the startup reset is covered; the CPU path writes them
  straight in. `save_ruts` on leaving the tree (and for the game's own save), `clear_saved_ruts`. Off in the
  editor. Bench `--test=ruts` (back after 600 m and in a new node, a cleared field drops its files, off starts
  flat; also with `--prop=gpu_simulation:false`). Not kept yet: crack state and the water's clay (lattice data).

### Round 4: proposed (18 Nov, card posted)

- **Kept ruts age while away.** A tile read back from disk is stepped forward by the time since it was written (its
  file time against the clock, scaled by `runoff_time_scale`): the same slump (Sumner's step at the mud's
  Bingham repose), rain from the weather map's history filling ruts and settling clay, and the tracked film drying,
  run in a few large stable steps on the CPU before it is seeded. Bounded per tile so a long absence settles to
  the ground's rest state rather than integrating hours.
  **Built (ageing ruts).** Kept tiles carry the unix time they were written (file version 2). Read back, a tile is
  stepped over the time away times `ruts_time_scale` with _age()'s own decays, which are exponential and so exact in
  one step: tint and mud film dry by `drying_time`, relief softens at `rain_smoothing` times `ruts_away_rain` (0.1 mm/h
  default, a temperate 900 mm a year: a 4 cm rut is about 1 cm after a month), water goes as evaporation beats that
  rain. A tile cached more than ten world minutes comes back from disk, aged. Bench `--test=ruts` (35 days).
- **Thrown clods leave lumps.** A landed clod over a size threshold deposits a mud depth (relief, with rim) instead
  of only the tint; mud on mud merges into the layer, on firm ground it dries and cracks as a small cake.
  **Built (clod cakes).** The mud spray child lands its clods with `track_mud` instead of a plain deposit: the same
  volume as relief (rim), and the clay film's tint at full share, wet, drying pale in place; on mud it merges with the
  layer as before. Check in `--test=clog` (clay film points within 5 m grow as the clods land).
- **Tread and sole patterns.** The film is laid through the tyre's or sole's pattern mask (the lug bars and boot
  sole already used for prints), so it reads as a tread mark rather than a smudge; share stays the coverage.
  **Built (prints).** `track_mud(..., print)`: the amount stamp's spare vec4 carries a print kind
  (ground_deform.glsl `mud_print_mask()`, mirrored on the CPU): a tyre's chevron lugs, 45 mm bars 9 cm apart across
  the wheel's heading and fixed on the ground so a rolling tyre's frames agree (15% between them); a boot's sole,
  heel and forefoot through `sole_sd()`/`sole_relief()` (no shank, no grooves), one disc per footfall with the relief
  spread over it. The share mask sets the film; the near field's 1.56 cm texels resolve the tyre lugs, the sole's
  grooves only soften.

### Round 5: proposed (18 Nov, card posted)

- **Grass flattened and torn in ruts.** The soil now shows through a meadow where ruts open it, but GrassField3D's
  blades still stand there. Build: GroundSurface3D publishes its exposure (ground_deform_exposed()) and the blade
  vertex shader lays blades flat by it (crushed under 2 cm, gone where the ground was turned or flooded); the
  trample field (add_trample) takes vehicle and boot passes for the bent-over band beside a rut, recovering over
  days. Grass is the vegetation system's: built there, fed from here.
  *Built (18 Nov):* GrassField3D's blade and tiller shaders read the ground's deformation map themselves
  (`grass_ground_opened()`, the same globals material.cpp samples, near field blended in), so nothing extra is
  published: a print or film a few millimetres deep, or any fresh tint, lays each blade flat its own way; past a
  centimetre or so of dent or rim, or under standing water, the blades are torn out (a dither by the blade's random,
  so the edge thins rather than cuts); the ones left stand on the dented ground (base lowered by the dent, raised on
  a rim). It lasts as long as the print does: the ground's own ageing and kept ruts carry it, so no trample field
  is needed. The VegetationField3D plants (flowers, clover, herbs) still stand in ruts: their materials are
  StandardMaterial3D; a next step. Bench: docs/bench/grass `--ruts` (two 8 cm tyre ruts and a light 6 mm pass,
  close view).
- **Everything kept with the ruts.** Crack state and the water's clay live on the ground lattice, not the field
  tiles, so a kept tile comes back with its puddle clear and its cracks gone. Build: a second small file per tile
  for the lattice cells under it (crack state, cycles, pool depth, clay kg/m2), aged the same way.
  *Built (18 Nov):* the clay is the field's (per texel, GPU), so it went into the tile itself: the GPU tile cache
  holds a third uint per ground texel (clay kg/m2; `GroundDeformGPU::cache_uints`), so a tile that leaves and comes
  back in the same session keeps its muddy water too (it used to come back clear), and the kept file is version 3
  (versions 1 and 2 still read, with clear water). Away, the clay settles as the water step has it, exp(-0.1 mm/s
  t / depth), and goes with the water. The crack state is the lattice's: `GroundCellStore` keeps it per block of
  32 x 32 cells (one `.cel` zstd file each, with each cell's write time), written as the lattice moves off cells or
  the node leaves the tree, read back as it comes onto them. Away, a soaked cell stays soaked while its pool lasts
  (the kept water's own budget: evaporation less ruts_away_rain), then its layer dries at the evaporation rate.
  Checks in --test=ruts: clay back in its pool on return and next session, dried cracks back with the same plates.


### Round 6 (picked 18 Nov: all three, in order)

- **Plants flattened in ruts.** VegetationField3D's flowers, clover and herbs keep standing in a rut the grass has
  left. Their materials are StandardMaterial3D on MultiMeshes, so the blade shader's trick does not reach them. Build:
  on the CPU, when a kept or live tile's dent passes a centimetre under a placement (the field's read back), drop
  it from its MultiMesh (torn) or lay it over (a per-instance lean, as `paths` trampling already shortens plants).
  *Built (18 Nov):* simpler than that: the plants' own shader (PlantGenerator's, which already reads the trample
  map) calls the grass's `grass_ground_opened()` (GrassField3D::get_ground_opened_shader_code(), shared): a light
  print lays the plant over its own way through the trample lean, a rut or standing water collapses it (dithered
  per plant), and the rest stand on the dented ground. No CPU work, and PT matches raster.
- **Ruts grow back over.** Grass torn out of a rut regrows from the edges over weeks (meadow recovery after wheel
  traffic: weeks to a season, faster in wet warm months), and a lightly pressed sward stands up again in days. Build:
  the ground's fresh tint already dries; a second slow channel (regrowth 0..1 per texel, aged with the kept ruts)
  that the blade shader reads to bring blades back, densest at the rut's shoulders first.
  *Built (18 Nov):* the regrowth share is per texel on the GPU field (the water cell's spare `s.z`), moved with the
  field, kept in the tile cache and the kept files (the clay uint's high half), read back (`get_regrown_at`). It
  grows only while a kept tile is away (`_ruts_age`: the bare share closes at `ruts_regrowth_time`, a month by
  default) and is zeroed wherever a stamp presses or digs again. The map carries it as A just over 1 (1 + 0.02 x
  share: a roughness factor too small to see, and A > 1 never happens otherwise), and the grass, the plants and the
  ground's exposure (material.cpp) read it: torn blades return, flattened ones stand up four times sooner, the
  ground shows its cover again; the hollow stays. Bench: docs/bench/grass `--ruts=aged[:scale]`.
- **Puddles placed by the world.** add_puddle() exists for scenes; the ground can call it itself where the terrain
  data map's wetness index is high in a hollow, after rain, so a generated world has puddles where water would
  gather without scene code.
  *Built (18 Nov):* `world_puddles` (on) and `world_puddle_density` (1 per 100 m2 where fully suited). Sites on a
  4 m world grid with their own hash; a site seen inside the field for a second is weighed by
  `_world_puddle_suit()` (the data map's wetness index or the first metres by water, as the deep mud's, or without a
  data map a hollow in the ground lattice; flat, bare soil only) and dug with add_puddle's hollow (radius 0.6 to
  1.6 m, 5 to 12 cm), filled with slightly muddy water (15 kg/m3) when the ground round it is wet, left a dry hollow
  (it can crack) when not; rain fills it later. Sites the field leaves are forgotten; a hollow already there on
  return (kept ruts) is not dug again. Check: --test=puddle-helper (three dips of a "hollows" ground).


### Round 7 (picked 18 Nov: all three, in order)

- **Puddles freeze in frost.** Below 0 C (WeatherMap3D air temperature, night radiative cooling) standing water
  grows an ice skin, thickness by Stefan's law (h ~ sqrt(degree-days)); thin ice is white-veined and cracks radially
  under a wheel or boot (a star of cracks, then broken plates floating), thicker ice bears. Build: an ice thickness
  per texel beside the water depth, drawn as a frosted, cracked sheet over the water in both renderers.
  *Built (18 Nov):* `puddle_ice` (on). Per lattice cell (25 cm) the ice's thickness and its broken share, stepped
  with the water twice a second from WeatherMap3D's air temperature: Stefan's law with Ashton's (1989) factor 0.6,
  dh/dt = 0.6 k dT / (rho L (h + 5 mm)) (3 cm in about 7 h at -5 C), degree-day melt above 0 C (8.1e-8 m/s/C,
  about 7 mm a day per degree), never thicker than the water, gone where the water goes. Loads break it by Gold's
  (1971) bearing law P = A h^2: `press`/`press_box` (a foot) below 3 cm, `press_wheel` below 6 cm, any dig; the
  broken share freezes back as 3 mm of new ice forms. Published as a fourth wet-map block (texel 3's tag 5.5; every
  reader now counts blocks by their tags, material.cpp `ground_wet_blocks()`); `ground_ice_look()` (material.cpp
  and the raygen, the same lines) draws it inside the standing water: ice's F0 (specular 0.22) and smoothness over
  the water's dark bed (black ice), milky patches of trapped air as it thickens, faint stress cracks on some cell
  edges, and where broken, Voronoi plates sized with the ice (6 to 25 cm), tilted, with open water and white
  fractured rims between them; no flow ripples under it. `get_ice_at()`. Not kept with the ruts (it re-forms from
  the temperature). Checks `--test=ice`; `--view=ice --pair` shoots a frozen puddle with a wheel track through it.
- **Regrowth follows the seasons.** The regrowth e-fold scaled by WeatherMap3D's season: grass grows from about 5 C,
  fastest in spring, stalled by summer drought and in winter (growing degree days), so a rut left in November is
  still bare in February.
  *Built (18 Nov):* kept ruts age their regrowth by growing time, not clock time (`_growing_seconds()`): day by day
  over the time away (ending on WeatherMap3D's day of the year; whole years at the year's sum), each day counts
  (T - 5 C) / 10 K of WeatherSeason's climatological day at the field's height (growing degree days, Cannell and
  Smith's base; the reference a late spring day), times a drought factor from the day's precipitation over its
  Thornthwaite evapotranspiration (none under 0.1, full over 0.7). The live weather's anomaly is left out: it is
  the last month's, the time away can be many. Default climate: 90 days to mid February regrow nothing, 35 days to
  late June about the reference; a Mediterranean August (bench dry_climate.gd) nothing. No WeatherMap3D: the old
  clock-time rate. Checks in `--test=ruts`.
- **Mud on plants beside ruts.** Spray and splashes that land on the sward tint the blades and plants there with the
  ground's clay (a per-texel splatter share the blade shaders read, as the body mud does), washed off by rain.
  *Built (18 Nov):* `plant_mud` (on). A CPU lattice share per 25 cm cell, raised where thrown clay lands
  (`track_mud` without a print: MudSpray's clods; full at a 0.3 mm film) and where muddy water splashes out
  (`add_water` with mud: full at 50 g of clay per m2), not by tyre or sole prints. Rain washes it off at an
  e-fold of 2 mm (simulated-rain wash-off of leaf deposits goes mostly early, Xu et al. 2017). Published as the wet
  map's fifth block (share, the clay's linear colour from mud_color/mud_color_map; texel 4's tag 6.5).
  GrassField3D's `grass_ground_mud()` (in the shared ground code, so PlantGenerator's plants read it too) and
  `grass_mud_apply()`: specks hashed on a 6 mm world grid that join into a coat as the share rises, thickest low
  down (reach 12 to 45 cm with the share), clay colour x0.8, matte (0.85); on blades, crops and plants, in both
  renderers. A thrown film no longer lays the sward flat (it came from the air): the tint term of
  `grass_ground_opened()` is scaled by 1 - the share. `get_plant_mud_at()`. Checks in `--test=ice`; grass bench
  `--ruts=mud`.


### Round 8: proposed (18 Nov, card posted)

- **Frozen ground holds its ruts.** WeatherSeason's soil frost (freezing degree days, none under snow) stiffens the
  soil: wheels and boots barely sink, old ruts freeze rigid and jolt a vehicle crossing them; the thaw then leaves
  the top wetter and weaker than before (thaw weakening, the spring mud season of forest roads), so ruts form
  deeper for a while. Build: a frost depth per lattice cell from the weather, scaling the cell's rate and bearing.
- **Raindrop rings on puddles.** Rain falling on standing water draws expanding rings (Lagarde's 2013 rain ripple
  texture approach, rings per drop cell timed by the rain rate), in both renderers, on ruts, puddles and ice-free
  water.
- **Hoar frost.** Clear, still nights below 0 C rime the grass, plants and bare ground white (deposition when the
  surface cools under the dew point), thickest in hollows; the low sun melts it off from the lit side first.


### Look (any stage)

Wet mud is dark and glossy with a thin water film (the Lagarde film we have); stiff mud matte, with
drier, lighter edges on berms; clay albedo per region. Under path tracing the ruts, berms, cracks and
clods are geometry, so nothing is lost; check raster against path traced at 100% crop for every stage.

## Cost and checks

Stage 0 and 1 are GPU passes on the existing field (a second field for the near ring, one more flow
pass). Stage 3 adds a mask per muddy body; stage 4 a particle buffer. Every stage: GPU cost on the PC
queue, a path-traced frame before merge, and no change to sand and snow numbers.

## Gap check for sand and snow

Things found here that would raise sand or snow too:

- **The finer near field** (stage 0): tread prints in sand and snow, sharp rill walls, ripple crests.
- **A field box that follows vehicles** (MudRunner's active blocks, TerraMech's tiles): ruts behind a
  fast vehicle outside the camera's 32 m.
- **Persistent ruts across the open world**: MudRunner and TerraMech keep ruts per tile; our tile cache
  is session memory only. Saving touched tiles to disk keeps a track system or a dune path between visits.
- **Caking and clogging** (stage 3) for snow (packed snow on tyres and boots, snowballing on wheels) and
  sand (a dust coat, wet sand on tyres).
- **Splats from spray** (stage 4): snow thrown by a wheel and kicked sand landing as marks on the ground
  and bodies; today the MPM grains return to the field but leave no mark on bodies.
- **Yield-stress flow for slush**: wet snow and saturated sand flow like a weak Bingham fluid; the same
  pass would replace their creep rates.
- **Crack patterns**: dried sand crusts after rain and refrozen snow crusts break into plates with the
  same Voronoi-by-thickness rule.
