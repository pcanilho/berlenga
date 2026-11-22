### Changed
- Landscape3D: meshes for new levels are built on worker threads and shown together once all are ready, so moving no longer stalls the main thread (42 ms to under 1 ms when 200 m of chunks change). With a TerrainGenerator3D source (`stream_heights`), the ground is read as a 4 m grid first and in full per chunk where the view needs it, nearest first; `load_all_heights()` reads the rest. Heights baked to an image load the 4 km, 1 m ground in under a second.

### Added
- TerrainGenerator3D: `changing` signal before a generate replaces its result.
