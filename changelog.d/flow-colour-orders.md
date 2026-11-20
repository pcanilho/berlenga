### Fixed

- Raster Flow gas tinted by its colour once where the path tracer tints every scatter: near-white gas such as snow powder drew grey and coloured gas too light and neutral. Each multiple-scattering order now takes the colour to its own power, and the octaves are refit to the path tracer.
