### Changed

- PhysX: bodies with the same friction and bounce share one material instead of each making its own, so streaming a region no longer costs a step that grows with the world (5,000 statics: 3.5 to 8 ms less), and large worlds stay under PhysX's 65,535-material limit.
