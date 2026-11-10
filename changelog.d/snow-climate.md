### Added

- `SnowClimate`: a place's winter (a WGEN-style weather generator) baked over the whole terrain by `SnowCover3D.bake_climate()` through the snowpack model: snow lines where the lapse rate puts them, shaded faces keeping their snow, loaded lee slopes. `SnowCover3D.climate` keeps it as a world tier carried on by the weather; it seeds the near square and is drawn beyond it by materials and the path tracer (`snow_world_map`, `snow_world_rect`).
- `SnowCornice3D`: the cornices the world tier grows on lee crests in the right wind, drawn near the camera with collision.
- `SnowCover3D.precipitation_gradient`: more precipitation higher up.

### Changed

- Blowing snow (`SnowFall3D`) also lifts a fresh surface on a settled pack.
