/**************************************************************************/
/*  hostfxr_resolver.cpp                                                  */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#include "hostfxr_resolver.h"

#include "../utils/path_utils.h"
#include "semver.h"

#include "core/config/engine.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/os/os.h"

#ifdef WINDOWS_ENABLED
#include <windows.h>
#endif

namespace {

String get_hostfxr_file_name() {
#if defined(WINDOWS_ENABLED)
	return "hostfxr.dll";
#elif defined(MACOS_ENABLED) || defined(APPLE_EMBEDDED_ENABLED)
	return "libhostfxr.dylib";
#else
	return "libhostfxr.so";
#endif
}

#if defined(ANDROID_ENABLED)
// Helper: Kinokopya ang libhostfxr.so mula external storage papuntang internal storage para ligtas i-dlopen
String prepare_android_fxr_lib(const String &p_src_path) {
	if (!FileAccess::exists(p_src_path)) {
		return String();
	}
	String internal_dir = OS::get_singleton()->get_user_data_dir().path_join("mono_libs");
	DirAccess::make_dir_recursive_absolute(internal_dir);
	String internal_path = internal_dir.path_join(get_hostfxr_file_name());

	if (!FileAccess::exists(internal_path) || FileAccess::get_modified_time(p_src_path) > FileAccess::get_modified_time(internal_path)) {
		Vector<uint8_t> data = FileAccess::get_file_as_bytes(p_src_path);
		if (!data.is_empty()) {
			Ref<FileAccess> dst = FileAccess::open(internal_path, FileAccess::WRITE);
			if (dst.is_valid()) {
				dst->store_buffer(data.ptr(), data.size());
				dst->close();
				print_verbose(".NET: Copied hostfxr to internal storage: " + internal_path);
			}
		}
	}
	return FileAccess::exists(internal_path) ? internal_path : String();
}
#endif

bool get_latest_fxr(const String &fxr_root, String &r_fxr_path) {
	godotsharp::SemVerParser sem_ver_parser;

	bool found_ver = false;
	godotsharp::SemVer latest_ver;
	String latest_ver_str;

	Ref<DirAccess> da = DirAccess::open(fxr_root);
	if (da.is_null()) {
		return false;
	}

	da->list_dir_begin();
	for (String dir = da->get_next(); !dir.is_empty(); dir = da->get_next()) {
		if (!da->current_is_dir() || dir == "." || dir == "..") {
			continue;
		}

		String ver = dir.get_file();

		godotsharp::SemVer fx_ver;
		if (sem_ver_parser.parse(ver, fx_ver)) {
			if (!found_ver || fx_ver > latest_ver) {
				latest_ver = fx_ver;
				latest_ver_str = ver;
				found_ver = true;
			}
		}
	}

	if (!found_ver) {
		return false;
	}

	String fxr_with_ver = Path::join(fxr_root, latest_ver_str);
	String hostfxr_file_path = Path::join(fxr_with_ver, get_hostfxr_file_name());

	if (!FileAccess::exists(hostfxr_file_path)) {
		return false;
	}

	r_fxr_path = hostfxr_file_path;
	return true;
}

#ifdef WINDOWS_ENABLED
BOOL is_wow64() {
	BOOL wow64 = FALSE;
	if (!IsWow64Process(GetCurrentProcess(), &wow64)) {
		wow64 = FALSE;
	}
	return wow64;
}
#endif

static const char *arch_name_map[][2] = {
	{ "arm32", "arm" },
	{ "arm64", "arm64" },
	{ "rv64", "riscv64" },
	{ "x86_64", "x64" },
	{ "x86_32", "x86" },
	{ nullptr, nullptr }
};

String get_dotnet_arch() {
	String arch = Engine::get_singleton()->get_architecture_name();

	int idx = 0;
	while (arch_name_map[idx][0] != nullptr) {
		if (arch_name_map[idx][0] == arch) {
			return arch_name_map[idx][1];
		}
		idx++;
	}

	return "";
}

bool get_default_installation_dir(String &r_dotnet_root) {
#if defined(WINDOWS_ENABLED)
	String program_files_env;
	if (is_wow64()) {
		program_files_env = "ProgramFiles(x86)";
	} else {
		program_files_env = "ProgramFiles";
	}

	String program_files_dir = OS::get_singleton()->get_environment(program_files_env);
	if (program_files_dir.is_empty()) {
		return false;
	}

#if defined(__x86_64) || defined(__x86_64__) || defined(__amd64__) || defined(_M_X64)
	String dotnet_root_emulated = Path::join(program_files_dir, "dotnet", "x64");
	if (FileAccess::exists(Path::join(dotnet_root_emulated, "dotnet.exe"))) {
		r_dotnet_root = dotnet_root_emulated;
		return true;
	}
#endif

	r_dotnet_root = Path::join(program_files_dir, "dotnet");
	return true;
#elif defined(MACOS_ENABLED)
	r_dotnet_root = "/usr/local/share/dotnet";

#if defined(__x86_64) || defined(__x86_64__) || defined(__amd64__) || defined(_M_X64)
	String dotnet_root_emulated = Path::join(r_dotnet_root, "x64");
	if (FileAccess::exists(Path::join(dotnet_root_emulated, "dotnet"))) {
		r_dotnet_root = dotnet_root_emulated;
		return true;
	}
#endif
	return true;
#elif defined(ANDROID_ENABLED)
	// Safe Android check: Mag-return lamang ng true kung talagang umiiral ang directory
	String internal_dir = OS::get_singleton()->get_user_data_dir().path_join("mono");
	if (DirAccess::exists(internal_dir)) {
		r_dotnet_root = internal_dir;
		return true;
	}
	if (DirAccess::exists("/storage/emulated/0/mono")) {
		r_dotnet_root = "/storage/emulated/0/mono";
		return true;
	} else if (DirAccess::exists("/sdcard/mono")) {
		r_dotnet_root = "/sdcard/mono";
		return true;
	}
	return false;
#else
	r_dotnet_root = "/usr/share/dotnet";
	return true;
#endif
}

#ifndef WINDOWS_ENABLED
bool get_install_location_from_file(const String &p_file_path, String &r_dotnet_root) {
	Error err = OK;
	Ref<FileAccess> f = FileAccess::open(p_file_path, FileAccess::READ, &err);
	if (f.is_null() || err != OK) {
		return false;
	}

	String line = f->get_line();
	if (line.is_empty()) {
		return false;
	}

	r_dotnet_root = line;
	return true;
}
#endif

bool get_dotnet_self_registered_dir(String &r_dotnet_root) {
#if defined(WINDOWS_ENABLED)
	String sub_key = "SOFTWARE\\dotnet\\Setup\\InstalledVersions\\" + get_dotnet_arch();
	Char16String value = String("InstallLocation").utf16();

	HKEY hkey = nullptr;
	LSTATUS result = RegOpenKeyExW(HKEY_LOCAL_MACHINE, (LPCWSTR)(sub_key.utf16().get_data()), 0, KEY_READ | KEY_WOW64_32KEY, &hkey);
	if (result != ERROR_SUCCESS) {
		return false;
	}

	DWORD size = 0;
	result = RegGetValueW(hkey, nullptr, (LPCWSTR)(value.get_data()), RRF_RT_REG_SZ, nullptr, nullptr, &size);
	if (result != ERROR_SUCCESS || size == 0) {
		RegCloseKey(hkey);
		return false;
	}

	Vector<WCHAR> buffer;
	buffer.resize(size / sizeof(WCHAR));
	result = RegGetValueW(hkey, nullptr, (LPCWSTR)(value.get_data()), RRF_RT_REG_SZ, nullptr, (LPBYTE)buffer.ptrw(), &size);
	if (result != ERROR_SUCCESS) {
		RegCloseKey(hkey);
		return false;
	}

	r_dotnet_root = String::utf16((const char16_t *)buffer.ptr()).replace_char('\\', '/');
	RegCloseKey(hkey);
	return true;
#else
	String install_location_file = Path::join("/etc/dotnet", "install_location_" + get_dotnet_arch().to_lower());
	if (get_install_location_from_file(install_location_file, r_dotnet_root)) {
		return true;
	}

	if (FileAccess::exists(install_location_file)) {
		return false;
	}

	String legacy_install_location_file = Path::join("/etc/dotnet", "install_location");
	return get_install_location_from_file(legacy_install_location_file, r_dotnet_root);
#endif
}

bool get_file_path_from_env(const String &p_env_key, String &r_dotnet_root) {
	String env_value = OS::get_singleton()->get_environment(p_env_key);
	if (!env_value.is_empty()) {
		env_value = Path::realpath(env_value);
		if (DirAccess::exists(env_value)) {
			r_dotnet_root = env_value;
			return true;
		}
	}
	return false;
}

bool get_dotnet_root_from_env(String &r_dotnet_root) {
	String dotnet_root_env = "DOTNET_ROOT";
	String arch_for_env = get_dotnet_arch();

	if (!arch_for_env.is_empty()) {
		if (get_file_path_from_env(dotnet_root_env + "_" + arch_for_env.to_upper(), r_dotnet_root)) {
			return true;
		}
	}

#ifdef WINDOWS_ENABLED
	if (is_wow64() && get_file_path_from_env("DOTNET_ROOT(x86)", r_dotnet_root)) {
		return true;
	}
#endif

	return get_file_path_from_env(dotnet_root_env, r_dotnet_root);
}

} // namespace

bool godotsharp::hostfxr_resolver::try_get_path_from_dotnet_root(const String &p_dotnet_root, String &r_fxr_path) {
	if (p_dotnet_root.is_empty() || !DirAccess::exists(p_dotnet_root)) {
		return false;
	}

	// 1. Tignan kung direktang nasa loob ng dotnet root ang hostfxr library
	String direct_fxr = Path::join(p_dotnet_root, get_hostfxr_file_name());
	if (FileAccess::exists(direct_fxr)) {
		r_fxr_path = direct_fxr;
		return true;
	}

	// 2. Standard .NET layout: <dotnet_root>/host/fxr/<version>/libhostfxr.so
	String fxr_dir = Path::join(p_dotnet_root, "host", "fxr");
	if (!DirAccess::exists(fxr_dir)) {
		return false;
	}
	return get_latest_fxr(fxr_dir, r_fxr_path);
}

bool godotsharp::hostfxr_resolver::try_get_path(String &r_dotnet_root, String &r_fxr_path) {
#if defined(ANDROID_ENABLED)
	// 1. Unahin i-check kung mayroon nang handang libhostfxr.so sa internal storage
	String internal_fxr = OS::get_singleton()->get_user_data_dir().path_join("mono_libs").path_join(get_hostfxr_file_name());
	if (FileAccess::exists(internal_fxr)) {
		r_dotnet_root = OS::get_singleton()->get_user_data_dir().path_join("mono");
		r_fxr_path = internal_fxr;
		return true;
	}

	// 2. I-check ang external storage paths at ligtas na kopyahin bago ibalik ang path
	static const char *android_search_dirs[] = {
		"/storage/emulated/0/mono",
		"/sdcard/mono",
		nullptr
	};

	for (int i = 0; android_search_dirs[i] != nullptr; i++) {
		String search_dir = android_search_dirs[i];
		if (DirAccess::exists(search_dir)) {
			if (try_get_path_from_dotnet_root(search_dir, r_fxr_path)) {
				r_dotnet_root = search_dir;
				String safe_path = prepare_android_fxr_lib(r_fxr_path);
				if (!safe_path.is_empty()) {
					r_fxr_path = safe_path;
					return true;
				}
			}
		}
	}
#endif

	if (!get_dotnet_root_from_env(r_dotnet_root) &&
			!get_dotnet_self_registered_dir(r_dotnet_root) &&
			!get_default_installation_dir(r_dotnet_root)) {
		return false;
	}

	bool found = try_get_path_from_dotnet_root(r_dotnet_root, r_fxr_path);
#if defined(ANDROID_ENABLED)
	if (found) {
		String safe_path = prepare_android_fxr_lib(r_fxr_path);
		if (!safe_path.is_empty()) {
			r_fxr_path = safe_path;
		}
	}
#endif
	return found;
}
