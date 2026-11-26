### Added

- `rendering/pathtracing/max_dispatch_time_ms`: path tracing dispatches are sized from the GPU time the last frames took, so an expensive frame (a dense volume, many samples a pixel) is split into shorter dispatches instead of tripping the system's GPU watchdog.

### Changed

- Path tracing traces a pixel's samples in runs over several dispatches when one row's samples alone are too many for a dispatch.
