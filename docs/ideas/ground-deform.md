# Ground deformation: footprints and ruts in sand, soil, mud and leaf litter

Research note, 10 Nov 2026. Written before the code. Goal: the deform field that already gives
snow its trails (docs/ideas/snow.md, stage 2) also marks bare ground: boots, hooves, wheels and
dropped bodies leave prints in sand, soil, mud and forest litter, walls slump to the material's
angle of repose, the ground's own moisture decides how a print looks and lasts, and both
renderers draw the same geometry.

## What we already have

- `SnowSurface3D`: a camera-following field of dent and rim heights (32 m at 6.25 cm), mass
  conserving, pressed by bodies in `deform_groups` with Bekker sinkage, tiles cached when the
  camera leaves, a heightfield collision patch, refilled by snowfall and wind. Its geometry is a
  vertex-displaced grid traced from its capture under path tracing.
- `SnowCover3D`'s ground query: `TerrainDeformer3D`, `TerrainDataMap3D`, anything with
  `get_height`, Terrain3D by name, or a raycast. Any ground node needs exactly this.
- `WeatherMap3D` wetness (0 to 1, from rain and drying) and canopy closure: the moisture that
  turns soil to mud and the crowns that drop litter.
- `TerrainGenerator3D.get_geology_at()`: soil depth and bare rock (rock does not print).
- `TerrainDeformer3D`: authoring-time brushes on a coarse heightmap. Not for footprints.

## How others do it

- **Sumner, O'Brien and Hodgins 1999, "Animating Sand, Mud, and Snow"** (Computer Graphics Forum
  18(1)). A height field of columns. A rigid body pressing in moves the material under it either
  into compression or sideways to the edge of the contact, outward from the body, so a lip forms
  around it. Then an erosion pass: wherever the slope between neighbouring columns exceeds the
  material's angle of repose, a share of the difference moves downhill, repeated until stable.
  The three materials differ only in four numbers: the compression ratio, the angle of repose,
  the erosion share per step, and how far from the contact the lip is spread. This is the method
  we build; our snow field already does the first half, mass conserving.
- **Onoue and Nishita 2003, "Virtual Sandbox"** (Pacific Graphics), and **2005, "An Interactive
  Deformation System for Granular Material"** (Computer Graphics Forum 24(1)): the same height
  field with the repose test run only where something changed, plus material lying on top of
  objects. The "only where it changed" part is what keeps it cheap; we keep a per-tile settle flag.
- **Zhu and Bridson 2005, "Animating Sand as a Fluid"** (SIGGRAPH), and the material point method
  (Stomakhin et al. 2013, "A Material Point Method for Snow Simulation"; Klar et al. 2016,
  "Drucker-Prager Elastoplasticity for Sand Animation"): full 3D granular flow. Correct for
  avalanches and spilling buckets; far too costly for a ground that is mostly still and a few
  centimetres deep. Not used.
- **Shipped games.** Batman: Arkham Origins (Barre-Brisebois, "Deformable Snow Rendering in Batman:
  Arkham Origins", GDC 2014), Rise of the Tomb Raider (Michels and Sikachev, "Deferred Snow
  Deformation in Rise of the Tomb Raider", GPU Pro 7) and God of War Ragnarok (GDC 2023) all
  render a camera-centred deformation texture into a tessellated or displaced ground; the same
  texture drives mud in their later work. None conserves mass or slumps walls: a print is a
  stamped shape. Unreal Engine's Virtual Heightfield Mesh with a runtime virtual texture is the
  engine-side version of the same idea. Journey's sand (Edwards, "Sand Rendering in Journey", GDC
  2013) is the reference for how disturbed sand reads: darker, with sharper glints on the walls.
- **Soil mechanics, for the numbers.** Bekker's pressure-sinkage relation (Bekker 1969,
  "Introduction to Terrain-Vehicle Systems") with the soil values tabulated in Wong, "Theory of
  Ground Vehicles" (dry sand: n 1.1, kc 0.99, kphi 1528; sandy loam at 15% moisture: n 0.7, kc
  5.27, kphi 1515), the same table our snow values come from. Angle of repose of dry sand 30 to
  35 degrees (Al-Hashemi and Al-Amoudi 2018, "A review on the angle of repose of granular
  materials", Powder Technology 330). Damp sand stands much steeper because water bridges hold the
  grains (Hornbaker et al. 1997, "What keeps sandcastles standing?", Nature 387), which is why a
  print in a wet beach keeps sharp walls and one in dry dune sand caves in to a dish. Mud is a
  yield-stress fluid (Coussot 1997, "Mudflow Rheology and Dynamics"): it keeps a print's shape
  when still and creeps back slowly, and it is nearly incompressible, so what a boot pushes down
  squeezes up as a lip. Wet surfaces darken (Lekner and Dorf 1988, "Why some things are darker
  when wet", Applied Optics 27), so freshly turned ground, which is moister than the dried crust,
  reads darker until it dries.

## What we build

### It outgrows the snow module

Sand, soil and mud are not snow, and the shared parts are not snow-specific. So:

- **A new module, `modules/ground`.** It holds the deform field as an internal C++ class
  (`GroundDeformField`: dent and rim arrays, the camera-following shift, the tile cache, pressing
  with mass conservation, the mover scan, Sumner's slumping), the ground query moved out of
  `SnowCover3D` (`GroundHeightQuery`), and the new node `GroundSurface3D`.
- **`SnowSurface3D` moves onto the shared field** in the same change, losing about 500 lines of
  its own copy; its behaviour and its checks stay the same. The snow module then depends on the
  ground module (it already depends on `terrain_deform` and `weather`).
- `TerrainDeformer3D` stays as it is (authoring brushes on the terrain thread's module).

### GroundSurface3D

One node per scene, following the camera like the snow surface, over a field 32 m wide at
6.25 cm by default.

- **Materials, per field texel, from rules.** `ground_type` picks the base (sand or soil). The
  moisture comes from `WeatherMap3D.get_wetness_at()` (or a fixed `moisture` when there is no
  weather map) and blends each parameter from its dry to its wet value, so soil turns to mud and
  dry sand to damp sand as it rains, with no separate mud type. Under tree crowns
  (`get_canopy_closure_at()`) a litter layer lies on top, `litter_depth` times the closure.
  Where `TerrainGenerator3D` says bare rock, nothing prints. A `type_map` image can override the
  base per place (a sandy path through a field).
- **Per-material parameters** (dry to wet):

  | | sand | soil to mud | leaf litter (layer) |
  |---|---|---|---|
  | Bekker n, kc, kphi | 1.1, 0.99, 1528 (Wong, dry sand) to firmer damp sand | 0.7, 5.27, 1515 (Wong, sandy loam 15%) to soft mud, fitted to 5 to 10 cm under a boot | compresses to a third of its depth under a boot |
  | Rim share (pushed out, the rest compacts) | 0.8 to 0.7 | 0.4 to 0.95 | 0.1 |
  | Angle of repose | 33 to 70 degrees | 40 to 25 degrees | 60 degrees |
  | Slumping | fast (walls collapse within a second when dry) | slow creep when wet (minutes) | none |
  | Loose depth (deepest print) | 0.3 m | 0.08 to 0.3 m | the layer, then the soil under it |
  | Turned ground | darker (damp grains from below), drying over minutes | darker and glossier when wet | soil shows through |

- **Pressing.** The same contact and Bekker sinkage as the snow field, with the column's own
  parameters. Bodies are found the same way (`deform_groups`, shapes, rigid body mass, character
  mass and boot area). The meta keys `ground_mass` and `ground_contact_area` override, and the
  snow ones still work.
- **Slumping (Sumner 1999).** After a press, every changed tile is marked settling. For each
  settling texel and its eight neighbours, the step in the surface (ground plus rim minus dent)
  is compared with the step the undisturbed ground already had there: only what deformation added
  beyond the angle of repose moves downhill, a share per step set by the material, until nothing
  moves and the tile sleeps. Undisturbed slopes are never touched, so a hillside steeper than
  sand's repose does not start to slide.
- **Ageing.** Wind slowly fills dry sand prints (the weather map's wind speed over the threshold
  for sand transport, about 5 m/s at 10 m), rain softens soil prints, and wet mud creeps back.
  "Rain filling prints" with water and debris kicked up were not picked.
- **Snow on top.** Where a `SnowCover3D` has more than a few centimetres, the snow surface takes
  the print and the ground does not.

### Drawing it: the ground's own material, lowered terrain, a displaced patch

Prints go below the terrain, so the terrain must get out of the way where the patch is, in both
renderers. Discarding terrain fragments would need an any-hit test in the path tracer for every
terrain triangle; moving the terrain's vertices does not, because displaced vertices are traced
from their capture (docs/ideas/displaced-hit-varyings.md). So:

- **A `BaseMaterial3D` feature, `ground_deform_enabled`**, which a ground material turns on. On
  the terrain it lowers the vertices inside the field by `skirt` (0.5 m), faded to nothing over
  the field's outer edge, so the terrain hides under the patch with no gap. On the patch (a copy
  of the same material the node makes, with `ground_deform_patch` set) it lifts the vertices to
  the ground plus rim minus dent and sets the normal and tangents from the same height function.
  Turned-ground darkening and roughness come from the same field texture. The ground lattice, the
  field and their rects are global shader uniforms (`ground_deform_map`, `ground_deform_rect`,
  `ground_deform_height`, `ground_deform_height_rect`, `ground_deform_skirt`).
- **The same lines as shader code**, `BaseMaterial3D.get_ground_deform_shader_code()`, for
  `ShaderMaterial`s and Terrain3D's shader override (one source, generated by the same C++).
- **Under path tracing**, the geometry needs nothing: both the lowered terrain and the patch are
  captured. The turned-ground tint on a `BaseMaterial3D` patch is applied in the ray generation
  shader like wetness and snow: a material flag (subsurface bit 29) and the field texture and rect
  in `rt_params` 60 to 63 (`RT_PARAM_COUNT` 60 to 64). Shader materials run their own code there.
- **The patch** is a tensor grid over the field: 6.25 cm over the middle 16 m, 12.5 cm out to the
  edge, so prints are geometry, not normal-map detail, and the path tracer sees all of it. UVs are
  world XZ in metres (the generator's terrain has no UVs; its materials map by world position).
- **Collision.** The terrain's collider stays where it was: bodies stand on the undisturbed ground
  and the print forms under them by Bekker sinkage (2 cm in dry sand, up to 10 cm in mud).
  `get_surface_height_at()` gives foot placement the true surface. Unlike snow, which lies above
  the terrain, a heightfield patch here would sit under the terrain's own collider and do nothing.

### Crusts (after the ground)

Snow that thawed and refroze carries a crust (SNOWPACK, Lehning et al. 2002, "A physical SNOWPACK
model for the Swiss avalanche warning"; Crocus, Vionnet et al. 2012, "The detailed snowpack scheme
Crocus and its implementation in SURFEX v7.2"). SnowCover3D tracks a crust strength from melt and
refreeze; a boot below the crust's bearing pressure stays on top, one above it breaks through,
leaving plates at the print's edge. Separate change, after this one.

## Cost and checks

- **CPU.** Pressing as the snow field. Slumping only on settling tiles: a 4 m tile is 4,096 texels
  times eight neighbours per pass, a few passes per frame while it settles, then it sleeps.
  `get_step_time_msec()` reports it; the bench prints it.
- **GPU.** The patch grid is about 115,000 vertices, captured each frame under path tracing like
  the snow surface; the terrain's material gains a texture read per vertex. Timings go on the PC
  queue.
- **Checks (bench `docs/bench/ground`, `--test`).** Volume is conserved through press and slump;
  a print in dry sand slumps to a dish whose walls are no steeper than 34 degrees, one in damp
  sand keeps walls over 60; boot sinkage 1.5 to 3 cm in dry sand, 5 to 10 in mud; undisturbed
  slopes steeper than the repose angle do not move; prints under the trees go through the litter;
  bare rock does not print; the snow checks pass unchanged after the move to the shared field.
  Shots: a boot trail across dry sand, damp sand, soil, mud and litter, raster against path traced
  at 100% crop.

## Built (10 Nov 2026)

As designed above. Checks (`docs/bench/ground --test`, all pass): a boot sinks 2.0 cm in dry sand, 0.6 cm
in damp sand, 0.2 cm in dry soil, 1.8 cm in moist soil and 8.0 cm in mud, 3.4 cm through 4 cm of litter;
a press keeps volume (80% to the rim in dry sand, the rest compacted) and so does slumping; a 12 cm
pit in dry sand slumps from 62 to 33 degree walls in 3 s, one in damp sand keeps 61; a pit in mud keeps
63 degrees after 10 s and creeps to 25 after 15 minutes; turned ground dries; a 50 degree undisturbed
slope does not move; bare rock does not print; a body in `ground_deform` prints by itself. The snow checks
pass unchanged on the shared field. `--view=trail --ground=dry_sand|damp_sand|soil|mud|litter` renders
the trail; raster and path traced match at 100% crop. Physics step 2 ms while dry sand settles under a
walker on the CPU here, under 0.1 ms otherwise.
