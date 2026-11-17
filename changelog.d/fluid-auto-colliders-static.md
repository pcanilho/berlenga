### Changed
- PhysXParticleFluid3D (MPM): `mpm_auto_colliders` is on by default and takes static bodies and every shape of a body, so a basin built from boxes holds the fluid without listing it. Scenes that relied on only the bodies in `mpm_colliders` (for example particles meant to fall through a ground body and be handed to a height field) should turn `mpm_auto_colliders` off.
### Fixed
- PhysXParticleFluid3D's isosurface casts shadows in the raster renderer (a shadow-only copy of the surface casts it).
