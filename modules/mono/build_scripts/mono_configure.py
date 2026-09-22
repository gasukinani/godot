import os
import sys

def is_desktop(platform):
    return platform in ["windows", "macos", "linuxbsd"]

def is_unix_like(platform):
    return platform in ["macos", "linuxbsd", "android", "ios"]

def module_supports_tools_on(platform):
    # Payagan ang Android para sa Editor / Tools build
    return is_desktop(platform) or platform == "android"

def configure(env, env_mono):
    is_android = env["platform"] == "android"
    is_web = env["platform"] == "web"
    is_ios = env["platform"] == "ios"
    is_ios_sim = is_ios and env["arch"] in ["x86_32", "x86_64"]

    # 1. Platform validation para sa Editor Build
    if env.editor_build:
        if not module_supports_tools_on(env["platform"]):
            raise RuntimeError(
                f"This module does not currently support building for {env['platform']} for editor builds."
            )

    # 2. Android Configuration (Editor at Template builds)
    if is_android:
        env_mono.Append(CPPDEFINES=["ANDROID_ENABLED", "UNIX_ENABLED"])

        # dynamic loading (dlopen) ang ginagamit ng custom gd_mono.cpp
        if env["platform"] == "android":
            env_mono.Append(LIBS=["dl", "log"])

        # Kung editor build sa Android, tiyaking kasama ang tools flag
        if env.editor_build:
            env_mono.Append(CPPDEFINES=["TOOLS_ENABLED"])

        return

    # 3. Desktop Configurations (Windows, Linux, macOS)
    if is_desktop(env["platform"]):
        if env["platform"] == "windows":
            env_mono.Append(LIBS=["ole32", "shell32"])
        elif env["platform"] == "linuxbsd":
            env_mono.Append(LIBS=["pthread", "dl"])
        elif env["platform"] == "macos":
            env_mono.Append(LINKFLAGS=["-framework", "CoreFoundation", "-framework", "Security"])

    # 4. iOS / Web Fallback (kung sakaling i-compile sa future)
    elif is_ios:
        env_mono.Append(CPPDEFINES=["IOS_ENABLED", "UNIX_ENABLED"])
    elif is_web:
        env_mono.Append(CPPDEFINES=["JAVASCRIPT_ENABLED", "WEB_ENABLED"])
