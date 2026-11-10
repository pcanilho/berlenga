### Added

- Weather: `WeatherLightning3D` makes lightning from the weather map's storms. The rate follows the cloud top (Price and Rind 1992), and the share that reaches the ground follows the cold cloud depth (Price and Rind 1993). A ground flash is a tortuous, branching channel with return strokes, lit under path tracing as an emissive light and in raster by a light at the channel. Flashes glow inside `WeatherSkyMaterial` clouds. Thunder is synthesized from the channel itself and arrives late by its distance. Scripts can call `strike()` and listen for `struck`.
