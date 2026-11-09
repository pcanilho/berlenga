### Added
- Shader outputs SCATTERING and SCATTERING_ANISOTROPY: a shader-defined medium (IOR + ABSORPTION) can scatter light under path tracing; PhysXWaterSurface3D uses them so deep water reads blue (#172)

### Fixed
- Path-traced caustics fade out at the edge of their area instead of drawing a dark line on the seabed (#172)
