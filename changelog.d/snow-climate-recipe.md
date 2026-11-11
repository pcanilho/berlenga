### Added
- Snow: `SnowClimate.recipe` takes the terrain's `ClimateRecipe` (temperatures, lapse rates, wet-day wind, the year's precipitation and its seasonality), and `SnowCover3D.climate_map` takes a `TerrainDataMap3D` whose orographic precipitation field sets where more snow falls, in place of `precipitation_gradient`. One climate for snow, rivers and plants.
