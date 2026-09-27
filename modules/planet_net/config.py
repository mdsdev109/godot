def can_build(env, platform):
    env.module_add_dependencies("planet_net", ["enet"])
    return env.module_check_dependencies("planet_net")


def configure(env):
    pass
