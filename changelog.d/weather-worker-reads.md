### Fixed

- WeatherMap3D: weather contracts read from worker threads no longer touch the scene tree (follow position, own position, SkyClock and terrain lookups are cached on the main thread).
