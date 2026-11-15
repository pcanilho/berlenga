### Added
- PhysXFlow3D.sun_scattering (off by default): in raster, directional lights light the smoke through the scattering pass with their shadow cascades, so walls shade it as they shade Godot's volumetric fog.

### Fixed
- PhysXFlow3D light scattering: lights now scatter with a normalized phase and Godot's light energy units, as the path tracer and volumetric fog do, and light reaching through dense smoke includes a multiple-scattering approximation, so raster smoke brightness follows the path-traced one.
