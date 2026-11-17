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

### Stage 3: mud on wheels and bodies

- **Tyres clog.** Per wheel a tread fill fraction, filled by slip in sticky mud, lowering traction
  toward a slick's; flung off when the tread's centripetal acceleration beats the mud's adhesion, which
  spawns clods (stage 4). Mud mass adds to the wheel.
- **Bodies cake.** Per mesh a mud coverage mask in texture space, filled by contact and landing spray,
  drying lighter over time, washed by water depth and rain. One material layer (wet mud over the base
  material, like the snow layer), the same in raster and path traced.
- Characters: boots and legs pick up mud the same way (this was "leg caking", not picked for the ground
  in November; offered again here).

### Stage 4: spray and clods

- GPU ballistic particles from each slipping wheel (speed from the wheel's surface speed, mass from
  the mud lifted): droplets and blobs as small meshes, not sprites, so path tracing keeps them. Landing
  writes a splat into the ground field and into a body's mud mask.
- Large clods as PhysX rigid bodies that break up, as MudRunner's chunks; MPM grains (built for sand)
  for wet mud ploughed by a blade or a bucket, with a cohesive Drucker-Prager setting.

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
