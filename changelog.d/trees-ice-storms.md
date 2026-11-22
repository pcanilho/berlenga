### Added

- Ice storms load trees: a new weather contract reports glaze (clear ice from freezing rain), and wind-rigged trees carry its weight, bow and break under it, and stand up again after the thaw (`TreeWind3D.glaze_load_enabled`).
- Freezing rain: a warm layer aloft (`WeatherMap3D.warm_nose_temperature`) turns rain over sub-zero ground into glaze, accreted by Jones' ice model and melted in a thaw (`get_glaze_at`).
- Clear ice on surfaces glazed by freezing rain, raster and path traced (`BaseMaterial3D.glaze`, on for trees).
