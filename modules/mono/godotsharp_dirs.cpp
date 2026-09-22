/**************************************************************************/
/*  godotsharp_dirs.cpp                                                   */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#include "godotsharp_dirs.h"

#include "mono_gd/gd_mono.h"

#ifndef TOOLS_ENABLED
#include "utils/path_utils.h"
#endif

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/os/os.h"

#ifdef TOOLS_ENABLED
#include "editor/file_system/editor_paths.h"
#endif

#ifndef TOOLS_ENABLED
#include "core/config/engine.h"
#ifndef ANDROID_ENABLED
#include "core/io/file_access.h"
#endif
#endif

namespace GodotSharpDirs {

String _get_expected_build_config() {
#ifdef TOOLS_ENABLED
	return "Debug";
#else
#ifdef DEBUG_ENABLED
	return "ExportDebug";
#else
	return "ExportRelease";
#endif
#endif
}

String _get_mono_user_dir() {
#if defined(ANDROID_ENABLED)
	// Sa Android, gamitin palagi ang internal app data directory para ligtas sa permissions
	return OS::get_singleton()->get_user_data_dir().path_join("mono");
#elif defined(TOOLS_ENABLED)
	if (EditorPaths::get_singleton()) {
		return EditorPaths::get_singleton()->get_data_dir().path_join("mono");
	} else {
		String settings_path = OS::get_singleton()->get_data_path().path_join(OS::get_singleton()->get_godot_dir_name());

		String exe_dir = OS::get_singleton()->get_executable_path().get_base_dir();
		Ref<DirAccess> d = DirAccess::create_for_path(exe_dir);
		// Safe Guard: I-check kung valid bago gamitin para maiwasan ang SIGSEGV
		if (d.is_valid() && (d->file_exists("._sc_") || d->file_exists("_sc_"))) {
			settings_path = exe_dir.path_join("editor_data");
		}

		if (OS::get_singleton()->has_feature("macos") && exe_dir.ends_with("MacOS") && exe_dir.path_join("..").simplify_path().ends_with("Contents")) {
			exe_dir = exe_dir.path_join("../../..").simplify_path();
			d = DirAccess::create_for_path(exe_dir);
			if (d.is_valid() && (d->file_exists("._sc_") || d->file_exists("_sc_"))) {
				settings_path = exe_dir.path_join("editor_data");
			}
		}

		return settings_path.path_join("mono");
	}
#else
	return OS::get_singleton()->get_user_data_dir().path_join("mono");
#endif
}

#if !TOOLS_ENABLED
static const char *platform_name_map[][2] = {
	{ "Windows", "windows" },
	{ "macOS", "macos" },
	{ "Linux", "linuxbsd" },
	{ "FreeBSD", "linuxbsd" },
	{ "NetBSD", "linuxbsd" },
	{ "BSD", "linuxbsd" },
	{ "Android", "android" },
	{ "iOS", "ios" },
	{ "Web", "web" },
	{ nullptr, nullptr }
};

String _get_platform_name() {
	String platform_name = OS::get_singleton()->get_name();
	int idx = 0;
	while (platform_name_map[idx][0] != nullptr) {
		if (platform_name_map[idx][0] == platform_name) {
			return platform_name_map[idx][1];
		}
		idx++;
	}
	return "";
}
#endif

class _GodotSharpDirs {
public:
	String res_metadata_dir;
	String res_temp_assemblies_dir;
	String mono_user_dir;
	String api_assemblies_dir;

#ifdef TOOLS_ENABLED
	String build_logs_dir;
	String data_editor_tools_dir;
#endif

private:
	_GodotSharpDirs() {
		String res_data_dir = ProjectSettings::get_singleton()->get_project_data_path().path_join("mono");
		res_metadata_dir = res_data_dir.path_join("metadata");
		res_temp_assemblies_dir = res_data_dir.path_join("temp").path_join("bin").path_join(_get_expected_build_config());

#ifdef WEB_ENABLED
		mono_user_dir = "user://";
#else
		mono_user_dir = _get_mono_user_dir();
#endif

		String exe_dir = OS::get_singleton()->get_executable_path().get_base_dir();
#ifdef MACOS_ENABLED
		String res_dir = OS::get_singleton()->get_bundle_resource_dir();
#endif

#ifdef TOOLS_ENABLED
		String data_dir_root = exe_dir.path_join("GodotSharp");

#if defined(ANDROID_ENABLED)
		// I-check sa parehong internal at external storage
		String internal_mono_dir = OS::get_singleton()->get_user_data_dir().path_join("mono");
		if (DirAccess::exists(internal_mono_dir.path_join("GodotSharp"))) {
			data_dir_root = internal_mono_dir.path_join("GodotSharp");
		} else if (DirAccess::exists(internal_mono_dir)) {
			data_dir_root = internal_mono_dir;
		} else if (DirAccess::exists("/storage/emulated/0/mono/GodotSharp")) {
			data_dir_root = "/storage/emulated/0/mono/GodotSharp";
		} else if (DirAccess::exists("/storage/emulated/0/mono")) {
			data_dir_root = "/storage/emulated/0/mono";
		}
#endif

		data_editor_tools_dir = data_dir_root.path_join("Tools");
		String api_assemblies_base_dir = data_dir_root.path_join("Api");
		build_logs_dir = mono_user_dir.path_join("build_logs");

#ifdef MACOS_ENABLED
		if (!DirAccess::exists(data_editor_tools_dir)) {
			data_editor_tools_dir = res_dir.path_join("GodotSharp").path_join("Tools");
		}
		if (!DirAccess::exists(api_assemblies_base_dir)) {
			api_assemblies_base_dir = res_dir.path_join("GodotSharp").path_join("Api");
		}
#endif

#if defined(ANDROID_ENABLED)
		// Suporta sa flexible folder structures sa Android
		if (!DirAccess::exists(data_editor_tools_dir)) {
			if (DirAccess::exists(internal_mono_dir.path_join("Tools"))) {
				data_editor_tools_dir = internal_mono_dir.path_join("Tools");
			} else if (DirAccess::exists("/storage/emulated/0/mono/Tools")) {
				data_editor_tools_dir = "/storage/emulated/0/mono/Tools";
			}
		}

		if (!DirAccess::exists(api_assemblies_base_dir)) {
			if (DirAccess::exists(internal_mono_dir.path_join("Api"))) {
				api_assemblies_base_dir = internal_mono_dir.path_join("Api");
			} else if (DirAccess::exists("/storage/emulated/0/mono/Api")) {
				api_assemblies_base_dir = "/storage/emulated/0/mono/Api";
			}
		}

		if (DirAccess::exists(api_assemblies_base_dir.path_join(GDMono::get_expected_api_build_config()))) {
			api_assemblies_dir = api_assemblies_base_dir.path_join(GDMono::get_expected_api_build_config());
		} else if (DirAccess::exists(api_assemblies_base_dir)) {
			api_assemblies_dir = api_assemblies_base_dir;
		} else if (DirAccess::exists(internal_mono_dir.path_join("assemblies"))) {
			api_assemblies_dir = internal_mono_dir.path_join("assemblies");
		} else if (DirAccess::exists(internal_mono_dir)) {
			api_assemblies_dir = internal_mono_dir;
		} else if (DirAccess::exists("/storage/emulated/0/mono/assemblies")) {
			api_assemblies_dir = "/storage/emulated/0/mono/assemblies";
		} else if (DirAccess::exists("/storage/emulated/0/mono")) {
			api_assemblies_dir = "/storage/emulated/0/mono";
		} else {
			api_assemblies_dir = api_assemblies_base_dir.path_join(GDMono::get_expected_api_build_config());
		}
#else
		api_assemblies_dir = api_assemblies_base_dir.path_join(GDMono::get_expected_api_build_config());
#endif

#else // !TOOLS_ENABLED
		String platform = _get_platform_name();
		String arch = Engine::get_singleton()->get_architecture_name();
		String appname_safe = Path::get_csharp_project_name();
		String packed_path = "res://.godot/mono/publish/" + arch;

#ifdef ANDROID_ENABLED
		String internal_mono_dir = OS::get_singleton()->get_user_data_dir().path_join("mono");
		if (DirAccess::exists(internal_mono_dir.path_join("assemblies"))) {
			api_assemblies_dir = internal_mono_dir.path_join("assemblies");
		} else if (DirAccess::exists(internal_mono_dir)) {
			api_assemblies_dir = internal_mono_dir;
		} else if (DirAccess::exists("/storage/emulated/0/mono/assemblies")) {
			api_assemblies_dir = "/storage/emulated/0/mono/assemblies";
		} else if (DirAccess::exists("/storage/emulated/0/mono")) {
			api_assemblies_dir = "/storage/emulated/0/mono";
		} else {
			api_assemblies_dir = packed_path;
		}
		print_verbose(".NET: Android platform detected. Setting api_assemblies_dir to: " + api_assemblies_dir);
#else
		if (DirAccess::exists(packed_path)) {
			String data_dir_root = OS::get_singleton()->get_cache_path().path_join("data_" + appname_safe + "_" + platform + "_" + arch);
			bool has_data = false;
			if (!has_data) {
				String global_packed = ProjectSettings::get_singleton()->globalize_path(packed_path);
				if (global_packed.is_absolute_path() && FileAccess::exists(global_packed.path_join(".dotnet-publish-manifest"))) {
					data_dir_root = global_packed;
					has_data = true;
				}
			}
			if (!has_data) {
				String packed_manifest = packed_path.path_join(".dotnet-publish-manifest");
				String extracted_manifest = data_dir_root.path_join(".dotnet-publish-manifest");
				if (FileAccess::exists(packed_manifest) && FileAccess::exists(extracted_manifest)) {
					if (FileAccess::get_file_as_bytes(packed_manifest) == FileAccess::get_file_as_bytes(extracted_manifest)) {
						has_data = true;
					}
				}
			}
			if (!has_data) {
				Ref<DirAccess> da;
				if (DirAccess::exists(data_dir_root)) {
					da = DirAccess::open(data_dir_root);
					ERR_FAIL_COND(da.is_null());
					ERR_FAIL_COND(da->erase_contents_recursive() != OK);
				}
				da = DirAccess::create_for_path(packed_path);
				ERR_FAIL_COND(da.is_null());
				ERR_FAIL_COND(da->copy_dir(packed_path, data_dir_root) != OK);
			}
			api_assemblies_dir = data_dir_root;
		} else {
			String data_dir_root = exe_dir.path_join("data_" + appname_safe + "_" + platform + "_" + arch);
#ifdef MACOS_ENABLED
			if (!DirAccess::exists(data_dir_root)) {
				data_dir_root = res_dir.path_join("data_" + appname_safe + "_" + platform + "_" + arch);
			}
#endif
			api_assemblies_dir = data_dir_root;
		}
#endif // ANDROID_ENABLED
#endif // TOOLS_ENABLED
	}

public:
	static _GodotSharpDirs &get_singleton() {
		static _GodotSharpDirs singleton;
		return singleton;
	}
};

String get_res_metadata_dir() {
	return _GodotSharpDirs::get_singleton().res_metadata_dir;
}

String get_res_temp_assemblies_dir() {
	return _GodotSharpDirs::get_singleton().res_temp_assemblies_dir;
}

String get_api_assemblies_dir() {
	return _GodotSharpDirs::get_singleton().api_assemblies_dir;
}

String get_mono_user_dir() {
	return _GodotSharpDirs::get_singleton().mono_user_dir;
}

#ifdef TOOLS_ENABLED
String get_build_logs_dir() {
	return _GodotSharpDirs::get_singleton().build_logs_dir;
}

String get_data_editor_tools_dir() {
	return _GodotSharpDirs::get_singleton().data_editor_tools_dir;
}
#endif

} // namespace GodotSharpDirs
