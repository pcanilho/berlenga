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
