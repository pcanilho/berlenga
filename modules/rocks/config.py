def can_build(env, platform):
    env.module_add_dependencies("rocks", ["terrain_deform", "weather"])
    return True


def configure(env):
    pass


def get_doc_classes():
    return [
        "RockGenerator",
        "RockField3D",
    ]


def get_doc_path():
    return "doc_classes"
