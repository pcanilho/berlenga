### Added

- PhysXFlow3D `diffuse_transmission` (on by default): in raster, bright gas lets the scene behind it through, blurred, by multiple scattering (two-stream transmittance beyond the direct beam), as under the path tracer.

### Fixed

- The far side of a dense, bright Flow cloud lit from behind it no longer draws bright white in raster: its deep scattering orders fade by diffusion theory's transport coefficient.
