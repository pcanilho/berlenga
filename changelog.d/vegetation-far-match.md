### Changed

- Vegetation: the far colour that replaces plants past their range is matched live to the plants on screen, so the ground no longer lightens into a ring where the cover plants end (`VegetationField3D.far_match_view`, on by default).
- Vegetation: floor plants keep clear of a stone a tree's roots grow over (mesh meta `tree_root_stone`).
- Vegetation: fallen leaves and twigs no longer land on open ground far from any crowns (the litter rule's fade ran below zero litter).
