### Fixed
- Path tracing works again: the light buffer declarations lost their semicolons in the cloud shadow change, so the path tracer's shaders failed to compile and every view fell back to raster.
