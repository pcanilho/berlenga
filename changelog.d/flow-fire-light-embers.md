### Added

- `PhysXFlow3D.fire_light`: a burning Flow emitter lights its surroundings with a soft-shadowed light that flickers at the flame's puffing frequency (raster and path traced; under the path tracer it stands in for the flame's glow as a light source, so the ground is lit without noise). `fire_light_energy` scales it.
- `PhysXFlow3D.embers`: glowing blackbody embers rise from a fire as streaks along their flight, `ember_rate` per second.

### Changed

- Flow's default `heat_ramp` is a blackbody: flames have a bright core and sharp tongues, and cooler gas reads as smoke instead of an orange glow.
- The path tracer honours a light's `light_volumetric_fog_energy` when it lights participating media.

### Fixed

- Screen-space refraction in MPM fluid no longer shows objects in front of the liquid.
