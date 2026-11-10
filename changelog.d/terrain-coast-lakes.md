### Added

- `TerrainGenerator3D` coast and lakes: `ocean_edges` or `sea_map` make sea that the land drains to, with a Dean-profile seabed and beaches; lakes form behind natural dams across valleys (`lake_count`, or `lakes` markers), at their spill level or lower when evaporation wins; `build_water()` creates the sea and lake surfaces (PhysXWaterSurface3D when present) and `build_river_paths()` ends rivers in them.
