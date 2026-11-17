### Added

- `PhysXFlow3D.fire_light`: a burning Flow emitter lights its surroundings with a soft-shadowed light that flickers at the flame's puffing frequency (raster and path traced; under the path tracer it stands in for the flame's glow as a light source, so the ground is lit without noise). `fire_light_energy` scales it.
- `PhysXFlow3D.embers`: glowing blackbody embers rise from a fire, `ember_rate` per second.
