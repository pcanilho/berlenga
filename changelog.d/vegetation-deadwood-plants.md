### Added

- Ground plants meet fallen wood: `PlantSpecies.deadwood_rule` keeps plants off logs (default), grows them on a log's upper surface (`DEADWOOD_ON`, `deadwood_min_radius`) or along its side (`DEADWOOD_BESIDE`, `deadwood_band`, `deadwood_uphill`), reading the axes of the trees module's logs, fallen branches and sticks; `VegetationField3D.get_deadwood_at()`. `PlantGenerator.make_floor()` adds moss, wood sorrel and seedlings on logs, ferns along them and leaves banked on their uphill side.
