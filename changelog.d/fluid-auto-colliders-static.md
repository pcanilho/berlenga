### Changed
- PhysXParticleFluid3D (MPM): `mpm_auto_colliders` is on by default and takes static bodies and every shape of a body, so a basin built from boxes holds the fluid without listing it.
### Fixed
- PhysXParticleFluid3D's isosurface casts shadows in the raster renderer (a shadow-only copy of the surface casts it).
