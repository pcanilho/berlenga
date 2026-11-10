### Added
- Strand hair in raster drawn as AMD TressFX does: camera-facing ribbons widened to at least about 0.7 px with coverage opacity (hashed; smooth with TAA or MSAA), and the same coverage in shadow maps so hair shadows itself; path tracing keeps real-width strands
- IN_PATH_TRACER spatial shader built-in: true while the path tracer captures or shades the geometry, false in raster
