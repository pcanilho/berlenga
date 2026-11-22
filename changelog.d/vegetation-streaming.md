### Added

- `VegetationField3D.streaming`: places plants in tiles on worker threads around the camera (or `stream_target`) out to `stream_radius`, each species only out to its visibility range, identical to a full `populate()`; the canopy, forest floor and far colour maps follow the camera. About 2 to 3 ms of main-thread time a frame at a run through dense woodland (`stream_budget_msec`).
