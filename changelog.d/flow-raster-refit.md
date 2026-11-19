### Fixed

- Flow smoke and dust in the raster renderer now match the path tracer under a real sky: sunlit gas was too dark and sky-lit gas too bright (the earlier parity fix was measured against a background the path tracer does not light with).
- Horizontal banding in tall, dense Flow clouds in raster: the sun and sky volumetric shadow maps now step once per gas cell.
