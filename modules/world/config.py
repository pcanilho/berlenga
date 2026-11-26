def can_build(env, platform):
    return True


def configure(env):
    pass


def get_doc_classes():
    return [
        "FootPlacementModifier3D",
        "WorldBeaverDam3D",
        "WorldBurnScar3D",
        "WorldCaves",
        "WorldContracts",
        "WorldExclusion3D",
        "WorldFire",
        "WorldGround",
        "WorldSnow",
        "WorldWater",
        "WorldWeather",
    ]


def get_doc_path():
    return "doc_classes"
