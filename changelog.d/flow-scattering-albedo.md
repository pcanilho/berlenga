### Added

- PhysXFlow3D `scattering_albedo` (default 0.9): how much light the gas keeps at each scatter, in both the path tracer and the raster renderer, so very white media such as snow powder (about 0.99) stay bright inside instead of turning grey.
- The raster renderer adds the light of scattering orders past its three multiple-scattering octaves for gas whiter than the 0.9 default, so it brightens with `scattering_albedo` as the path tracer does.
