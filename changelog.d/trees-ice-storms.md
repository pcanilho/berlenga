### Added

- Ice storms load trees: a new weather contract reports glaze (clear ice from freezing rain), and wind-rigged trees carry its weight, bow and break under it, and stand up again after the thaw (`TreeWind3D.glaze_load_enabled`).
- Freezing rain: a warm layer aloft (`WeatherMap3D.warm_nose_temperature`) turns rain over sub-zero ground into glaze, accreted by Jones' ice model and melted in a thaw (`get_glaze_at`).
- Clear ice on surfaces glazed by freezing rain, raster and path traced (`BaseMaterial3D.glaze`, on for trees).
- Glazed trees as geometry: a clear ice coat round twigs and limbs and icicles under them, seen alike by raster and the path tracer (`TreeGenerator.glaze_mm`).
- Near trees in a vegetation field grow the weather's glaze as ice geometry, in bands, and swap as it builds or melts.
- After an ice storm: broken tops, stripped crowns and birch bent over in arches, by species, and a northern hardwood stand after one (`TreeGenerator.ice_storm_years`, `STAND_ICE_STORM`).
