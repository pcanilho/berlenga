### Added

- Fire makes its own wind: a `PhysXFireSpread3D` draws air in toward its burning front, so grass and `PhysXCloth3D` nearby lean into it and fronts heading toward each other pull together. `fire_wind` (on by default), `get_fire_wind_at()`, `WorldFire.wind_at()`, shader globals `fire_wind_map` and `fire_wind_rect`.
