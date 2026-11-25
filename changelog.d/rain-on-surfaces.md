### Added

- Rain on glass: drops that land, grow and run down the pane in jerks, leaving beaded trails and leaning with the wind, with a fine mist the runners and wipers clear; in the path tracer each drop refracts the view through it (BaseMaterial3D Wetness from the weather on transparent or Transmission materials).
- Standing water in a surface's hollows from the weather's pooled water, with raindrop rings on it (BaseMaterial3D `wetness_standing_water`, `wetness_standing_relief`).
- Footsteps, wheels and splashes send wave trains across puddles on the ground (GroundSurface3D, `WorldWater.impulse` and `disturb`).
- Raindrop rings on water and puddles under a RainFall3D with no WeatherMap3D (`rainfall_rate_override`).

### Fixed

- Falling rain no longer draws as dark streaks under the path tracer.
