### Fixed
- Raster rivers seen from above now reflect the sky and the sun's glitter by the exact Fresnel term, like the sea since `raster_reflections` on PhysXWaterSurface3D; PhysXRiver3D `raster_reflections` (on) is the opt-out. The sea and rivers share the code (water_raster_reflections.h).
