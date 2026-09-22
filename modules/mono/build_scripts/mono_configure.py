def is_desktop(platform):
    return platform in ["windows", "macos", "linuxbsd"]


def is_unix_like(platform):
    return platform in ["macos", "linuxbsd", "android", "ios"]


def module_supports_tools_on(platform):
    # PAYAGAN ANG ANDROID DITO:
    return is_desktop(platform) or platform == "android"


def configure(env, env_mono):
    # is_android = env["platform"] == "android"
    # is_web = env["platform"] == "web"
    # is_ios = env["platform"] == "ios"
    # is_ios_sim = is_ios and env["arch"] in ["x86_32", "x86_64"]

    if env.editor_build:
        # Siguraduhing kasama ang android sa allowed platforms:
        if env["platform"] not in ["windows", "macos", "linuxbsd", "android"]:
            raise RuntimeError("This module does not currently support building for this platform for editor builds.")

    # Tiyakin na kung Android, hindi ito maghahanap ng desktop-specific hostfxr static libraries
    # dahil dynamically loaded ito via dlopen sa gd_mono.cpp mo.
