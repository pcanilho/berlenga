### Fixed
- Tall falls: the notch now opens just below the brink instead of cutting through it, so the lake and river stay graded to the lip and no water is left hanging above the rock.
### Added
- Landscape3D `get_cliff_point()`: where the drawn surface lies over a point once `cliff_relief` has moved it (for water laid on faces); joint blocks stand out more and ledge steps are rounded over 1.5 m.
- TerrainGenerator3D: with `outwash_age` above 0, the basins the last glaciation wore are filled from the bottom up by the sediment their catchments yielded since the ice left, so a main trough gets a flat floor while small cirques keep their lakes.
