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
  Reworked after the docs review (18 Nov): the look follows how a puddle freezes, from its rim in: where the ice
  reaches the bed (h over the water's depth, ragged by noise) it is milky with crowded bubbles (1 to 3 mm dots on a
  1.5 cm Worley grid, averaged past 2 mm footprints), the middle clear and dark with a few; frozen ripples (slope
  0.04) and roughness 0.12 instead of a mirror; no crack lines on intact ice; broken plates only where the broken
  share passes 0.4, tilted (0.35), with wider, softer white fractured edges. add_puddle's bank is now a continuous
  berm (48 overlapping discs, 2 cm over the water only where the rim is lower), not 16 mounds.
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
  *Built (18 Nov):* `frozen_ground` (on). Per lattice cell (thawed top a, frost depth b, thaw weakening w), stepped
  with the water from WeatherMap3D's air: Stefan's front with the soil water's latent heat (the modified Berggren
  equation's leading term, Aldrich 1956): dz/dt = k dT / (334 MJ/m3 theta (z + 2 cm)), frozen k 2, theta 0.1 to 0.4
  by wetness (12 cm in 12 h at -8 C); the thaw works down from the top (unfrozen k 1.2); a frost after a thaw
  refreezes the wet top first; snow-covered cells are left alone. New cells seed from WeatherSeason's soil_frost
  for the day. Crust share = smoothstep(2, 8 cm) of the frozen layer, gone under 0.5 to 3 cm of thaw: the cell's
  Bekker k's x(1 + 30 f), cohesion +200 kPa f, no slumping, max_dent x(1 - f), not deformable past 0.95 (ruts in
  it stay as they are, get_soil() reports it firm); ksat x(1 - 0.99 f), so water stands on it. Thaw weakening:
  while thawed soil lies on the frost (and for an e-fold of 5 days after it has gone) the cell's wetness is raised
  to past mud_wet_full and, with mud_layer, the thawed layer is mud as deep as it is. `get_frost_at()`.
  Checks `--test=frost`: 12 cm in a night, a boot leaves 0 mm (98 unfrozen), the old rut does not move, a 3 kN
  wheel sinks 87 mm in the thaw against 65 before the frost.
- **Raindrop rings on puddles.** (Moved to the rain thread, 18 Nov: rain, snow and wind are one system. It adds its
  ring slope to the standing-water block's `gfs`, scaled by 1 - the ice cover.) Rain falling on standing water draws expanding rings (Lagarde's 2013 rain ripple
  texture approach, rings per drop cell timed by the rain rate), in both renderers, on ruts, puddles and ice-free
  water.
- **Hoar frost.** (Moved to the world-links thread, 18 Nov, with dew, across every material.) Clear, still nights below 0 C rime the grass, plants and bare ground white (deposition when the
  surface cools under the dew point), thickest in hollows; the low sun melts it off from the lit side first.


### Round 9: proposed (18 Nov, card posted)

- **Ruts steer the wheels.** A wheel in a rut deeper than about a third of its sidewall is held by the rut's walls:
  turning out of it needs the lateral force to climb the wall (the wall's slope and the soil's shear), so the
  vehicle follows old ruts and lurches when it climbs out, MudRunner's rut tracking. Build: the wall slope under
  each tyre's edges from the field, a lateral force on PhysXVehicle3D's wheels.
  *Built, ground side (18 Nov):* `get_rut_guidance(contact, lateral, half_width)`: the deformation's cross slope
  under the tread (least squares over 7 points, dent less rim only, so the terrain's own camber the road plane
  already carries is not counted twice) and the wall height beside each edge (the highest relief 3 to 15 cm
  outside it over the tread's floor). A tyre resting across a rut's wall is pushed down the tilt by load x slope
  (rut wandering; the normal force resolved on a slope, as on a road's camber); one moving sideways shoves the
  wall as a bulldozed face that high. Checks in `--test=frost`: centred no push, walls 7.6 cm over a 6 cm rut,
  12 cm off-centre a slope of 0.36 back in. *Vehicle side (18 Nov):* PhysXVehicle3D's soil step reads the guidance
  under each soft wheel along its lateral direction; in the substep the wheel takes -load x slope sideways (capped at
  0.6) and its side bulldozing acts on the wall's height when it slides into one, not only its own sinkage (the soft
  force block; also fixed: side bulldozing had sat inside the rolling-resistance branch, so a wheel not rolling never
  felt it). Check in `--test=frost`: a car driven straight with its wheels 15 cm inside 12 cm ruts drifts 15 cm into
  them over 4 s, none on unmarked soil; it also drives 7.9 m in the old ruts against 0.5 m bogging on fresh soil.
- **Mud holds boots.** Feet in sticky, saturated mud are held by suction (the pore pressure under a lifting sole,
  as in breakout force of objects from seabed mud): a GroundLegMud3D or character foot slows to lift, with a sound
  hook, and loses a boot past a limit.
  *Built (18 Nov):* research: breakout of objects embedded in soft seabed clay (Vesic 1971, NCEL; Das 1980 for
  anchor plates). A quick lift is undrained: the pore water cannot follow the sole, so the soil above it fails in
  shear, F = A c_u N_c, with N_c rising with embedment D/B to about 9 past D/B = 4; lifted slowly, water seeps in
  under the sole and the long-term breakout is about a quarter. GroundLegMud3D `boot_suction` (on): the stance
  records how deep the sole went into sticky mud (slurry, or clay with stickiness 0.2-0.5 and up), and as the foot
  lifts the mud holds it with that force (cohesion capped at 30 kPa so frozen crust adds nothing; it needs depth
  anyway) until the sole has come up a quarter of its depth, then `boot_released(leg, force)` (the squelch).
  `get_boot_hold`, `get_boot_drag` = hold / (hold + `leg_pull` 400 N) for a controller or animation to slow the
  lift, `boot_hold_limit` 900 N emits `boot_lost` and sets `is_boot_lost` until the next footfall. Checks in
  `--test=legs`: dry soil no releases, 15 cm of slurry 9 releases up to 215 N, the static formula 1.1 kN at 40 cm in
  stiff clay mud quick, a quarter slow. The node reads animation; it does not move the foot itself.
- **Needle ice on bare soil.** Frosty nights on wet bare soil grow needle ice (pipkrake) that lifts the top
  centimetres; the thaw collapses it into loose, fluffy soil that prints easily and washes away. Ground-only,
  beside the frozen ground just built.
  *Built (18 Nov):* research: needle ice (pipkrake) grows where the soil surface falls below 0 C while the soil under
  it is unfrozen and wet: water drawn up to the freezing front freezes in columns and its latent heat holds the front
  at the surface (Outcalt 1971; Lawler 1988, "Environmental limits of needle ice"); typically 1 to 4 cm a night on
  bare silty soil near field capacity, none under litter or vegetation, and colder nights freeze the ground solid
  instead. The thaw drops the cap as loose crumbs of low bulk density, a known erosion agent on river banks and
  trails (Lawler 1986). Ours, in the frozen ground's lattice (`needle_ice`, on): the needles grow by Stefan through
  the mat (half ice, k 1 W/(m K)) while the frost front is under 1 cm and the cell is bare (litter under 5 mm), wet
  (moisture 0.25 to 0.4 and up) and fine (sand under half), between about -0.5 and -8 C; the front stays put while
  they grow. Their height is laid on the deformation field as bilinear tents a lattice step wide (a new
  `GroundDeformField.amount_tent`, so equal needles rise flat; the first try with discs left a dimpled grid), in
  steps of 2 mm, taking turns round the lattice, and taken off as they melt. Weak: cells with needles or crumbs
  take Bekker k and cohesion x0.4, so a boot sinks about three times as deep; runoff picks the crumbs up as it
  does loose sand. Crumbs settle over a week. Look (wet map ice block B and A, both shaders, `ground_needle_look`):
  the cap's Voronoi clods 4 cm across with thin dark cracks, a few tipped clods showing a white band of needles on
  their raised side (the first look, white gaps at every joint, read as a net), crumbs as rough tilted grains after
  the thaw. A foot on the needles does not clear the lattice (a cell is 25 cm, a boot 10): the print is the press's
  own, deeper in the weak mat. No repeat (Paulo's rule for anything that repeats): the growth varies by two
  octaves of value noise (1.7 and 0.6 m, x0.5 to 1.5, the soil's patchy water and silt), so the lift is real
  uneven geometry; the clod lattice is domain-warped by a noise about a metre across and the crack width and the
  share of tipped clods vary in patches of 1 to 2 m. Checks in `--test=frost` (`_run_needle_tests`): 1.7 cm in
  10 h at -3 C at the test point, the front held, the ground 1.7 cm higher, a boot 50 mm deep against 16.5 before,
  gone after about 2.6 h at +5 C leaving crumbs 0.84 and the ground within 0.3 mm of where it was, the crumbs
  50 mm. `--view=needle [--thaw] --pair` shoots it.


#### Round 10 (Paulo picked all three, in order, 18 Nov)

- **Centre ridge.** Research: mobility models count "hang-up" (high-centring) failures where the soil between the
  ruts, plus the berms the wheels shoved up, reaches the belly once the rut is deeper than the ground clearance (Rula
  and Nuttall 1971, NRMM; Wong 2008 on ground clearance); MudRunner's signature stuck. *Built (18 Nov):* the belly
  (stage 3a) bore only below the undisturbed surface, so berms and ridges above it held nothing; it now bears on
  the undisturbed surface plus the rim above it (rim less dent, where positive), and ploughs that height with its
  leading row. The wheels unload as the belly takes the weight, as before. Check in `--test=frost`: along ruts 35 cm
  deep with a ridge 25 cm proud between them the belly carries a third of the car's weight on average and it gets
  3.0 m in 4 s, against 15.7 m along 12 cm ruts with no belly contact.
- **Tyre chains and tread types.** Research: lugs and grousers shear soil below the tread face, raising thrust in
  soft soil (Wong 2008, sec. 2.6); forestry chains add about a fifth to a wheel's pull on soft ground (Forest
  Research FCTN013; Sever et al. 2018, CROJFE 39); open voids self-clean, closed road treads pack (the clogging of
  stage 3); on ice chains about treble a tyre's grip and on snow about double, but ride on steel on dry tarmac (CRREL
  tyre and chain tests). *Built (18 Nov):* `PhysXVehicleWheel3D.tread` (road, all terrain default, mud, chains),
  read live. On soft soil it scales the shear limit (0.85, 1, 1.12, 1.2), the clog fill rate (1.4, 1, 0.6, 0.5),
  the shedding speed (1.3, 1, 0.7, 0.75) and the void depth (9, 20, 25, 20 mm); chains keep half the grip a clogged
  tread loses. Firm ground (world-links' branch) gets `PhysXVehicleWheel3D.tread_firm_grip(tread, ice, snow)`. Check
  in `--test=clog`: 4 s at half throttle in stiff sticky clay, road tyres 0.29 m and 39% packed, mud tyres 0.78 m
  and 16%, chains 0.80 m and 13%.
  The ruts they cut: `press_wheel()` takes the tread and prints it in the rut floor (`tread_relief()`, GPU and
  CPU): road tyres' grooves stand as faint ribs, all terrain chevron lugs every 9 cm, mud tyres' staggered open lugs
  every 13 cm with deep voids, chains a ladder of cross links every 22 cm pressed 1.5 cm in over fainter lugs. The
  pattern is fixed on the ground along the heading (the frames of a rolling tyre agree), warped by noise at 0.9 m
  and 25 cm so a long rut does not repeat, faded at the shoulders and smeared out past 0.6 to 3 m/s of slip. Checks
  in `--test=treads`: rolled presses give floor relief of 0.2 mm rms plain, 4 all terrain, 7.8 mud, chains cut 16 mm
  below the mean; a driven car on mud tyres 4.4 mm rms against 0.5 on road tyres.
- **Mud slides downhill.** Research: a mud layer is a Bingham fluid; on a slope it stands while its weight down its
  free surface stays under the yield stress, rho g h tan(surface) <= tau_y, and a mudflow stops at
  h = tau_y / (rho g sin theta) (Johnson 1970; Coussot 1997). Rain lowers the solid share and the yield stress falls
  a hundredfold from stiff mud to slurry (O'Brien and Julien 1988, the fits we use), so a bank that held all summer
  runs in a storm; shallow-landslide work (infinite slope, Iverson 2000) gives the same trigger for soil. *Built
  (18 Nov):* `mud_slides` (on). Each lattice cell carries the mud's slide length tau_y / (rho g) (a fourth vec4 per
  cell on the GPU); the field's slumping lets the undisturbed ground's own step stand only where the layer holds it
  (slide length over the depth left, deepest dent less what is gone plus rim); steeper, the layer's surface relaxes
  to that slope, so the whole layer runs as one mass, thins from the top and piles at the foot, volume kept, until its
  surface is as gentle as it holds. Settled texels take part while a cell slides; the GPU field slumps for a minute
  after the cells change (rain renews it) at four passes a step. `get_mud_slide_angle_at()`. GPU field only.
  Check in `--test=slides`: 40 cm of mud on a 15 degree bank, stiff it holds; soaked for a minute the upper bank
  loses half its mud and its surface eases to 9.6 degrees (it holds 9.4 there), the 20 cm rut at the foot is filled
  and heaped 24 cm, volume kept.

#### Round 11 candidates (card 18 Nov)

- **Gullies in ruts.** Wheel ruts on slopes concentrate runoff and are the classic start of gully erosion on
  forest and farm tracks (Croke and Mockler 2001; Foltz 1996, rutted logging roads lose several times the
  sediment of unrutted ones). The runoff channels (RillGrow on the field, Exner bed) already cut where water runs;
  the gap is routing rain caught by a rut along it instead of standing, and a deposition fan where it slows.
- **Winch.** MudRunner's core recovery tool: a cable to an anchor (tree, rock, another vehicle) pulling at the
  frame; PhysX distance joint with a motor-driven rest length, sag from the cable's weight as a catenary drawn
  between, and the pull fighting the soil's resistance we already compute.
- **Body mud dries and flakes.** Caked mud dries lighter and cracks (the desiccation crack model we use on the
  ground, at body scale), then sheds in flakes on bumps; the clods of the ground-detail thread can carry them.

#### Round 11 (picked 18 Nov: all three, in order)

**11.1 Gullies in ruts: built.** Runoff over cohesive ground now detaches soil instead of rolling it as bed load:
WEPP's rill detachment (Nearing, Foster, Lane and Finkner 1989), Kr (tau - tau_c), Partheniades' (1965) excess
shear law, Kr 0.006 s/m (cropland baseline, Flanagan and Nearing 1995) and three times that where tyres freshly
turned it. The soil rides the water as a suspended load (water[].s.w, metres of bed), moved with it upwind like the
clay, and settles at 1 mm/s where the shear falls under the ground's own (Krone 1962). The cut deepens downstream as
the flow gathers and the soil lands as a fan where the slope eases or the water ponds; the old bed-load pull sent the
walls' soil onto the floor, so ruts filled instead of cutting. Each cell carries a critical shear (cover part 3 .y):
sand 0.19 Pa (Shields, stays bed load), soil 3.5 to 1.5 Pa by moisture, mud 0.3 to 2 Pa by stiffness, litter up to
five times (roots); halved on fresh ground, ten times under regrown sward (Knapen et al. 2007). Muddy runoff shows
in the water's tint. Bench --test=gullies: 20 minutes at 10 mm/h (runoff_time_scale 30) down a 6 degree rut cuts
14 mm along its floor, none beside it, and lays 21 mm on the flat below.

Bug found on the way: with runoff on, any texel whose own standing water passed 2 mm counted as a lake and its water
was dropped (the lattice's water depth included the GPU's puddles). Puddles in ruts under runoff never held; now
only the water bodies' depth ends the runoff. The bench checks 2 L poured in a rut is kept.

**11.2 Winch: built.** PhysXWinch3D, a child of a PhysXVehicle3D at its fairlead, cables to an anchor: any physics
body (a tree's or rock's static body), another vehicle, or a fixed point. The cable is a PhysX distance joint
(PxDistanceJoint), tension only: its max distance is the cable paid out, a spring at the rope's axial stiffness EA / L
past it (6 MN for 9.5 mm steel wire rope), damped near critically against the vehicle's mass. The drum (hold, in,
free spool) winds at the speed its load allows: an electric winch's line speed falls about linearly from no load to a
tenth at its rated pull and stalls just past it (WARN's published 10,000 lb curves: 44.5 kN, about 0.14 m/s no load);
the brake slips past 1.5 times rated. The cable is drawn as a catenary from its length and weight per metre (sag from
the length when slack, w L^2 / 8 T when taut) and lies on the ground where it would pass under it. Bench
--test=winch: a free-rolling car comes in at the line speed, a braked one is dragged on its tyres' friction, a slack
cable sags onto the ground and carries nothing, a taut one sags as theory says, a car is pulled through mud it cannot
leave alone, and a braked car winches a dead one in.

Bug found on the way: a car on soft soil with its brakes on slid on a seventh of its tyres' friction at walking pace.
Under 0.2 m/s PhysX's sticky-tyre constraint (a soft damper with no friction limit) replaces the tyre force, and above
it a braked wheel's slip was over the 3 m/s floor kept for driven wheels on soft soil. Braked with no throttle, the
sticky regime now starts only at rest (0.02 m/s) and the floors are 0.5 m/s; a locked wheel does not turn, so the
floor's ringing cannot start.

**11.3 Body mud dries, cracks and flakes off: built.** Mud on a body now dries with the weather: Penman's wind
function, 0.26 (1 + 0.54 u) times the air's vapour deficit, plus the sun, against a reference day (20 C, half
humidity, 2 m/s, half sun) that dries a coat in five minutes; rain wets it back (a coat soaks in about two minutes of
heavy rain) and, past a few mm/h, rinses it thin. Once a thick coat is dry it cracks into plates: Worley cells
of 2.2 cm, warped, open only where the cake is over the critical cracking thickness (Routh 2013: 2.8 to 4.5 mm by
place) and drier than 0.45; the cracks are half a millimetre wide, the plate edges curl up 0.7 mm and the paint shows
in them, in raster and path traced (same code in material.cpp and the raygen). Dry plates fall off on jolts: each
splat sheds at a rate that grows with the body's acceleration past 0.8 g times its dryness, and a shed plate drops to
the ground under it as a track_mud mark (vehicle get_mud_flakes_shed()). Wet mud never flakes. Bench --test=flakes:
a dry caked car over three bars sheds 5 of 12 plates, a wet one none. --view=bodymud --crack shows the cracked coat.

Bug found on the way: a wheel standing still in mud counted as freshly churned (its cake kept topping up), so a
parked car never dried. The tread must be turning (0.1 m/s at the rim) to pick up fresh mud.


#### Round 12 candidates (card 19 Nov)

- **Tyre pressure.** Airing down lengthens and widens the footprint: Bekker's b and the contact length grow, sinkage
  and rolling resistance fall (Wong, Theory of Ground Vehicles, ch. 2; the CTIS literature puts 15 to 25 % more
  drawbar pull at a third of road pressure). PhysXVehicleWheel3D.pressure scales the flexible-tyre terms we already
  feed to the soil model.
- **Firm subsoil.** A soft layer over a hard pan: the pressure-sinkage curve of the top layer until its depth, then the
  stiffer one (layered-soil Bekker, Wong 2010; the "hardpan" of MudRunner's deep ruts). A depth per cell in the field.
- **Branches under wheels.** Sticks and logs laid in a rut (deadwood from the trees thread) are pressed in and add
  shear strength under the tyre, the corduroy road: a cell factor on the Janosi-Hanamoto shear, and their meshes sink.

#### Round 12 (picked 19 Nov: all three, in order)

**12.1 Tyre pressure: built.** PhysXVehicleWheel3D.tire_pressure (read live, so a script or a tyre inflation system can
air down while driving) now acts on firm ground as well as soft. The tyre stands on its air plus its carcass's own
stiffness, p_g = p_i + p_c (Wong 2008, sec. 2.4; p_c 15 kPa, within the 10 to 30 kPa measured for radial tyres), over a
flat W / (b p_g) long. It deflects by its vertical stiffness, k = C p_g sqrt(b D) (Rhyne 2005's scaling; C set so a
225/65 R17 on 220 kPa is about 220 N/mm), and bottoms at a third of its section height. Below the 200 kPa default an
aired-down tyre stands lower by the extra deflection (get_wheel_deflection()), corners more softly (MF 6.1's pressure
term, 1 + 0.6 dp, kept between half and 1.15 times; Besselink, Schmeitz and Pacejka 2010) and on firm ground rolls with
more resistance (Gillespie 1992, f_r = 0.005 + (1 / p)(0.01 + 0.0095 (v / 100)^2), the difference from road pressure). Horne's
aquaplaning speed now uses the tyre's own pressure. A tyre left at 200 kPa drives exactly as before. Bench
--test=pressure: at 80 kPa the car stands 23 mm lower, loses 0.25 m/s more coasting for 3 s (Gillespie: 0.23), sinks
2.0 cm instead of 4.5 cm standing in stiff mud and gets 5.9 m instead of 1.5 m in 8 s.

**12.2 Firm subsoil: built.** GroundSurface3D.subsoil (on): the loose layer (max_dent) now lies on a firm base whose
depth varies 0.7 to 1.3 times over a few metres (two octaves of value noise, 2.5 m and 0.9 m;
get_subsoil_depth_at()). A load stiffens the layer once what is left under it is thinner than the load is wide:
finite-depth pressure-sinkage, k_app = k (1 + B (exp(c (z - z0) / b) - 1)) past z0 = H - b (Salman 2022, after
Yegorov; B 1, c 2). What a spinning wheel digs on top of that (Lyasko's slip sinkage) is thrown out, so it still digs
down to the base. Once the soil left under the tread is thinner than its lugs (3 cm) the tyre bites the base at
friction 0.5, whatever clay clogs it. Bench --test=subsoil: a light wheel in 21 cm of slurry sinks 5 cm instead of
12 cm, a heavy one still reaches the base and grips on it, and a car whose wheels spin in place (slip 0.98) with no
base crawls on at slip 0.38, 3.4 times as far.

**12.3 Logs and branches in ruts: built.** GroundSurface3D.mats (on): a RigidBody3D in the "ground_mats" group with a
cylinder or capsule shape is taken by the ground once it settles on soft soil. It is frozen and laid on its highest
supports along the deformed surface, so it lies in the ruts and on the berms. A wheel that touches one rests on the
wood, the height where circle meets circle, so it bridges the gaps of a corduroy the road ray falls through, with
the body's own friction. The log spreads the wheels' load (smoothed over a fifth of a second) along the length a beam
on an elastic foundation bends over, pi / lambda with lambda = (k_s d / 4 E I)^(1/4) (Hetényi 1946; E 9 GPa for green
wood, USDA Wood Handbook). It presses its print to the plate sinkage of that load, layered as in 12.2, and sinks with
it. A branch whose bending moment under the load, P / (4 lambda), passes green wood's 40 MPa snaps and stops carrying
wheels. Bench --test=mats: on 29 logs across 25 cm of slurry the car drives 4.6 m in 8 s, where bare it bogs at
0.5 m. The logs sink 6 cm at most against a bare tyre's 28 cm. A 16 cm log carries a 3.7 kN wheel; a 3 cm branch
snaps under it. --view=mats shows a car on a corduroy.
#### Post-fire debris flow body (queued 19 Nov, picked through the PhysX thread)

Valence's runoff fires a debris-flow event (WorldRunoffProvider::contract_runoff_debris_flow: event id, outlet, volume
from USGS M1 and Gartner 2014, the channel path downstream, peak I15, burnt share, channel width and burn severity
along the path). Mud owns the moving body:

- **Model.** A depth-averaged flow on a 1 to 2 m lattice in a corridor along the path (the GPU field covers only
  32 m): shallow-water mass and momentum with a Voellmy basal resistance, tau = mu rho g h cos(theta) + rho g u^2 / xi
  (RAMMS: Christen, Kowalski and Bartelt 2010; mu about 0.1, xi 200 to 1000 m/s^2 for post-fire flows), plus the
  Bingham yield stress (O'Brien, Julien and Fullerton 1993, FLO-2D's quadratic rheology) so it stops on its own.
- **Bulking.** Entrainment along the channel at McDougall and Hungr's (2005) natural-exponential growth rate,
  E = ln(V_out / V_in) / L, scaled by the path's burn severity and capped by the bed's loose depth (the cells' own
  layer, round 12's subsoil).
- **Stopping.** The yield stress holds once rho g h sin(theta) falls under tau_y, about 10% slope for typical flows
  (the fan apex). What stops is written as a mud deposit (depth, clay) into GroundSurface3D and the WorldGround
  height and mud fields, so the field's ruts, slides, cracks and drying carry on from there.
- **Near the camera.** Within the 32 m field the GPU slide pass (round 10) takes the front's depth, so it flows over
  ruts and logs at 6 cm.
- **For PhysX.** get_debris_front(event_id) gives the front's position, depth and speed for its surge particles
  and boulders.
- **Wiring (19 Nov).** Valence eab1d857: basin_area in km2; get_debris_flows_since(-1) returns the whole log. Steep burnt
  valleys fire many small events at once (68 in 4 km2 at 40 mm/h): pool events whose paths join into one body. A
  deposit reaching water goes to WorldWater.add_sediment. PhysX 427652c3bb: PhysXDebrisSurge3D.front_source takes the
  GroundDebrisFlow3D and follows get_debris_front(id) while it returns a position.

**Built (19 Nov): GroundDebrisFlow3D** (modules/ground). Listens to WorldGround's debris_flow (listen, on) and
pools events whose outlets lie within pool_distance (30 m) of a running body's path. Corridor lattice cell_size 1.5 m,
corridor_width 60 m, carried 40 m past the path's end for the fan; local inertial momentum with a semi-implicit
Voellmy turbulent term (voellmy_xi 400) and a Coulomb plus yield stop (voellmy_mu 0.1, yield_stress 1500 Pa);
Rickenmann hydrograph feeds; entrainment 0.002/m times severity, max_scour 0.6 m. On stopping: add_mud_deposit on the
active GroundSurface3D, TerrainBrush3D add rectangles on terrain_deformer if set, WorldWater.add_sediment_mass where the
water is deeper than 10 cm. PhysX reads get_debris_front(id) and debris_front_changed(id) (once per physics frame per
body, pooled ids too); debris_flow_started / debris_flow_finished. The sheet mesh clips at the 1 cm contour (marching
triangles), smooths drawn depth over neighbours and carries ash streak noise at 4 m and 1.3 m.
Bench --test=debris: 2,000 m3 down a 25% channel: front peaks 7.1 m/s, stops 39 m onto the 4% fan, entrains 310 m3,
budget closes to 0.1%, fan laid up to 3 m deep. --view=debris [--t=40] shows a flow spreading at the fan apex.
Limits: margins are 1.5 m resolution from afar (near the camera the slide pass and PhysX surge carry the detail).

#### Round 13 (picked 19 Nov: all three, in order)

**13.1 Debris fans up close.** Research: grain-size segregation pushes coarse grains to the front and margins, which
stop first and build lateral levees (Johnson et al. 2012, JGR; Iverson 1997 coarse snout; de Haas et al. 2015 fan
experiments); a fresh granular deposit holds roughly its friction angle (Major and Iverson 1999); drag on bodies in
the flow is rho Cd A u^2 / 2. Boulders at the snout and on the levees are PhysX's (it owns the boulders).
*Built (19 Nov):*
- `levee_height` (0.5): each body tracks the deepest flow per cell (hmax). Margin cells (run, beside a never-reached
  cell, at least 4 of 9 neighbours run) lay a ridge of levee_height x the 3x3 mean hmax, varied 0.7 to 1.3 by noise,
  at most 1 m; the rest of the deposit gives up the volume (at most half of it), so the budget still closes.
- `push_bodies` (on): RigidBody3Ds the flow reaches get drag rho 1.2 width h u_rel^2 / 2, applied as one impulse per
  advance over the scaled time and clamped to mass x relative speed. `get_flow_velocity_at(position)`.
- The fan becomes the ground: the deposit is sampled into 1 m mud_deposits cells (3x3 bilinear mean per flow cell);
  GroundSurface3D raises its ground lattice by a cubic B-spline of the deposits (`refresh_deposits()`), so the
  surface is smooth with no cell steps. In the print field the relief under it is buried: ruts fill, rims level.
  Fan cells read as saturated (wetness from depth, moisture lifted past mud_wet_full), make no slides and hold a
  repose of at least tan 0.6. The flow sheet draws nothing over field cells it has handed to the ground.
- Logs (ground_mats) under a deposit at least their diameter deep are buried: hidden, no collision, no grip
  (`is_mat_buried`).
Bench --test=fans (valley height): levees 0.33 m against 0.10 m mid-channel; a rut under 2.8 m of fan filled; a log
buried; a 150 kg crate shoved 1.7 m (peak 8.2 m/s); 2.6 m of soft mud where there was none.
Also (19 Nov): `add_mud_deposit(..., color)` gives a deposit its own colour; GroundSurface3D publishes coloured
deposits as a wet map block (texel 5's tag 7.5: cover, linear colour, B-spline weighted like the depths, edge
wobbled by noise), and the ground's raster and PT shading take it, so the fan in the print field shows the flow's ash
grey and meets the sheet beyond it with no seam. Slurry water ponding along a levee's foot reads as muddy water.
Edge fixes (19 Nov): a coloured deposit (one its depositor draws) raises the ground only inside the print field,
fading in over the field's 1 to 4 m from its edge; the sheet runs on 3 cm under the ground there (hidden), so seen
from any side there is no step where the two meet. Margin cells of the sheet split 4 x 4 with bilinear depth, so its
1 cm contour curves instead of a 1.5 m saw-tooth. The deposit's colour covers past a few millimetres.

**13.2 Stream fords.** Research: off-road vehicle crossings show as turbidity spikes that pass downstream with each
crossing (Alaska DEC, Kenai ATV crossings 2002; Forest Service low-water crossing guides); remoulded soil erodes past
a far lower critical shear than intact soil (Knapen et al. 2007, about a sixth); saturated banks fail as shallow
slabs (infinite slope, Skempton and DeLory 1957), the water table standing near the stream's stage (Simon et al.
1999), and a fast surface load on saturated soil is carried by the pore water first (undrained), adding weight but
no friction; trampled banks retreat that way (Trimble 1994).
*Built (19 Nov):* `stream_fords` (on), in `press_wheel`:
- In water from the contract (3 cm or more): a share of the rut, smoothstep(0.5, 3 Pa) of the current's shear times
  the soil's fines, is scoured (a dig under the wheel); the press throws 70% less rim (material_cell's rim_share
  under a current); both go to `WorldWater.add_sediment_mass` as mineral, the plume the stream carries on.
- On a bank (12 degrees or more, water within 3 m downhill): FS = (c' + gamma' z cos^2 b tan phi' + (1 - m) q cos^2 b
  tan phi') / ((gamma z + q) sin b cos b), slab z 0.5 m, m its share below the stream's level plus a 0.3 m capillary
  fringe (or the soil's own wetness), q the tyre's Bekker pressure spread 2:1 to the slab's base (at most 250 kPa).
  Under 1: the slab is dug out above the wheel, half of it laid as fresh mud at the toe, half its fines into the
  water; one failure per 1 m cell per 4 s. `get_ford_stats()`.
Bench --test=fords (height "ford": 30 degree bank into a 35 cm stream at 0.8 m/s): rut 6.7 cm in the stream against
5.7 on land, 23 kg of plume, rims 1.2 against 2.3 cm; the low saturated bank gives way under one wheel (12 cm lower,
16 cm at the toe, 477 kg into the water); the same wheel higher up, above the fringe, holds.
Valence's river carries and shades the load (WorldWater sediment); the ground draws no plume of its own.

**13.3 Off-camber sliding.** Research: a tyre's side force on soft soil is the soil's shear limit (A c + W tan phi)
times Janosi-Hanamoto's mobilisation on the side slip, so on wet clay (low c, low phi, a clogged tread) the slope's
pull W sin b beats it and the vehicle slides; sliding sideways a wheel sinks as it does spinning, the soil flowing
out from under it to the side (Ishigami et al. 2007, J. Field Robotics; Wong 2008), and the lower wheels carry more of
the load. Already in place: the side force capped by the soil (road friction from the shear limit), side bulldozing
against the sidewall (Ishigami 2007), the sideways scuff and berm in the print.
*Built (19 Nov):* GroundTerramechanics::wheel() takes the slip sinkage on the combined slip, sqrt(i^2 + tan^2 a);
PhysXVehicle3D's soil probe passes the tyre's lateral slip angle, eased in over 0.1 to 0.4 m/s of side speed (at a
crawl the angle is noise). Bench --test=camber (height "camber", 20 degree side slope): a wheel at 20 degrees of side
slip sinks 4.3 cm against 2.6 rolling; a 1500 kg 4x4 at 0.4 throttle across the slope in stiff mud slides 6.7 m
downhill in 5 s, lower wheels 11.2 cm deep against 8.4 upper; on drier soil it holds (0.18 m).

#### Round 14 (picked 19 Nov: all three, in order)

**14.1 Bow waves in liquid mud.** Research: slurry is a Bingham fluid with inertia; at a vehicle's speed its Reynolds
number rho U h / mu is of order 10 to 100, so what a tyre or a belly shoves aside runs off as a wave and the rut
closes with a slosh, not a creep. Shallow Bingham flow: depth-averaged shallow water with the plug's bed stress
tau_b = tau_y + 3 mu u / h (Huang and Garcia 1998, JFM; FLO-2D's quadratic rheology without its turbulent part,
O'Brien et al. 1993), stepped as the local inertial pipe model (Bates, Horritt and Fewtrell 2010, J. Hydrology), as
the standing water already is. It stands where g h S < tau_y / rho, the same critical slope slumping and slides use.
*Built (19 Nov):* `mud_waves` (on). Cells from slurry to soft mud (stiff share under a half) carry tau_y / rho and
mu / rho (cover cell 3 z, w); on those texels the GPU field runs MODE_MUD_FLUX / MODE_MUD_DEPTH instead of
slumping: per face q += dt g h d(eta) / l, minus the yield's dt tau_y / rho (never past zero), the viscous part
semi-implicit, outflow capped at the layer (max_dent - dent + rim); arriving mud fills the dent first, leaving mud
takes the rim first. Firm ground and stiff mud are walls. Substeps under CFL (0.5 l / sqrt(g 1.2 m)), a press keeps
the field moving 4 s. Bench --test=mudwave (80 cm of slurry, a 25 cm rut): 1.1 cm deep after 1 s against 5.1
slumping, the layer 75 cm out drops 3 mm as it runs in (slumping does not reach), volume kept, it settles at 1
degree; stiff mud unchanged.

**14.3 Hidden stones and roots.** Research: MudRunner and SnowRunner hide rocks and logs under mud (the surface reads
even; a wheel sinking rides up onto what it cannot see); stones weather out of soil and lie through the layer;
tree roots spread mostly in the top 20-30 cm within about the crown's reach (Stone and Kalisz 1991). Stiff bodies in
a soft layer bound the sinkage: the wheel's contact can go no lower than their tops.
*Built (19 Nov):* `buried_obstacles` (on), `buried_density` (0.05 per m2 of deep mud). Each 2 m cell with a mud
layer (as deep as when soaked) holds a few from its hash: stones radius 8 to 25 cm at any depth in the layer, roots
(capsules 3 to 8 cm thick, 0.8 to 2 m long, near level, 3 to 20 cm down) with probability 0.7 x canopy closure.
`get_buried_depth_at(position, radius)` is the shallowest top under a disc; press_wheel and PhysXVehicle3D cap the
sinkage there, so the wheel jolts up onto it and its rut floor stays on it. Within 16 m of the followed point each is
a StaticBody3D (group ground_buried; sphere or capsule shape) with a lumpy stone or knotted root mesh, wet-looking;
uncovered by ruts or water they collide as any rock. `get_buried_near`. Bench --test=buried (density 0.5): 25 within
4 m, same every query; depth to a stone is its top's; a 70 cm press stops on a stone 10.5 cm down while clear ground
takes 36 cm; bodies near the probe; none when off or without mud. Ground bench _setup turns it off for other checks.
Limits: the cap applies across a whole wheel print (the stone stops the print, it does not dig round it); round 15.2
lifts this.

#### Round 15 candidates (card 19 Nov)

- **Flood silt skin.** WorldWater.sediment_deposit_at (kg/m2, Valence 77919272) laid as a thin fresh layer: depth =
  mass / dry bulk density (about 1300 kg/m3 for fresh silt, Asselman and Middelkoop 1995), saturated at first (a slick
  film: low tread grip, Wong's adhesion), drying and cracking with the existing desiccation cracks. Merge with
  add_mud_deposit's overbank silt so there is one deposit store.
- **Stones dug round and kicked loose.** Round 14.3's limit: the stone caps the whole print. Instead stamp the print
  minus the stone's footprint (cap per texel on the GPU from a small list of nearby obstacle discs), and an exposed
  stone whose bearing in the mud fails under a tyre's shear (Terzaghi bearing on a buried sphere; Ishigami's side
  bulldozing force) turns into a PhysX rigid body and is pushed out.
- **Worn tracks kept between sessions.** Save wear_cells with keep_ruts (same file, same time scaling as ruts' ageing),
  so a used route is still packed and bare on return.

#### Round 15 built (20 Nov)

**15.1 Flood silt skin.** What a river records as settled on its bed (WorldWater.sediment_deposit_at, kg/m2) is
laid once, as the increase over what the ground already took per 1 m cell (silt_river_taken), at a dry bulk density
of 1300 kg/m3 (Asselman and Middelkoop 1995); still floodwater over ground with no river record still lays its own
(_flood_silt, Pizzuto 1987). Both go to one store (_lay_silt: mud_deposits for the height and colour, silt_depth for
the skin). The skin is grey-brown silt darkened by its ash share. Fresh, it is soaked (desiccation crack state 2):
the wet map reads it dark and glossy from 0.5 mm, its cell's moisture is raised by the water left in it and the
litter under it is buried. PhysXVehicle3D takes the firm grip down to 0.85 to 0.5 (wet) over 0.3 to 2 mm of skin
(fine earth on a hard surface roughly halves tyre friction; SAE 2007-01-0736, TRL PPR073). It then dries from its own
top whatever the ground under it, pales into a crust and cracks into plates about four times its thickness
(Groisman and Kaplan 1994), its cracks shown from 1 to 3 mm of skin. It is soft mud only while wet. `get_silt_at`
(depth, water). Bench --test=silt: 2 then 4 mm laid for records of 2 and 4 mm (never twice); flood water lays 3 um in
1.5 s; dried, water 0 and cracks open at 2 cm plates; off, nothing is laid. --view=silt [--dry]: an older lobe dried
and cracked, a fresh band dark over its far end.

Look fix (20 Nov, docs declined the first cracks as a decal): a thin layer drying on a substrate that does not
shrink cracks into polygons some ten times its thickness (Groisman and Kaplan 1994; Weinberger 1999), and its plates
curl up at their edges as the top dries first (mud curls: Allen 1986; Minter et al. 2007). The skin's crack state now
carries plates of 10 x its depth, varied 0.6 to 1.5 by a 2.3 m value noise, and its share as a negative regularity;
ground_cracks() (material.cpp and the path tracer alike) then opens wider gaps (12% shrinkage), cracks only through the
layer, and curls each plate's rim up to half its "thick" over 0.18 of the domain. The curl now survives footprints up
to three reaches wide, so the ground mesh carries the dished plates where it cannot carry the slots (pond cracks gain
a little curl at distance too).

**15.2 Stones dug round and kicked loose.** A wheel is held up only by a stone or root under its middle (a quarter of
the print's short side for press_wheel; half the tyre's half-width in PhysXVehicle3D). Under the rest of the print the
tyre wraps round it: the stamp carries the highest body under the print as a floor (GroundDeformField press_floor,
Stamp.f/g on the GPU, STAMP_BYTES 112), so each texel stops on that body's top and the mud is pressed out round it.
A stone whose rut floor has gone below its centre, met ahead of a wheel's middle at over 0.3 m/s, is knocked loose:
a RigidBody3D (group ground_loose, 2650 kg/m3 over its collider) leaving with 0.6 of the tyre's speed and a little
lift, its socket closing (a dig over 0.8 of its radius). Loose stones leave the hidden list for good (buried_loose)
and their bodies go past twice buried_body_range. `buried_kick`, `get_loose_stone_count`. Bench --test=buried adds:
a 0.8 x 0.5 m print with a stone under its edge goes 40 cm deep beside it and 26 cm over its top 25 cm down (stiff
mud: a slurry's own flow carries the mud off its crown, where its body shows); a rolling tyre past a stone's middle
knocks it loose and it moves off. Limits: one body per print (prints are small, stones sparse); slumping and waves
do not see the stones.

**15.3 Worn tracks kept between sessions.** wear_cells go to one zstd file (worn_tracks.wear) in ruts_path with the
ruts (save_ruts, on leaving the tree), read back the first time the folder is used, aged by the time away at
ruts_time_scale with wear_regrowth_days' half-life. clear_saved_ruts removes it. Bench --test=wear adds: 8 passes
saved, 8 read back, none after years away.

#### Round 16 built (20 Nov)

**16.1 Mud flows round stones.** Slumping and the mud waves took the layer's base as the deepest dent everywhere, so
liquid mud drained off a stone's top down to the base. Now a hidden stone's top is the base over it: the GPU's cover
buffer ends with the 64 stones nearest the followed point (shallow ones first; GroundSurface3D::_gpu_cells(),
GroundDeformGPU::STONE_VEC4S), and slump_params() and mud_liquid() take max_dent down to the depth of the highest stone
top over the texel (stone_dent()); the CPU's Columns does the same with _buried_depth(). The stamp's floor (15.2), the
slumping and the waves now agree. Bench --test=buried: in slurry a print beside a stone drains to 57 cm while the mud
over its top (38.8 cm down) stays at 37.8 cm, then the waves close the rut. Limits: 64 stones per field.

**16.2 Deposits kept with the ruts.** One zstd file (deposits.dep) in ruts_path with save_ruts: per 1 m cell the
deposit's depth, the silt skin's share, its colour and the river record already taken, then the ids of the stones
knocked loose. Read back the first time the folder is used, merged by the larger value; how dry a skin is comes back
with the crack state. Deposits do not wear away while away (fresh silt and fan mud last seasons). `clear_mud_deposits`
for a new game. Bench --test=silt: 4 mm saved, cleared, 4 mm read back dry.

**16.3 Stones shoved along.** A loose stone in front of a rolling tyre's middle, within its width and low enough for
its front to meet, is driven on toward the tyre's speed and out sideways toward the side it sits on (a bulldozing
front's berm: Bekker 1956; Ishigami et al. 2007), the push held to the passive earth pressure on its face over the rut
depth (Rankine, Kp 3, 20 kN/m3, 2 kPa) plus its sliding friction, at most 6 kN. Bench --test=buried: behind a tyre at
2 m/s a stone goes 1.95 m ahead and 11 cm aside in a second.

#### Round 17 built (20 Nov)

**Loose stones lie in the mud, not on the terrain.** A loose stone on a mud layer passes through the terrain's own
collider (a collision exception with the body under it on ground_collision_mask, found by a ray) and is held at its
settled depth in the deformed mud instead; off the mud, or thrown clear, the colliders hold it again. Settled depth:
bearing Nc tau A plus the displaced mud's weight against its weight, Nc 6 at the surface (Skempton 1951) rising to 13.9
fully under, which is Beris et al.'s (1985) criterion for a sphere held in a Bingham fluid (Y 0.048); tau the yield
stress of slurry and soft mud, the cohesion of stiff mud. Past the criterion it goes to the base.

**17.1 Ploughed.** Moving, the stone presses its underside each step as a moving round print, so the field's bow wave
heaps mud ahead and levees beside; it rides the undisturbed mud ahead (ground less dents), so it drops into ruts. The
mud brakes it: rolling resistance sqrt(z / d) of its weight (Bekker 1969) and Rankine passive pressure on its face. A
tyre shoving it meets it above its middle, so it also drives it down by (R - r) / (R + r) of the push, R about the
print's length. The push is now the tyre's (at most 6 kN) against that drag, not the 16.3 bulldozing cap. Bench
--test=buried: a 22 cm stone shoved 1.4 m through soft mud (moisture 0.7) leaves a 0.9 cm furrow and a 0.3 cm heap.
Small stones in stiff mud plough little (a 1 to 2 cm settled depth); big ones and soft mud plough deep, and soft
mud's own slumping (tan_repose tau / (rho g h)) softens a narrow furrow. That is the model, not tuned up.

**17.2 Sunk back.** At rest it creeps to its settled depth (Stokes with the plastic viscosity, at most 5 cm/s). Bench:
1.2 cm into stiff mud, 12.7 cm into slurry (tau 528 Pa) for a 22 cm stone. Out of sight (top 3 cm under the surface),
sunk or buried by mud laid over it (a fan, silt, a wave), it is a hidden stone again: a Buried record with a new id in
the cell's extras, a StaticBody near the followed point, kept in deposits.dep after the loose ids (count, then id, xyz,
r each; older files without it read as none). Bench: a 27 cm deposit over it hides it; saved, cleared, read back.

**17.3 Dry silt as dust.** A dried skin (water under 0.1, 0.5 to 2 mm and up) gives a tyre's dust as dry bare sand
does (playas and dried floodplains are the main dust sources: Gillette et al. 1980; Reynolds et al. 2007), and loses
what it gives at 1300 kg/m3; the tyre breaks its crust. Wind: u* = kappa U10 / ln(10 / z0), z0 1 mm; thresholds
0.3 m/s broken, 1 m/s crusted (Gillette et al. 1980, 1982); dust flux alpha Q, Q White's (1979) saltation flux,
alpha 1e-3 per m (Gillette 1977; Marticorena et al. 1997). The 64 strongest cells near the followed point feed
GroundDust3D each step; the skin thins at runoff_time_scale; a broken skin crusts over once wet again. Bench
--test=silt: 60 tyre steps lift 0.18 kg and thin the skin 0.14 mm; a 12 m/s wind thins the broken cell, not the
crusted one beside it.

#### Round 18 built (20 Nov)

**18.1 Carried.** A flowing mud drags a loose stone in it: 0.5 rho Cd A u^2 (Cd 1.2, as GroundDebrisFlow3D's push on
bodies) plus the yield stress's hold on the same face, 13.9 tau_y A (the factor matching Beris et al.'s criterion),
never more than brings it to the flow's speed; a yield-stress flow takes a stone held in its plug along at its own
speed (boulders rafted in debris flows: Iverson 1997). Sources: GroundDebrisFlow3D (now in the group
ground_debris_flows; get_flow_depth_at / get_flow_velocity_at, its density and yield stress) and the liquid mud layer
itself, its speed from the GPU mud waves' momentum balance held steady, q = h^2 / (3 nu) (g h S - tau_y / rho) down
the surface's slope (Liu and Mei 1989; Coussot 1997). The stone drops where the flow stops, and a deposit over it
hides it again (17.2). add_loose_stone(position, radius, velocity) puts one anywhere. Bench --test=carry: soaked mud
running down a 15 degree bank takes a 26 cm stone 2.4 m down and buries it in the toe; stiff mud holds it; a debris
flow sweeps one 60 m down its channel at up to 8 m/s and leaves it on the fan.

**18.2 Rolled onto.** Whether a tyre pushes a stone or rolls onto it is the wheel-over-step geometry: the tread meets a
stone standing p over the rut floor at alpha from the vertical, cos(alpha) = (R - p) / R; under 30 degrees it rolls
onto it, steeper it shoves it (16.3). Rolled onto, the carcass presses it with about its inflation plus carcass
stiffness (100 + 15 kPa, round 12's tyre_flat) over its section, kilonewtons, past what any mud here bears: pressed
in until its top is at the tyre's floor (or the base). Under the shoulder that force leans out by asin(offset /
(half width + r)); past the mud's lateral bearing (about 10 tau A, Randolph and Houlsby 1984) it squirts out sideways
at up to the tyre's speed. Bench --test=carry: under the middle its top goes from 4 cm over the floor to 0.3 cm under;
under the shoulder it is squeezed 37 cm out of the rut.

**18.3 Tracked-out mud as dust.** The film tyres and boots leave on firm ground (round 3) dries to a crumb traffic
grinds to powder, the main source of road dust's silt loading (US EPA AP-42 13.2.1; Kuhns et al. 2001 on track-out
at site exits). Dry (water under 0.1, none past 0.3) it gives a tyre's dust as dry sand does; what is lifted is owed
by its 1 m cell and taken off in texel-sized discs scattered round the spot (the field's washing-away path), so the
trail fades in patches, mass kept at 1.5 kg/m2 for a full film. The wind lifts it from u* 0.3 m/s as a broken silt
skin (17.3), over the 1 m cells films were laid in near the followed point. Bench --test=trackdust: a wet trail gives
none; dried, 60 tyre steps lift 0.1 kg and clear it at the spot; a 12 m/s wind lifts 0.22 kg and thins another spot
from full to 0.62 (the check now averages the whole trail, since the wind thins it in patches). Film cells are kept in deposits.dep (d35dbf2e), so a reloaded trail lifts in the wind too.

#### Round 19 built (20 Nov)

Picked 20 Nov: all three, in order. 19.3 (reloaded trails lift dust) went first as a bug fix (d35dbf2e).

**19.1 Streams carry loose stones.** Research: the onset of motion of a lone grain is a force balance, drag and lift
against submerged weight past the grain's pivot angle (Wiberg and Smith 1987; Komar and Li 1986). A stone standing
proud of a bed finer than itself moves at a Shields number far under the 0.045 of a bed of its own size (Komar and
Li's exposure, theta_c about 0.045 (D/K)^-0.7; Carling 1983 for lone boulders on fine beds). The water contract's
height and depth-averaged flow give the speed at the stone's face by a log profile over a fine bed (ks 1 cm):
u = U ln(z/z0) / (ln(h/z0) - 1), z0 = ks/30, z half the face standing above the bed. Drag 0.5 rho Cd A u^2 with Cd 0.76
(Schmeeckle et al. 2007, a grain on a bed), lift with Cl 0.2, buoyancy on its part under the surface. On firm ground
(rock, a gravel bed held by the colliders) it starts once drag beats tan 40 deg of its submerged weight less lift and
rolls on against 0.6 of it (Bagnold's dynamic friction for bedload), so it drops where the water slackens. On mud the
drag joins the round 17 hold (bearing depth, ploughing and rolling resistance), with the load less buoyancy and lift.
Bench --test=stream (12 cm stone, 0.5 m deep): still at 1.75 m/s, off at 2.25 m/s (u* 0.14 m/s, Shields 0.006), at
3 m/s rolled 6 m and stopped 15 cm into the still pool; sunk 5 cm in stiff mud it moved 1.7 m at 2.5 m/s, against 6 m
on rock. `stream_stones` (on).

**19.2 Loose stones kept.** deposits.dep now ends with each loose stone as it lies: id, position, rotation, mesh
radius, depth in the mud and the mud height it rides. Read back, they are placed at the first step with the ground up,
taken out of the hidden stones for good. A save made as the node leaves the tree (its stones have left it first)
uses each stone's transform from the last step. Bench --test=stream: a stone saved in stiff mud is read back by a new
node where it lay, at the same depth and turn (under 1 mm off).

#### Round 20 built (20 Nov)

Picked 20 Nov: all three, in order; plus game trails from the stand contract (asked by the world links work).

**20.1 Currents push fording vehicles.** Research: vehicle flood stability is a body on its tyres, buoyed by the water
it keeps out and pushed by the drag of the current on its wet face (Shu et al. 2011, Journal of Hydraulic Research;
Xia et al. 2014, Natural Hazards; Martinez-Gomariz et al. 2017 and UNSW Water Research Laboratory TR2017/07, full size
cars). Cars float near 0.5 to 0.6 m of still water: the body keeps out about half its box. PhysXVehicle3D now reads the
water contract each step (`river_forces`, on; `hull_seal` 0.5): the chassis box in 3 x 5 columns, each buoyed by its
wet part and dragged at 0.5 rho Cd A |u - v| (u - v), Cd 1.1, through the box's width across the flow; each tyre by its
wet circle segment. PhysX's tyres give the friction from the lighter load. Skipped with a PhysXBuoyancy3D child. Also
found: a braked car on soft ground crept sideways at 10 cm/s under a 40 N side push (PhysX's lateral sticky-tyre
damper, as round 11 found lengthwise); held, the lateral sticky regime now starts at rest too. Bench --test=fordcar
(1500 kg, braked, on the soaked bed): held at 0.3 m / 1 m/s and 0.5 m / 1 m/s; at 0.7 m and 3 m/s (drag 16.7 kN)
carried 1.6 m in 4 s, its tyres ploughing the soaked bed; 1.4 m of still water floats it.

**20.2 Scour round stones.** Obstacle marks at boulders (Euler, Herget and others 2017, laboratory and field: a crescent
hole round the upstream face, a sediment ridge trailing in the lee); the horseshoe vortex raises the bed shear about
fourfold at the foot (Melville and Coleman 2000). A cohesive bed erodes at Partheniades' E = M (tau / tau_c - 1),
M 2e-4 kg/m2/s, tau_c from 0.5 Pa rising with cohesion to 5 Pa: millimetres an hour, so a stone a few days in a stream
has its hole. Towards half the stone's width deep, slowing as it nears it; dug in three texel-sized discs (the nose
deepest, two at the flanks), a third laid as the tail two widths back, the rest to the water as silt. In liquid mud the
hole fills back as it is dug (the mud's own flow); only the silt shows there. Bench --test=stream (12 cm stone on a
soaked soil bed, 1.2 m/s, 2.5 days sped up): 7.8 cm deeper at its nose than its lee, a 3.4 cm tail, 3 kg of silt.

**20.3 Stones cloud the water.** A stone rolled along a muddy bed under water churns the mud it shoves aside: a
twentieth to three tenths of its fines (rising with the bed shear, as the ford's plume) go to the water contract as
suspended load, which carries it on as a plume. Bench --test=stream: a stone rolled 1.7 m through stiff mud at 2.5 m/s
stirred 2.6 kg into the water.

**Game trails.** WorldStand.trail_at() (passes a year, tread share) adds a trail's steady passes to the wear: walked
P times a year against the regrowth half-life T days, P T / (365 ln 2) passes over the tread. So a trail is packed
(Bekker values, slower to drain) and bare (the grass reads the wear map) and rain stands on it first. stand_changed
re-reads it. Bench --test=trails: a daily trail holds 43.3 passes; a wheel sinks 22 mm on it against 52 mm beside it;
in 20 mm/h rain water stands on it 16 frames (sped up) before the ground beside it.

### Round 21: proposed (21 Nov, card posted), research notes

**21.1 Engines drown, cars flood.** Hydrolock: an engine that draws water through its air intake stalls (water does
not compress; a running engine bends rods). Makers rate a wading depth from the intake's height (typically 0.5 to
0.9 m for off-roaders, raised by a snorkel to the roof line); off-road games model it as a stall past the intake with
a snorkel upgrade (SnowRunner's snorkel addon). A car's body is not sealed: water comes in through door seals and
the floor at an orifice rate Q = Cd A sqrt(2 g h) under the head outside (cars sink in one to a few minutes once
floating: flood-safety guidance and the vehicle tests of Martinez-Gomariz et al. 2017). Plan: PhysXVehicle3D
`air_intake_height` (with a snorkel raising it), the engine's torque cut and a stall once the water passes it;
`hull_seal` falling as the cab fills by the orifice rate over a leak area, so a floating car settles and sinks;
draining again on land.

**21.2 Wet trails pug.** Treading damage on wet soils (pugging, poaching): hooves at 100 to 400 kPa (deer to cattle)
punch into soil wetter than its plastic limit, leaving pocked mud that holds water and recovers over weeks to months
(Climo and Richardson 1984; Drewry 2006, review of natural recovery from treading). Plan: on trails from
WorldStand.trail_at(), when the soil is wet past a threshold, print hoof pocks over the tread at the trail's passes
(as the walker steps' prints), worn and slumped, healed by the wear's regrowth; puddles fill the pocks.

**21.3 Bow waves.** A vehicle wading at a walking pace pushes a bow wave that keeps the water lower round its engine
bay (the wading technique taught for off-roaders: steady, about 5 to 8 km/h); faster, the wave climbs over the bonnet.
A body in shallow water makes a wave by its depth Froude number Fr = v / sqrt(g h): near 1 the bow wave steepens and
breaks. Plan: through the water contract's disturb() (owner-keyed moving disturbance) with the chassis' wet width
and speed, the bow's height from Fr, the wake washing banks through the ford's bank path (_ford()), and the
water level at the intake (21.1) read with the bow's trough.

#### Round 21 built (22 Nov)

Picked 22 Nov: all three, in order.

**21.1 Engines drown, cabs flood.** PhysXVehicle3D `air_intake_height` (-1: three quarters up the chassis box) and
`snorkel` (the intake 0.25 m over the roof). Water over the intake stalls the engine (throttle cut); it starts again
once the intake has been clear 5 s. The cab fills through `hull_leak_area` (0.01 m2) at Q = 0.6 A sqrt(2 g |dh|) under
the head outside, its water adds weight and drains the same way on land. Getters is_engine_drowned(), get_cab_water().
Bench --test=fordcar: through 0.05 m2 a floating car took 2.16 m3 in 40 s and settled 0.76 m, 1.25 m3 left 10 s
after the water went; in 0.75 m of water a low intake stalled the car (0 m in 3 s, restarted 5 s after clearing),
the snorkelled one drove 7.1 m.

**21.2 Wet trails pug.** GroundSurface3D `trail_pugging` (on): every 2 s, trail cells from WorldStand.trail_at()
within 15 m of the followed point whose soil is wet (moisture 0.55 or more, or mud) take hoof prints at 11 per m2 per
pass, Poisson-drawn by hash (at most 64 a tick): a 6 x 8 cm hoof at 150 kPa, sunk by Bekker plate sinkage through
press_box(). The wear's regrowth heals them as any print. Bench --test=trails: 320 prints in ten wet days, 2.7 cm
deep; none on a dry trail.

**21.3 Bow waves.** The hull pushes the water at three keel points through the water contract's disturb() (owner
keyed, so the water's solver makes the moving wave and wake), strength 0.15 at rest rising to 1 at depth Froude
number 0.8, times how much of the keel is wet; cleared once out of the water. get_bow_wave_strength(). Bench
--test=fordcar: 0.07 at rest, 0.52 at 6.6 m/s in 0.75 m of water, cleared on leaving. Not built yet: the wake washing
the banks (through the ford bank path) and the bow's trough lowering the water at the intake.

### Round 22: proposed (22 Nov, card posted), research notes

**22.1 Wakes wash banks; the bow trough at the intake.** Boat-wake bank erosion is taken from the wave's energy at
the bank against the soil's critical shear (Bauer, Lorang and Sherman 2002; Glamore 2008, wake wave energy as the
erosion measure): the wave's height decays with distance from the sailing line and scour goes with H^2 over the
threshold. Plan: the water contract's wave height at the ford's banks (_ford()) feeds the scour path round 20 built
(Partheniades, bank cohesion), the washed soil to the water as silt. The intake reads the water lowered by the bow's
trough (a moving body draws the water down behind its bow; Fr near 0.5 to 0.7 for wading drill), so a steady pace
keeps an engine running a little deeper than parked.

**22.2 Parked trucks sink in.** Soft soil under a standing load keeps settling by consolidation and creep
(Terzaghi one-dimensional consolidation; secondary compression for organic soils), and an object lying in mud needs
more than its weight to lift free: the breakout suction (Vesic 1969, breakout of objects from the ocean bottom;
the breakout force rises with embedment time). Plan: a wheel or body resting on mud keeps pressing by the
consolidation rate of the soil, and a held contact adds a suction share to the rolling resistance that falls once
it moves; saved with the deposits.

**22.3 Wallows.** Deer and wild boar keep wallows: wet hollows rolled out and refilled through the warm season, with
churned rims and puddled floors (red deer wallows in the rut, Clutton-Brock et al. 1982;
boar wallowing to cool and shed parasites; sources to check before building).
Plan: WorldStand hollows on trails in wet soil get a wallow (press and berm, mud tint), revisited at the trail's passes,
and fill as puddles.

#### Round 22 built (22 Nov)

Picked 22 Nov: all three, in order.

**22.1 Wakes wash banks; the bow's trough at the intake.** GroundSurface3D.wash_wake() (`wake_wash`, on), called by
PhysXVehicle3D ten times a second while its bow is wet: waves v^2 / g crest to trough at the hull (at most 0.78 of the
depth), decaying as y^-1/3 to the bank (Havelock 1908; Kriebel and Seelig 2005), break and run up about their height
(Hunt 1959); the swash wets the strip (a fiftieth soaks in), and past the bank's critical shear (orbital speed
0.5 sqrt(0.78 g H), f_w 0.03) it erodes at Partheniades' rate, fines to the water. Honest scale: a single crossing
wears the bank by far less than a millimetre; a day of steady traffic past one stretch washed 1.5 kg. The intake
reads the water v^2 / 2g lower at a wading pace (Bernoulli round the bluff front), higher past depth Froude 0.7
(get_intake_trough()). Bench --test=fords: 0.17 m waves at the bank at 2 m/s, 0.02 m at 0.5 m/s, 1.53 kg washed in a
day at 2 m/s and none at a walk; --test=fordcar: 0.16 m lower at a wading pace, 0.40 m higher racing.

**22.2 Parked trucks sink in.** PhysXVehicle3D `parked_settling` (on). A wheel standing on sticky mud keeps settling
by creep, 0.02 x stickiness x (two tyre widths within the layer) x log10(1 + t / 1 s) (Buisman 1936), the time the
world's (WorldWeather.time_scale()); the soil sets up round it, so suction (half the contact at the soil's cohesion,
1 - exp(-t / 600 s); Vesic 1969 on breakout) resists the pull-away. Both let go over a contact length of rolling.
get_wheel_creep(), get_wheel_suction(). Bench --test=parked (new): five world hours in stiff mud sank the car
0.046 m against 0.032 m with it off; 0.37 kN of suction; 0.17 m in the first second against 0.24 m.

**22.3 Wallows.** GroundSurface3D `trail_wallows` (on). Hashed sites (one in about four 8 m squares) on trails from
WorldStand.trail_at(), where the soil is wet (moisture 0.7 or mud), not frozen, and within 6 degrees of level. One
passing animal in ten stops (assumption), scaled 0.25 to 1 by air temperature 10 to 25 C (boar wallow to cool:
Bracke 2011; red deer stags in the rut: Clutton-Brock, Guinness and Albon 1982). Each visit deepens a 1.6 m bowl by
2 cm toward 35 cm (field-guide sizes, not measured), a quarter of the spoil heaped on the rim as fresh mud. Wet soil
slumps back into a bowl left unused for weeks, so only well-used wallows stay open. get_wallow_count(),
get_wallow_visits(). Bench --test=trails: in 100 warm days 3 wallows, 35 visits, 8.8 cm deep; none in dry soil;
10 visits at 2 C.

#### Round 23 built (22 Nov)

Picked 22 Nov: all three, in order.

**23.1 Puddled clay holds rain.** Wheels and wallowing animals knead wet clay until its structure is gone; puddling
cuts saturated conductivity by one to three orders of magnitude (Sharma and De Datta 1985, puddled rice soils; the
same seal lines clay ponds and canals). Each wheel pass over clay wetter than about 0.55 (its plastic range) takes the
25 cm wear cell a tenth of the way to a full seal; each wallow visit a third. A full seal cuts infiltration to a
hundredth; it halves over 60 game days (drying cracks, frost, roots), is kept with the wear (file v2) and does not form
in sand. get_seal_at(). Bench --test=puddles: two equal hollows, one kneaded by ten passes (seal 0.65): of 15 l poured
in, 9.5 l stood after 33 h against 2.8 l.

**23.2 Body mud dries on the world clock and blows off.** The body's mud dries and rain soaks it on
WorldWeather.time_scale (a truck parked overnight dries overnight). A dried crust cracks into curling plates (Kindle
1926; Allain and Limat 1995): besides a hard jolt, road vibration under 0.8 g wears them off by fatigue (Basquin,
exponent 4), and the air past the body lifts them once 0.5 rho v^2 passes about three times a 4 mm plate's weight per
area (about 200 Pa, some 18 m/s). Bench --test=flakes: 2 s at a world clock of 60 took wetness to 0.60 against 0.99;
dried and driven 10 s on smooth ground, 11 of 12 clods blew off at speed against 1 at a crawl.

**23.3 Treads cut clods.** A grousered wheel slipping in cohesive soil shears it in blocks between its grousers (Wong
2008, sec. 2.6); chains' cross links act as tall, sharp grousers. A quarter of width x grouser depth x |slip| x tread
speed is cut from behind the contact, scaled by the tread's bite (road none, mud tyres 0.6, chains 1) and the soil's
stickiness; past the shedding speed the tread flings it with its clay, slower it drops just behind the wheel as clods
(GroundClods3D stands the big ones up). get_wheel_clods_thrown(). Bench --test=clog: 4 s spinning in stiff clay,
chains 9.7 kg, mud tyres 8.3 kg, road tyres none.

**Also:** the ground's height query adds the terrain's drawn relief (WorldGround.relief_at, terrain 57b253b5), so
prints and contact sit on the drawn bank (--test=relief). Float casts for 64-bit position builds (ground, vehicle,
walker steps).

#### Round 24 built (22 Nov)

Picked 22 Nov: both, in order.

**24.1 Sticky clay balls up on tyres.** Near its sticky limit clay sticks to clay better than to rubber or steel
(Atterberg's sticky limit; Fountaine 1954 on soil-metal adhesion): once the tread is packed, each turn through sticky
clay lays another skin on it, a few millimetres a turn at full slip, up to a smooth drum 6 cm thicker. The tyre rolls
and is drawn that much larger, and its inertia grows with the cake. The cake goes when the tread turns past its own
shedding speed, sqrt(ca r / (rho t)) for its thickness t, on firm ground and in water, and goes in chunks: what stays
is lopsided by a random share of each loss, up to a quarter of it out of balance. That unbalance m e w^2 (rotor
unbalance, ISO 21940) pulls the axle round with the wheel, so the truck shudders until it is spun clean.
get_wheel_cake(), get_wheel_shudder(). Bench --test=balling: 15 s spinning in stiff clay balled 1.0 cm on; floored,
it shed to nothing, shaking the axles at up to 5.7 kN on the way.

**24.2 Peat bogs.** Muskeg (Radforth's classification; MacFarlane 1969) is a fibrous root mat over soft peat. Wong's
model (Wong, Garber, Radforth and Dowell 1979; Wong, Radforth and Preston-Thomas 1982) gives p = (k_p + 4 m_m / D_h) z
up to a critical sinkage, where the mat ruptures and the bearing drops to the peat's; bevameter tests put the break at
8 to 14 cm (Massah and Noorolahi 2010). Here the mat's term rides on Bekker's kc / b (n 1, kc 180 = 4 m_m, k_p 300
kN/m3), the break at 12 cm, the peat under it kc 0, kphi 60, 4 kPa, about 80 cm deep. A light car's wheel sits 9 cm
down; a several-tonne truck's goes past 12 cm and tears through. The mat is a web of roots, so slipping does not dig
it out, but a tyre spinning past a slip of 0.7 shreds it. Its fibres are viscous: a landing or a jolt passes, a load
kept past the break for a quarter of a second tears it. Torn cells are kept with the wear (file v3), bear as peat for
every later wheel and foot, and knit over years (half in 5). Bogs sit where water gathers on flat ground (a wetness
index over 11 to 14 and under 2 to 5 degrees on the data map; waterlogged ground without one), one in three 24 m
squares, 12 to 22 m across with wobbled edges. Peat is little sticky. peat_bogs, get_peat_bog_at(), tear_mat(),
get_mat_tear_count(). Bench --test=peat: a 1.5 t car stayed on the mat at 10 cm; a 7 t truck tore it and dropped
72 cm; a car's wheel in the torn rut sinks 57 cm against 9 cm on the mat.

#### Round 25 built (22 Nov)

Picked 22 Nov: both, in order.

**25.1 Peat water fills torn ruts.** A bog's water table stands within a few centimetres of its surface (Rydin and
Jeglum 2013, The Biology of Peatlands), held in the acrotelm, the loose living top layer whose hydraulic conductivity
runs to about 1e-3 m/s against 1e-7 to 1e-5 in the humified catotelm below (Ingram's diplotelmic bog; Holden and Burt
2003). A rut torn through the mat cuts that layer, so water runs in through its walls until it stands at the table,
dark humic water that hides the rut's depth; water over the table drains back the same way. Each torn 25 cm cell moves
toward the level 5 cm under the bog's surface with time constant L / K, L about half a rut (15 cm): 150 s of world
time, capped at a twentieth of the difference per update since the level is read back from the GPU late. Water on a
bog does not soak in (the peat is saturated): its infiltration is cut to a thousandth. peat_water,
get_peat_water_seeped(). Bench --test=peat: the 7 t truck's ruts took 1.3 m3 and stood 63 cm deep, 3 cm under the
table; 40 l poured in rose 13 cm over it and drained back to 1 cm. Water on the unbroken mat ponds.

**25.2 Clay packs the wheel arches.** Sticky clay thrown off the tread sticks inside the arch (soil-to-steel
adhesion, Fountaine 1954; the known trouble of mudguards and fender liners in clay, why farm and military vehicles run
open or wide arches): about 60% of what the tread throws hits the arch, and the sticky share packs at the clay's
density over the arch's inside, half a circle round the tyre by its width; only flung clay, from a couple of metres a
second of tread speed (slower it drops off under the tyre). Once it (and the tyre's balled cake, 24.1)
closes the gap of arch_clearance (8 cm by default) the tyre shears through it along the arc it cuts in,
2 sqrt(2 R d) long for an interference d, at a stiff remoulded clay's 15 kPa: a torque against the wheel, added to its
brake response each substep, while the tyre shaves the interference off in about half a turn. Standing water washes it
out slowly, rain rinses it, and dried it cracks off with the body's mud. arch_clearance, get_wheel_arch_mud(),
get_wheel_arch_drag(). Bench --test=arches: 10 s spinning in stiff clay packed 1.2 cm in a 12 cm arch; a 1 cm arch
rubbed at up to 223 N m and the car got 1.12 m against 1.18 m; open wheels none.

**Scan on the mud (texture audit, 25 Nov).** TerrainMaterial3D's mud_scan slot (brown_mud_02, 1.3 m; see
scan-textures.md) goes by the patch's own maps (material.cpp ground_deform_mud()), not by where the cover is opened:
ruts, digs, prints and the rims and laid mud they leave pressed a centimetre or more, and turned or mud-filmed ground,
where the worked soil is wet mud: the gloss the patch already gives freshly worked wet ground (the deform map's
roughness factor: mud 0.35 to 0.7, moist soil 0.6, dry soil 1), or the pore water's saturation past half; wallows of
muddy water and fresh mud deposits at any wetness. Ground bench: --view=ruts --meadow --ground=mud --mud=0.3,0.7
--scans=dir (--scans-mud=0 for the frame without). A dry rut keeps the soil's
crumb, a puddle on turf keeps the turf. The scan is detail only (no baked shading); the mud's darkening, film and water
stay the patch's. One function, so raster and the path tracer (which runs the material's fragment) agree.

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
