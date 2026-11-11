### Fixed
- TerrainGenerator3D: the erosion detail no longer cuts trenches along river channels on an alluvial floor or a comb of gullies into a fan toe: it fades over the first half metre of alluvium (was 2 m), and the fade reaches 32 m past the deposit and then thins out over 3/8 of `detail_wavelength`.
- TerrainMaterial3D: the detail normal fades where the geometric detail does (alluvium, water, coastal strip), from a new `terrain_detail_keep_map` shader global, so alluvial plains no longer shade with a ripple pattern.
