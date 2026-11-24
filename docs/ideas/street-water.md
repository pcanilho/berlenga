# Streets that hold water: road relief and street water

Paulo, 23 Nov: puddles must move (a big ripple sloshes past the edge, wets new ground, changes the shape), so the
water needs a real bed: "deform the road to actually hold the puddles", and "make deformations not repeat".
Order: road relief (this note's first half), then the street water solver over it (second half), then the game's
iWave WaterSim swapped for it (game PR), checked with a tyre through a puddle, raster and path traced.

## Road relief (RoadRelief3D): research

How a used asphalt street is shaped, from pavement engineering (measured, not invented):
- Cross slope: a crown at the centre falling 1.5 to 2 % to each kerb (AASHTO Green Book, high-type pavements); a
  one-way street falls one way. A gutter pan along the kerb, about 0.3 m wide, a few centimetres lower (8 %).
- Rutting: two wheel paths per lane, about 1.8 m apart (a car's track), each a dip about 0.4 to 0.5 m wide with
  low shoulders of displaced asphalt beside it (the "W" transverse profile of shear rutting; FHWA LTPP transverse
  profile studies, Simpson 1999/2001). Depth grows with traffic: a few mm on a quiet street, 10 to 20 mm on a busy
  one (severity limits in rut surveys: 6 / 12 / 25 mm). Wheels wander a little, so the ruts drift along the road.
- Roughness: ISO 8608 classifies road profiles by their displacement spectrum, Gd(n) = Gd(n0) (n / n0)^-2 with
  n0 = 0.1 cycles/m: class A (very good) Gd(n0) = 16e-6 m3, B (good) 64e-6, C (average) 256e-6. Synthesised the
  standard way: a sum of cosines with random phases, amplitudes from the spectrum (the "harmonic superposition"
  method used for vehicle-dynamics road profiles), here with random directions for an isotropic surface.
- Settlement: long, shallow sags (metres to tens of metres across, millimetres to a centimetre deep), and
  "birdbaths": local depressions where water stands after rain (centimetre deep, a metre or two across).
- Patches (utility cuts) sit a few millimetres proud or sunk with stepped edges; potholes are rarer, 3 to 6 cm deep,
  ragged, steep-walled.

Non-repetition (Paulo's rule): every field is a function of world position (x, z), never of a tile: the cosines'
phases and directions come from a seed, sags from hashed gradient noise, and depressions, patches and potholes are
placed per world cell by a hash (a jittered grid, one candidate per cell), so two streets, or two ends of one,
never match, and adjacent road pieces join without a seam.

## Road relief: design (built in modules/ground)

RoadRelief3D (a MeshInstance3D): the road in its own frame, X along (0..length), Z across (-width/2..width/2), Y
up. Its relief h(x, z) is the sum above (crown and gutter, ruts per lane scaled by traffic, ISO 8608 roughness,
sags, depressions, patches, potholes), all from world x, z. It builds:
- the displaced mesh (a grid of `cell`, 8 cm default: the ruts and bowls are geometry that raster, the path tracer
  and the water all see; finer texture stays in the material), normals from the analytic gradient, UV and UV2 in
  metres along and across (as the game's Kit.road);
- an optional static collider of the same surface (PhysX trimesh where PhysX is the physics server);
- get_height_at(world point) from the same function, for the water and for anything placed on the road.

## Street water: research

What the water on a street has to do (Paulo, 23 Nov): fill the relief's hollows from rain and runoff, slosh when a
tyre or a foot goes through, spill over a puddle's rim onto dry road and wet it, change shape as it does, and soak in
and dry out. A ripple field over fixed puddle masks (the game's iWave WaterSim, Tessendorf 2004) cannot: its water
has no volume, so a wave stops at the mask. Water that moves its own edge needs a depth-carrying solver with wet/dry
fronts, the shallow water equations:
- Virtual pipes (O'Brien and Hodgins 1995; Mei, Decaudin and Hu 2007): each cell keeps its outflow through four
  pipes, accelerated by the surface-height difference, scaled down together when they would take more than the cell
  holds. Depth never goes negative, volume is kept exactly, and dry cells are simply cells with no water, so fronts
  move for free. The standard real-time choice (the engine's PhysXLiveWater3D on the CPU, the ground's standing
  water on the GPU).
- Thin sheets need bed friction or they race: the local inertial form (Bates, Horritt and Fewtrell 2010) with
  Manning's friction taken semi-implicitly, n = 0.013 for asphalt (Chow 1959, Open-Channel Hydraulics, table 5-6), and
  the face-depth rule for wet/dry faces (flow depth = the higher surface over the higher bed). The same grid then
  carries both the runoff film (a sheet a fraction of a millimetre deep running to the ruts and gutters) and the
  puddles, so the film costs nothing extra.
- Bodies displace volume (Chentanez and Mueller 2010, sec. 6.1): the part of a body below the water's surface counts
  as head in the pipes, so a tyre lifts water in front of it into a bow wave, leaves a hollow behind that refills, and
  the water it pushes out runs onto dry road.
- Waves in water a centimetre deep travel at sqrt(g h), about 0.3 m/s: the pipes carry them. Raindrop rings (5 mm to
  2 cm) are below any grid a street window can afford; they stay procedural (Lagarde 2013, world-anchored rings), drawn
  only where the solver says there is water.
- Camera window: a square of cells following the view as a ring buffer (toroidal addressing, as the game's WaterSim):
  a texel holds world cell (i mod N, k mod N), so the window slides by whole cells without copying; cells entering it
  take their bed from the road (RoadRelief3D.get_height_at, or a ray cast) and start dry.
- Out: soaking into the pavement (porous asphalt drains quickly, dense asphalt almost not at all) and evaporation
  (open water on a road in mild weather: about 0.1 to 0.3 mm/h, Penman 1948 orders of magnitude). A damp channel
  remembers where water stood and fades over drying_time, so a puddle that drained leaves a dark patch.
- Drawn by the road's own material, not a second mesh: a puddle a centimetre deep is a surface layer (Lagarde,
  "Water drop 2/3", 2013: albedo darkened by wetness, roughness to the water's, the normal to the water's), so the
  raster and the path tracer read the same map and nothing floats or z-fights.

## Street water: design (StreetWater3D, modules/ground)

- GPU compute on the main RenderingDevice, one shader, four passes per step: OCCUPY (bodies' displaced height per
  cell), FLUX (local inertial pipes with Manning), DEPTH (flux divergence, rain, film rule, soak, evaporation, damp),
  PACK (an RGBA16F map: depth, the surface's slope x and z, damp). LOAD writes beds of cells entering the window.
- Window: resolution N (512) cells of cell (2.5 cm), 12.8 m, following follow_path or the camera.
- Rain: rain_rate mm/h plus the weather's rainfall_at at the window, on the whole road; it runs off as the film. The
  film is not a separate system and has no switch: it is how the puddles fill (rain landing only on standing water
  never starts one, tried), and it costs nothing beyond the grid. The cost levers a game's options can use are the
  grid's: simulating (off: the water holds still, drawn as it was), resolution and step_rate.
- Bodies: submit_sphere/clear_sphere (a tyre is a sphere of its radius at the hub, a foot a small one) and the water
  contract's disturb; add_water for drips and pours.
- Shading: get_map() (a Texture2DRD, sampled with repeat at world x z / window size) and get_window(); materials in
  `materials` get both each frame; get_shader_include() returns the GLSL the road material calls.

## For a game's streets (24 Nov, before the game's swap)

- A street that slides past (the metro's city strip): RoadRelief3D.relief_frame and StreetWater3D.frame_path put the
  relief and the water in the strip's space, so the road is not rebuilt as it moves and the water rides with it (bodies'
  velocities are over the street). The road shader gets street_water_frame (world to water space).
- Puddles already there: a street entering the window at speed would start dry. RoadRelief3D floods its own grid at
  each build (Priority-Flood, outlets the road's ends and its inlets) and StreetWater3D.prefill starts each entering cell
  with that share of the ponding (the storm's fill), then the solver takes over.
- Storm drains: inlets in the gutters every inlet_spacing metres (world-hashed) or where the game says, each a
  depressed curb inlet (FHWA HEC-22, 2013): a grate 3 cm down with an apron; water reaching it leaves the street, so a
  long gutter does not fill end to end.
- get_depth_at(): the map read back a few times a second, for footsteps, splashes and spray.
