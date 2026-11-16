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

### Stage 2: water in ruts, drying and cracks

- Ruts collect rain and runoff (the runoff routing already stops water in hollows): a water depth per
  cell, drawn as a thin water surface in the ruts (real geometry, shadowed, path traced), which also
  softens the mud under it.
- Drying by evaporation from the weather: albedo lightens, gloss goes, then a crust. Once the crust is
  thin and dry, desiccation cracks: a Voronoi pattern sized by the drying layer's thickness, opened as
  geometry in the near field, curled plate edges; rewetting closes them, and each cycle rounds the
  pattern towards hexagons (Goehring 2010).

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
