### Changed

- `TerrainDataMap3D`: rivers that list their cross-sections are only queried near their channel, so world-size maps with rivers bake about 18 times faster (same result); water surfaces always bake as a level plane, without their waves.
