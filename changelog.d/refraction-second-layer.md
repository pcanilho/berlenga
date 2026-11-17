### Added

- Two-layer refraction in Forward+ (`rendering/transparency/refraction_second_layer`, on): water and glass that read the screen texture (Transmission, refraction) now show the refracting surface behind them, such as the pool under a falling stream or a second pane of glass, instead of only the opaque scene. Depth peeling, one extra layer; only frames with such a surface in view pay.
