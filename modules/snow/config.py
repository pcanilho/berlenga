def can_build(env, platform):
    env.module_add_dependencies("snow", ["terrain_deform", "weather"])
    return True


def configure(env):
    pass


def get_doc_classes():
    return [
        "SnowClimate",
        "SnowCornice3D",
        "SnowCover3D",
        "SnowFall3D",
        "SnowSurface3D",
    ]


def get_doc_path():
    return "doc_classes"
