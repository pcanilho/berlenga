### Added
- Path tracing: `rendering/pathtracing/transmission_bounces` (default 4). After a path's first bounce, crossing glass, ice or water does not spend `pathtracing_max_bounces`; up to this many extra segments are traced instead. Clear ice under shaded eaves and back faces of panes no longer go black at 3 bounces.
