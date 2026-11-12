### Added
- Hair cards for distant strand hair: PhysXHair3D bakes textured cards from its groom (clustered clumps of strands, shared by every node with the same groom) and draws them in raster past `card_distance`, simulating only the guides and the cards (the path tracer keeps the strands) (`cards_enabled`, off by default; `card_count`, `card_distance`)
