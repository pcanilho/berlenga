def can_build(env, platform):
    return True


def configure(env):
    pass


def get_doc_classes():
    return [
        "WorldBurnScar3D",
        "WorldContracts",
        "WorldFire",
        "WorldGround",
        "WorldSnow",
        "WorldWater",
        "WorldWeather",
    ]


def get_doc_path():
    return "doc_classes"
