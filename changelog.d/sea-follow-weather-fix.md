### Fixed

- Weather: `snap_to_settings()` refreshes the map's cells at once (so followers read the snapped state, not the old one), and the first update after it eases from there instead of snapping again.
- Water: `follow_weather` no longer starts from the weather map's stale wind (it snaps on the first cells after a snap), and `WeatherMap3D.advance()` grows the sea along with the rest of the weather.
