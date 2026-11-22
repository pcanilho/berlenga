### Fixed

- Flow gas in strong wind no longer fills `max_blocks` within seconds: blocks the wind moves fast stay allocated only while they hold some smoke, so empty air the wind blows through is freed.
