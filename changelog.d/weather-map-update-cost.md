### Changed

- The weather map's cloud cover noise takes its per-update constants once instead of per cell (16 % fewer instructions, identical cover), and its texture packing runs on worker threads instead of the main thread.
