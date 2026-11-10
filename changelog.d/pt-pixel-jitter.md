### Changed
- Path tracing without Ray Reconstruction spreads each pixel's samples after the first across the pixel (box filter), so detail finer than a pixel, such as hair strands, is antialiased instead of sparkling
