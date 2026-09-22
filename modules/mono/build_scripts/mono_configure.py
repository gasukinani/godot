def is_desktop(platform):
    return platform in ["windows", "macos", "linuxbsd"]


def is_unix_like(platform):
    return platform in ["macos", "linuxbsd", "android", "ios"]


def module_supports_tools_on(platform):
    # AYOS: Pinapayagan ang android para sa tools/editor build
    return is_desktop(platform) or platform == "android"


def configure(env, env_mono):
    if env.editor_build:
        if env["platform"] not in ["windows", "macos", "linuxbsd", "android"]:
            raise RuntimeError("This module does not currently support building for this platform for editor builds.")
