def can_build(env, platform):
    env.module_add_dependencies("weather", ["world"])
    return True


def configure(env):
    pass


def get_doc_classes():
    return [
        "RainFall3D",
        "SkyClock",
        "WeatherMap3D",
        "WeatherRunoff3D",
        "WeatherSkyMaterial",
    ]


def get_doc_path():
    return "doc_classes"
