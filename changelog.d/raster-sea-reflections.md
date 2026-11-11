### Fixed
- Raster water seen from above now reflects the sky and the sun's glitter by the exact Fresnel term, as the path tracer does; before, it was an unlit tint with no reflection at all (no glitter path, no sky at grazing angles). PhysXWaterSurface3D `raster_reflections` (on) is the opt-out.
