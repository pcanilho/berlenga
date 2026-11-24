def can_build(env, platform):
    env.module_add_dependencies("ground", ["terrain_deform", "world"])
    return True


def configure(env):
    pass


def get_doc_classes():
    return [
        "GroundClods3D",
        "GroundDebrisFlow3D",
        "GroundDust3D",
        "GroundLegMud3D",
        "GroundStreamers3D",
        "GroundSurface3D",
        "RoadRelief3D",
        "StreetWater3D",
    ]


def get_doc_path():
    return "doc_classes"
