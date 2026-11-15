### Fixed
- Landscape3D cliff relief: vertices gather along the ledge lips and joints (split where the relief strays from the triangle edge), so path-traced ledge shadows are clean lines instead of beads; about 18% more triangles on cliff views.
- Landscape3D cliff relief: ledges are now lit per pixel by TerrainMaterial3D from the same relief the vertices follow, so their shadow edges are clean lines instead of a saw-tooth under a low sun.
- Tall falls: the notch now opens just below the brink instead of cutting through it, so the lake and river stay graded to the lip and no water is left hanging above the rock.
### Added
- Landscape3D `get_cliff_point()`: where the drawn surface lies over a point once `cliff_relief` has moved it (for water laid on faces); joint blocks stand out more and ledge steps are rounded over 1.5 m.
- TerrainGenerator3D: with `outwash_age` above 0, the basins the last glaciation wore are filled from the bottom up by the sediment their catchments yielded since the ice left, so a main trough gets a flat floor while small cirques keep their lakes.
- TerrainGuide3D: hand-drawn guides the terrain generator reads before erosion: raise (a ridge or peak), lower (a valley or bowl) and bar (hard rock at the curve's heights with a lip and an optional rock-slope apron), so drawn features still drain and look eroded; coast (sea inside or beside a drawn shore, which wanders naturally) and beach (beach width, or none, along a stretch of coast).
- TerrainGenerator3D `guide_preview`: a coarse run (`guide_preview_scale` times the cell size) a moment after a guide is moved or changed, so drawing guides in the editor shows their effect without a full run.
- TerrainGenerator3D touch-ups: a TerrainBrush3D under the generator is applied to the generated ground and the water solved again (`apply_touch_ups()`), so a raised dam backs up a lake and a dug hollow holds a pond; moving a brush re-applies it a moment later.
- TerrainMaterial3D `far_start_from_vegetation`: the far blend ends where a streaming VegetationField3D's placed plants end.
- TerrainMaterial3D `cliff_self_shadow`: ledges of Landscape3D `cliff_relief` shadow each other per pixel in raster (a short march toward the sun through the same relief); spatial shaders gain `sun_direction()` and the fragment output `SUN_SHADOW`.
