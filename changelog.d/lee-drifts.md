### Added

- Snow drifts into the lee of rocks, walls, buildings and vehicles and blows off their tops (`SnowCover3D.drift_obstacles`); blown sand drops behind them (`GroundSurface3D.obstacle_lee`).
- Boulders and vehicles block the wind (`RockField3D.wind_obstacles`, `PhysXVehicle3D.wind_obstacle`); any node in the `weather_shelter` group can list its own boxes with `get_wind_obstacles(rect)`.
- Weather contract: `WorldWeather.obstacle_height_at` and `obstacle_lee_at`.

### Fixed

- Wind obstacles on raised terrain counted their height from the weather's flat ground height, so they cast a lee far too long.
