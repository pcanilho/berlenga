### Added
- `PhysXWaterfalls3D`: tall free falls (`plunge_falls`). The jet sheet whitens and comes apart after its break-up length (`6 q^0.32` m), then falls as clumps drawn as soft streaks, each slowed by air drag to its own terminal speed, widened by the air's eddies (`column_turbulence`) and carried by the weather's wind (`column_density` sets how many). Spray lands at the speed the drag leaves, and a tall mist veil hangs along the column. `get_plunge_falls()` reports break-up length, landing speed, fall time, column width and wind drift.
- `PhysXWaterMist3D`: `column_height` and `column_share` feed mist up a falling column as well as at the source.

### Changed
- `PhysXWaterMist3D`: mist fades out over the last cells at the box's sides and top, so the box never shows as a hard-edged block.
