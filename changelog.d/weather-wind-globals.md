### Fixed
- Shaders using `WeatherMap3D.get_wind_shader_code()` compile without a WeatherMap3D in the scene (the wind globals are declared as project settings), and `weather_wind()` returns no wind there.
