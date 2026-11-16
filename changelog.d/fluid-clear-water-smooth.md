### Changed
- PhysXParticleFluid3D's default liquid is clear water: transmission with refraction (n 1.33), Fresnel reflection and depth tint, instead of a translucent blue tint.
- MPM fluid surface is smooth: a wider surface kernel (three particle spacings) and normals continuous across grid cells, so it no longer shades as facets.
### Fixed
- MPM fluid takes as many substeps as `mpm_stiffness` needs (up to 128) instead of quietly softening the water, which made it sag like a gel in layers at fine particle sizes.
- Emitted fluid leaves at its rest spacing from a round emission sphere, so a high `emission_rate` pours a stream instead of a sheet or a burst.
- PhysXParticleFluid3D's raster shadow copy is left out of the path tracer, where it turned the water black.
