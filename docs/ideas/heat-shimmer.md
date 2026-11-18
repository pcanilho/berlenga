# Heat shimmer: light bending through hot air

Paulo picked raster and path traced (card, 18 Nov). Source of heat: the temperature field every `PhysXFlow3D`
already simulates (fires from `PhysXFireSpread3D` and campfires included), so the shimmer sits exactly where the
hot gas is, moves with it, and needs no artist set-up.

## Physics

- Air's refractive index follows its density (Gladstone-Dale): n - 1 = 2.9e-4 at 15 C and sea-level pressure,
  scaling as 288.15 / T at constant pressure, so hot air has the lower index and light bends toward the cooler
  side. Per kelvin near ambient that is about -1e-6 (Merzkirch, "Flow Visualization", 1987; Settles,
  "Schlieren and Shadowgraph Techniques", 2001).
- Ray equation for a slowly varying n (eikonal, paraxial): d/ds (n dr/ds) = grad n, so a ray's direction turns by
  the integral of the gradient across it, delta = integral of grad_perp n ds. Background-oriented schlieren
  measures exactly this deflection (Raffel 2015, "Background-oriented schlieren (BOS) techniques").
- Size: a flame edge (several hundred kelvin over a 10 cm cell) turns a ray by a few 1e-4 rad, about a pixel at
  1080p; the shimmer people see comes mostly from the turbulent temperature fluctuations finer than the grid.
  Fire plume turbulence intensity: rms temperature fluctuation about 0.3 to 0.4 of the mean excess on the axis
  (George, Alpert and Tamanini 1977, "Turbulence measurements in an axisymmetric buoyant plume"). The fluctuations
  below the grid are modelled as a noise field of a few centimetres' correlation length carried upward at the
  plume's rise; each march step adds an independent kick with variance scaled by correlation length over step
  (the random-walk phase screen used for atmospheric seeing, Tatarskii 1961), so the result does not depend on
  the step length.
- Temperature in kelvin: Flow's temperature is unitless. The fork's convention (PhysXFireSpread3D, campfire
  showcase) puts `temperature_range` at a sooty flame's ~1,500 K core, so a Flow temperature equal to
  `temperature_range` is `heat_shimmer_kelvin` (1,200 K) above the air; the air's own temperature comes from the
  weather contract (`air_temperature_at`).

## How engines do it

- Unreal: a refraction / distortion material on a sprite or mesh over the fire, an artist-painted normal map
  offsetting the scene colour (screen-space). Frostbite and CryEngine likewise (heat haze particles). They bend
  nothing physical: the strength is a material setting.
- The fork's version keeps the screen-space cost of that approach but takes the offset from the simulated
  temperature, and under path tracing bends the rays themselves.

## Fork implementation

- Raster (`FlowShimmerRD`, after Flow composites and scatters): per pixel inside the gas's screen rectangle,
  march the view ray through Flow's sparse atlas to the scene depth, integrate grad_perp n (resolved field plus the
  sub-grid kicks), find the deflection's centroid along the ray, and fetch the scene colour where the bent ray
  lands: the point bend + d' (D - t_c) projected to the screen. A pixel whose landing point lies in front of the
  hot air keeps its colour (no foreground smearing).
- Path traced (`raytracing_volume_inc.glsl`, `rt_volume_bend`): camera rays march the same field before the trace
  and leave along the bent line through the deflection's centroid. The line differs from the straight one by
  delta * (t_c - t) before the bend point (millimetres), so hot gas behind a surface does not move it.
- Settings on `PhysXFlow3D`: `heat_shimmer` (on), `heat_shimmer_kelvin`, `heat_shimmer_turbulence`. Flows with no
  hot emitter (dust, mist) skip the pass.
