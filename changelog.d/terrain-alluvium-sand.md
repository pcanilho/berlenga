### Fixed
- TerrainMaterial3D with a generator painted sand on all ground under `beach_height`, turning low coastal plains into sand flats. Sand now follows the generator's beaches (passed in the geology map's soil channel as -1..0), plus the ground under the sea.
- TerrainGenerator3D with `deposition`: the bedrock detail filter (gullies, crest noise) fades out over the first 2 m of alluvium, so fan fronts and terrace banks no longer grow rocky knobs.
