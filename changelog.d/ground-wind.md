### Added
- Weather: wind near the ground. WeatherMap3D shelters the wind inside plant canopies (Cionco's profile; `add_canopy()`, `canopy_map`) and in the lee of obstacles (`add_shelter_box()`, the `weather_shelter` group), and carries gust patches of a few metres across fields (`gust_patch_size`, `gust_patch_strength`); `get_wind_shader_code()` gives shaders the same wind as `get_wind_at()`
