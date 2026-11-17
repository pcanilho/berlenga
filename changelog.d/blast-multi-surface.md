### Added

- `PhysXBlastAuthoring.build_prefractured` takes pieces with several materials (a Mesh or an Array of surfaces; `PhysXBlastAsset.materials` by slot) and an optional `health` per bond (`PhysXBlastAsset.bond_healths`, a multiple of the destructible's health), so a tree's limbs draw bark, wood and leaves and a twig joint breaks before a trunk joint. `PhysXDestructible3D.get_piece_chunks()` lists the chunks a piece carries.
