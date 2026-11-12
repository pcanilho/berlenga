### Added
- Spatial shaders can set `OPAQUE_PREPASS_THRESHOLD` in fragment(): the alpha from which a `depth_prepass_alpha` fragment writes depth (0.99 by default)

### Fixed
- Hair cards no longer show the head's outline through the hair: their dense cores draw depth first
