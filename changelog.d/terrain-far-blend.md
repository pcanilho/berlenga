### Added
- TerrainMaterial3D: `far_blend_enabled`, `far_start`, `far_fade`. Past `far_start` the ground takes the plant cover's colour from the `terrain_cover_map` / `terrain_cover_rect` globals, replacing the near grass tint and plants' far colour, before wetness and snow. Off by default.
