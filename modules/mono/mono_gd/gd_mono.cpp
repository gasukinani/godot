/**************************************************************************/
/*  gd_mono.cpp                                                           */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#include "gd_mono.h"

#include "../glue/runtime_interop.h"
#include "../godotsharp_dirs.h"
#include "../thirdparty/coreclr_delegates.h"
#include "../thirdparty/hostfxr.h"
#include "../utils/path_utils.h"
#include "gd_mono_cache.h"

#ifdef DEBUG_ENABLED
#include "core/object/class_db.h"
#endif

#ifdef TOOLS_ENABLED
#include "../editor/hostfxr_resolver.h"
#include "../editor/semver.h"
#include "editor/plugins/editor_plugin.h"
#endif

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/os/os.h"
#include "core/os/thread.h"
#include "core/templates/hash_set.h"

#ifdef UNIX_ENABLED
#include <arpa/inet.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifdef ANDROID_ENABLED
#include "../thirdparty/mono_delegates.h"
#endif

GDMono *GDMono::singleton = nullptr;

namespace {

// Nanatiling persistent sa buong buhay ng Android OS process
static void *coreclr_dll_handle = nullptr;
static void *hostfxr_lib_handle = nullptr;
static void *hostpolicy_lib_handle = nullptr;
static void *active_coreclr_handle = nullptr;
static unsigned int active_domain_id = 0;

static bool s_runtime_bootstrapped = false;
static bool s_plugins_initialized = false;
static bool s_has_crypto_support = false;

#ifdef TOOLS_ENABLED
static gdmono::PluginCallbacks s_cached_plugin_callbacks{};
#endif
static GDMonoCache::ManagedCallbacks s_cached_managed_callbacks{};

typedef int (*roslyn_compile_project_fn)(const char *project_dir, const char *output_dll, char *error_buf, int error_buf_size);
static roslyn_compile_project_fn roslyn_compile_fn = nullptr;

// Helper function para sa tamang pagkuha ng file size sa Godot 4
uint64_t get_file_size(const String &p_path) {
	Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::READ);
	if (f.is_valid()) {
		return f->get_length();
	}
	return 0;
}

void write_mono_log(const String &p_msg) {
	print_line(p_msg);
#if defined(ANDROID_ENABLED)
	String log_path = "/storage/emulated/0/mono/mono_log.txt";
	Ref<FileAccess> f = FileAccess::open(log_path, FileAccess::READ_WRITE);
	if (!f.is_valid()) {
		f = FileAccess::open(log_path, FileAccess::WRITE);
	}
	if (f.is_valid()) {
		f->seek_end();
		f->store_line(p_msg);
		f->flush();
	}
#endif
}

#ifdef _WIN32
static_assert(sizeof(char_t) == sizeof(char16_t));
using HostFxrCharString = Char16String;
#define HOSTFXR_STR(m_str) L##m_str
#else
static_assert(sizeof(char_t) == sizeof(char));
using HostFxrCharString = CharString;
#define HOSTFXR_STR(m_str) m_str
#endif

HostFxrCharString str_to_hostfxr(const String &p_str) {
#ifdef _WIN32
	return p_str.utf16();
#else
	return p_str.utf8();
#endif
}

const char_t *get_data(const HostFxrCharString &p_char_str) {
	return (const char_t *)p_char_str.get_data();
}

void HOSTFXR_CALLTYPE hostfxr_error_callback(const char_t *p_message) {
#ifdef _WIN32
	write_mono_log(String(".NET [HostFxr Message]: ") + String::utf16((const char16_t *)p_message));
#else
	write_mono_log(String(".NET [HostFxr Message]: ") + String::utf8((const char *)p_message));
#endif
}

String get_csharp_project_name() {
	String assembly_name = GLOBAL_GET("dotnet/project/assembly_name");
	if (assembly_name.is_empty()) {
		assembly_name = GLOBAL_GET("application/config/name");
	}
	if (assembly_name.is_empty()) {
		assembly_name = "Game";
	}
	return assembly_name;
}

#ifdef TOOLS_ENABLED
void ensure_csharp_project_files_exist() {
	if (Engine::get_singleton()->is_project_manager_hint()) {
		return;
	}

	String project_name = get_csharp_project_name();
	String project_dir = ProjectSettings::get_singleton()->globalize_path("res://");
	if (project_dir.is_empty() || project_dir == "/") {
		return;
	}

	String csproj_path = project_dir.path_join(project_name + ".csproj");
	String sln_path = project_dir.path_join(project_name + ".sln");

	if (!FileAccess::exists(csproj_path)) {
		Ref<FileAccess> f = FileAccess::open(csproj_path, FileAccess::WRITE);
		if (f.is_valid()) {
			String csproj_content =
					"<Project Sdk=\"Godot.NET.Sdk/4.3.0\">\n"
					"  <PropertyGroup>\n"
					"    <TargetFramework>net8.0</TargetFramework>\n"
					"    <EnableDynamicLoading>true</EnableDynamicLoading>\n"
					"  </PropertyGroup>\n"
					"</Project>\n";
			f->store_string(csproj_content);
			f->close();
			write_mono_log(".NET: Created auto-generated project file: " + csproj_path);
		}
	}

	if (!FileAccess::exists(sln_path)) {
		Ref<FileAccess> f_sln = FileAccess::open(sln_path, FileAccess::WRITE);
		if (f_sln.is_valid()) {
			String sln_content =
					"Microsoft Visual Studio Solution File, Format Version 12.00\n"
					"# Visual Studio Version 17\n"
					"VisualStudioVersion = 17.0.31903.59\n"
					"MinimumVisualStudioVersion = 10.0.40219.1\n"
					"Project(\"{FAE04EC0-301F-11D3-BF4B-00C04F79EFBC}\") = \"" + project_name + "\", \"" + project_name + ".csproj\", \"{28A25368-2458-4B52-BF01-1C2EE5DC22FF}\"\n"
					"EndProject\n"
					"Global\n"
					"	GlobalSection(SolutionConfigurationPlatforms) = preSolution\n"
					"		Debug|Any CPU = Debug|Any CPU\n"
					"		ExportDebug|Any CPU = ExportDebug|Any CPU\n"
					"		ExportRelease|Any CPU = ExportRelease|Any CPU\n"
					"	EndGlobalSection\n"
					"	GlobalSection(ProjectConfigurationPlatforms) = postSolution\n"
					"		{28A25368-2458-4B52-BF01-1C2EE5DC22FF}.Debug|Any CPU.ActiveCfg = Debug|Any CPU\n"
					"		{28A25368-2458-4B52-BF01-1C2EE5DC22FF}.Debug|Any CPU.Build.0 = Debug|Any CPU\n"
					"		{28A25368-2458-4B52-BF01-1C2EE5DC22FF}.ExportDebug|Any CPU.ActiveCfg = ExportDebug|Any CPU\n"
					"		{28A25368-2458-4B52-BF01-1C2EE5DC22FF}.ExportDebug|Any CPU.Build.0 = ExportDebug|Any CPU\n"
					"		{28A25368-2458-4B52-BF01-1C2EE5DC22FF}.ExportRelease|Any CPU.ActiveCfg = ExportRelease|Any CPU\n"
					"		{28A25368-2458-4B52-BF01-1C2EE5DC22FF}.ExportRelease|Any CPU.Build.0 = ExportRelease|Any CPU\n"
					"	EndGlobalSection\n"
					"EndGlobal\n";
			f_sln->store_string(sln_content);
			f_sln->close();
			write_mono_log(".NET: Created auto-generated solution file: " + sln_path);
		}
	}

	String bin_dir = project_dir.path_join(".godot/mono/temp/bin/Debug");
	DirAccess::make_dir_recursive_absolute(bin_dir);

	String tools_dir = project_dir.path_join("GodotSharp/Tools");
	DirAccess::make_dir_recursive_absolute(tools_dir);

	String mono_src_dir = "/storage/emulated/0/mono/assemblies";
	if (!DirAccess::exists(mono_src_dir)) {
		mono_src_dir = "/storage/emulated/0/mono";
	}

	Ref<DirAccess> da = DirAccess::open(mono_src_dir);
	if (da.is_valid()) {
		da->list_dir_begin();
		for (String file = da->get_next(); !file.is_empty(); file = da->get_next()) {
			if (!da->current_is_dir() && file.ends_with(".dll")) {
				String dst_bin = bin_dir.path_join(file);
				if (!FileAccess::exists(dst_bin)) {
					DirAccess::copy_absolute(mono_src_dir.path_join(file), dst_bin);
				}

				if (file.begins_with("GodotTools") || file.begins_with("GodotSharp") || file.begins_with("GodotPlugins")) {
					String dst_tools = tools_dir.path_join(file);
					if (!FileAccess::exists(dst_tools)) {
						DirAccess::copy_absolute(mono_src_dir.path_join(file), dst_tools);
					}
				}
			}
		}
		write_mono_log(".NET: Synced GodotSharp & GodotTools assemblies to project.");
	}
}

uint64_t get_latest_cs_modified_time(const String &p_dir) {
	uint64_t latest = 0;
	Ref<DirAccess> da = DirAccess::open(p_dir);
	if (da.is_valid()) {
		da->list_dir_begin();
		for (String file = da->get_next(); !file.is_empty(); file = da->get_next()) {
			if (file == "." || file == ".." || file == ".godot") {
				continue;
			}
			String full_path = p_dir.path_join(file);
			if (da->current_is_dir()) {
				uint64_t sub_latest = get_latest_cs_modified_time(full_path);
				if (sub_latest > latest) {
					latest = sub_latest;
				}
			} else if (file.ends_with(".cs")) {
				uint64_t mod_time = FileAccess::get_modified_time(full_path);
				if (mod_time > latest) {
					latest = mod_time;
				}
			}
		}
	}
	return latest;
}
#endif

hostfxr_initialize_for_runtime_config_fn hostfxr_initialize_for_runtime_config = nullptr;
hostfxr_get_runtime_delegate_fn hostfxr_get_runtime_delegate = nullptr;
hostfxr_close_fn hostfxr_close = nullptr;
hostfxr_set_error_writer_fn hostfxr_set_error_writer = nullptr;

typedef int(CORECLR_DELEGATE_CALLTYPE *coreclr_create_delegate_fn)(void *hostHandle, unsigned int domainId, const char *entryPointAssemblyName, const char *entryPointTypeName, const char *entryPointMethodName, void **delegate);
typedef int(CORECLR_DELEGATE_CALLTYPE *coreclr_initialize_fn)(const char *exePath, const char *appDomainFriendlyName, int propertyCount, const char **propertyKeys, const char **propertyValues, void **hostHandle, unsigned int *domainId);

coreclr_create_delegate_fn coreclr_create_delegate = nullptr;
coreclr_initialize_fn coreclr_initialize = nullptr;

#ifdef ANDROID_ENABLED
mono_install_assembly_preload_hook_fn mono_install_assembly_preload_hook = nullptr;
mono_assembly_name_get_name_fn mono_assembly_name_get_name = nullptr;
mono_assembly_name_get_culture_fn mono_assembly_name_get_culture = nullptr;
mono_image_open_from_data_with_name_fn mono_image_open_from_data_with_name = nullptr;
mono_assembly_load_from_full_fn mono_assembly_load_from_full = nullptr;

String prepare_android_executable_lib(const String &p_filename) {
	String ext_path = String("/storage/emulated/0/mono").path_join(p_filename);
	if (!FileAccess::exists(ext_path)) {
		return p_filename;
	}

	String internal_dir = OS::get_singleton()->get_user_data_dir().path_join("mono_libs");
	DirAccess::make_dir_recursive_absolute(internal_dir);
	String internal_path = internal_dir.path_join(p_filename);

	if (!FileAccess::exists(internal_path) || FileAccess::get_modified_time(ext_path) > FileAccess::get_modified_time(internal_path)) {
		Vector<uint8_t> data = FileAccess::get_file_as_bytes(ext_path);
		if (!data.is_empty()) {
			Ref<FileAccess> dst = FileAccess::open(internal_path, FileAccess::WRITE);
			if (dst.is_valid()) {
				dst->store_buffer(data.ptr(), data.size());
				dst->close();
				write_mono_log(".NET: Copied " + p_filename + " to internal storage.");
			}
		}
	}

	return FileAccess::exists(internal_path) ? internal_path : p_filename;
}

void preload_android_crypto_libs() {
	Vector<String> search_dirs;
	search_dirs.push_back(OS::get_singleton()->get_user_data_dir().path_join("mono_libs"));
	search_dirs.push_back("/storage/emulated/0/mono");
	search_dirs.push_back("/storage/emulated/0/mono/lib");
	search_dirs.push_back("/storage/emulated/0/mono/assemblies");

	const char *lib_names[] = {
		"libcrypto.so.3",
		"libcrypto.so.1.1",
		"libcrypto.so",
		"libssl.so.3",
		"libssl.so.1.1",
		"libssl.so",
		nullptr
	};

	s_has_crypto_support = false;

	for (int i = 0; lib_names[i] != nullptr; i++) {
		for (const String &dir : search_dirs) {
			String full_path = dir.path_join(lib_names[i]);
			if (FileAccess::exists(full_path)) {
				void *handle = dlopen(full_path.utf8().get_data(), RTLD_NOW | RTLD_GLOBAL);
				if (handle) {
					write_mono_log(".NET: Preloaded global crypto lib: " + full_path);
					s_has_crypto_support = true;
					break;
				}
			}
		}
	}

#ifdef UNIX_ENABLED
	if (!s_has_crypto_support) {
		void *test_sym = dlsym(RTLD_DEFAULT, "EVP_MD_CTX_new");
		if (!test_sym) {
			test_sym = dlsym(RTLD_DEFAULT, "EVP_MD_CTX_create");
		}
		if (test_sym) {
			s_has_crypto_support = true;
			write_mono_log(".NET: OpenSSL symbols resolved via global namespace.");
		}
	}
#endif

	if (!s_has_crypto_support) {
		write_mono_log(".NET: Notice - OpenSSL (libcrypto.so.3) not found. In-process Roslyn will be guarded to prevent SIGSEGV.");
	}
}

void sync_all_android_native_libs() {
	String ext_dir_path = "/storage/emulated/0/mono";
	String internal_dir = OS::get_singleton()->get_user_data_dir().path_join("mono_libs");
	DirAccess::make_dir_recursive_absolute(internal_dir);

	// Fallback symlink para sa libdl.so.2
	String libdl2_path = internal_dir.path_join("libdl.so.2");
	if (!FileAccess::exists(libdl2_path)) {
		const char *sys_libdl[] = {
			"/apex/com.android.runtime/lib64/bionic/libdl.so",
			"/system/lib64/libdl.so",
			"/system/lib/libdl.so",
			nullptr
		};
		for (int i = 0; sys_libdl[i] != nullptr; i++) {
			if (FileAccess::exists(sys_libdl[i])) {
#ifdef UNIX_ENABLED
				symlink(sys_libdl[i], libdl2_path.utf8().get_data());
				write_mono_log(String(".NET: Created fallback symlink: libdl.so.2 -> ") + sys_libdl[i]);
#endif
				break;
			}
		}
	}

	Ref<DirAccess> da = DirAccess::open(ext_dir_path);
	if (da.is_valid()) {
		da->list_dir_begin();
		for (String file = da->get_next(); !file.is_empty(); file = da->get_next()) {
			if (!da->current_is_dir() && file.ends_with(".so")) {
				prepare_android_executable_lib(file);
			}
		}
	}

	preload_android_crypto_libs();
}

void setup_android_dotnet_environment() {
	String mono_dir = "/storage/emulated/0/mono";

	String dotnet_bin = mono_dir.path_join("dotnet");
	if (!FileAccess::exists(dotnet_bin)) {
		Ref<FileAccess> f = FileAccess::open(dotnet_bin, FileAccess::WRITE);
		if (f.is_valid()) {
			f->store_string("#!/bin/sh\necho \"8.0.8\"\n");
			f->close();
#ifdef UNIX_ENABLED
			chmod(dotnet_bin.utf8().get_data(), 0777);
#endif
			write_mono_log(".NET: Created fallback dotnet launcher script.");
		}
	}

	String sdk_base = mono_dir.path_join("sdk");
	String target_sdk = sdk_base.path_join("8.0.8");
	if (!DirAccess::exists(target_sdk)) {
		Ref<DirAccess> da = DirAccess::open(sdk_base);
		if (da.is_valid()) {
			da->list_dir_begin();
			for (String dir = da->get_next(); !dir.is_empty(); dir = da->get_next()) {
				if (da->current_is_dir() && dir != "." && dir != ".." && dir.begins_with("8.0.")) {
					String existing_sdk = sdk_base.path_join(dir);
					write_mono_log(".NET: Linking SDK " + dir + " to 8.0.8...");
#ifdef UNIX_ENABLED
					symlink(existing_sdk.utf8().get_data(), target_sdk.utf8().get_data());
#endif
					break;
				}
			}
		}
	}

	String msbuild_sdks_path = target_sdk.path_join("Sdks");
	if (!DirAccess::exists(msbuild_sdks_path)) {
		msbuild_sdks_path = mono_dir.path_join("sdk/8.0.402/Sdks");
	}
	if (!DirAccess::exists(msbuild_sdks_path)) {
		msbuild_sdks_path = mono_dir.path_join("Sdks");
	}

	OS::get_singleton()->set_environment("DOTNET_ROOT", mono_dir);
	OS::get_singleton()->set_environment("DOTNET_HOST_PATH", dotnet_bin);
	OS::get_singleton()->set_environment("MSBuildSDKsPath", msbuild_sdks_path);
	OS::get_singleton()->set_environment("MSBUILD_EXE_PATH", mono_dir.path_join("assemblies/GodotTools.ProjectEditor.dll"));
	OS::get_singleton()->set_environment("MSBUILDUSESERVER", "0");
	OS::get_singleton()->set_environment("MSBUILDDISABLENODEREUSE", "1");
	OS::get_singleton()->set_environment("DOTNET_CLI_TELEMETRY_OPTOUT", "1");
	OS::get_singleton()->set_environment("DOTNET_MULTILEVEL_LOOKUP", "0");
	OS::get_singleton()->set_environment("DOTNET_GCHeapHardLimit", "1C0000000");
	OS::get_singleton()->set_environment("COREHOST_TRACE", "0");

	write_mono_log(".NET: Environment variables set. MSBuildSDKsPath: " + msbuild_sdks_path);
}
#endif

#if defined(ANDROID_ENABLED) && defined(TOOLS_ENABLED)
bool compile_csharp_project_via_termux_socket() {
	String project_name = get_csharp_project_name();
	String project_dir = ProjectSettings::get_singleton()->globalize_path("res://");
	String csproj_file = project_dir.path_join(project_name + ".csproj");

	write_mono_log(".NET: [Termux Build] Contacting background compiler daemon...");

	int sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock < 0) {
		write_mono_log(".NET: [Termux Build] Socket creation failed.");
		return false;
	}

	struct sockaddr_in serv_addr;
	memset(&serv_addr, 0, sizeof(serv_addr));
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_port = htons(8088);
	inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr);

	struct timeval tv;
	tv.tv_sec = 6;
	tv.tv_usec = 0;
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
	setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof(tv));

	if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
		write_mono_log(".NET: [Termux Build] Compiler daemon on port 8088 not reachable.");
		close(sock);
		return false;
	}

	String query_params = "?path=" + project_dir.uri_encode() + "&proj=" + csproj_file.uri_encode();
	String req = "GET /" + query_params + " HTTP/1.1\r\nHost: 127.0.0.1:8088\r\nConnection: close\r\n\r\n";

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
	send(sock, req.utf8().get_data(), req.utf8().length(), MSG_NOSIGNAL);

	char buffer[1024];
	String response;
	int bytes_read = 0;
	while ((bytes_read = recv(sock, buffer, sizeof(buffer) - 1, 0)) > 0) {
		buffer[bytes_read] = '\0';
		response += String::utf8(buffer);
	}
	close(sock);

	if (response.contains("200 OK") || response.contains("BUILD_SUCCESS")) {
		write_mono_log(".NET: [Termux Build] Background compilation SUCCEEDED!");
		return true;
	}

	write_mono_log(".NET: [Termux Build] Daemon returned build failure.");
	return false;
}

bool execute_hybrid_csharp_build() {
	String project_dir = ProjectSettings::get_singleton()->globalize_path("res://");
	uint64_t latest_cs_time = get_latest_cs_modified_time(project_dir);

	if (latest_cs_time == 0) {
		write_mono_log(".NET: No .cs source files found. Skipping compilation.");
		return true;
	}

	write_mono_log(">>>>> [AUTO-BUILD] Starting Hybrid C# Build Engine <<<<<");

	// Step 1: Subukan ang Termux background compiler (port 8088)
	if (compile_csharp_project_via_termux_socket()) {
		return true;
	}

	// Step 2: ROSLYN SIGSEGV GUARD
	if (!s_has_crypto_support) {
		write_mono_log(".NET: [Roslyn Guard] In-process compilation skipped: OpenSSL (libcrypto.so.3) is missing.");
		write_mono_log(".NET: [Action Required] Please start the Termux build daemon on port 8088, or copy libcrypto.so.3 to /storage/emulated/0/mono/");
		return false;
	}

	// Step 3: In-Process Roslyn Compiler
	String project_name = get_csharp_project_name();
	String bin_dir = project_dir.path_join(".godot/mono/temp/bin/Debug");
	DirAccess::make_dir_recursive_absolute(bin_dir);
	String output_dll = bin_dir.path_join(project_name + ".dll");

	if (roslyn_compile_fn != nullptr) {
		write_mono_log(".NET: [Roslyn Engine] Compiling in RAM via Microsoft.CodeAnalysis...");
		char err_buffer[2048] = { 0 };
		int res = roslyn_compile_fn(
				project_dir.utf8().get_data(),
				output_dll.utf8().get_data(),
				err_buffer,
				sizeof(err_buffer));

		if (res == 0) {
			write_mono_log(".NET: [Roslyn Engine] RAM BUILD SUCCEEDED! -> " + output_dll);
			return true;
		} else {
			write_mono_log(String(".NET: [Roslyn Notice] In-process compiler returned ") + itos(res) + ":\n" + String::utf8(err_buffer));
			// Burahin ang sirang output kung nag-fail ang compilation gamit ang get_file_size()
			if (FileAccess::exists(output_dll) && get_file_size(output_dll) < 1024) {
				DirAccess::remove_absolute(output_dll);
			}
		}
	} else {
		write_mono_log(".NET: [Roslyn Notice] In-Process Roslyn delegate not bound.");
	}

	return false;
}
#endif

String find_hostfxr() {
#if defined(ANDROID_ENABLED)
	return prepare_android_executable_lib("libhostfxr.so");
#else
	return String();
#endif
}

String find_coreclr() {
#if defined(ANDROID_ENABLED)
	prepare_android_executable_lib("libhostpolicy.so");
	String coreclr = prepare_android_executable_lib("libcoreclr.so");
	if (FileAccess::exists(coreclr)) {
		return coreclr;
	}
	return prepare_android_executable_lib("libmonosgen-2.0.so");
#else
	return String();
#endif
}

bool load_hostfxr(void *&r_hostfxr_dll_handle) {
	if (r_hostfxr_dll_handle != nullptr) {
		return true;
	}

#if defined(ANDROID_ENABLED)
	String hostpolicy_path = prepare_android_executable_lib("libhostpolicy.so");
	if (FileAccess::exists(hostpolicy_path) && !hostpolicy_lib_handle) {
		dlerror();
		hostpolicy_lib_handle = dlopen(hostpolicy_path.utf8().get_data(), RTLD_NOW | RTLD_GLOBAL);
		if (hostpolicy_lib_handle) {
			write_mono_log(".NET: Preloaded libhostpolicy.so successfully.");
		} else {
			const char *err = dlerror();
			write_mono_log(String(".NET: Notice - Preloading libhostpolicy.so failed: ") + (err ? err : "Unknown error"));
		}
	}
#endif

	String hostfxr_path = find_hostfxr();
	if (hostfxr_path.is_empty() || !FileAccess::exists(hostfxr_path)) {
		write_mono_log(".NET: libhostfxr.so does not exist on disk.");
		return false;
	}

	write_mono_log(".NET: Loading hostfxr library: " + hostfxr_path);

#if defined(UNIX_ENABLED)
	dlerror();
	r_hostfxr_dll_handle = dlopen(hostfxr_path.utf8().get_data(), RTLD_NOW | RTLD_GLOBAL);
	if (!r_hostfxr_dll_handle) {
		const char *err = dlerror();
		write_mono_log(String(".NET: CRITICAL - dlopen failed on libhostfxr.so: ") + (err ? err : "Unknown error"));
		return false;
	}
#else
	Error err = OS::get_singleton()->open_dynamic_library(hostfxr_path, r_hostfxr_dll_handle);
	if (err != OK) {
		write_mono_log(".NET: Failed to open hostfxr: " + hostfxr_path);
		return false;
	}
#endif

	void *lib = r_hostfxr_dll_handle;
	void *symbol = nullptr;

#if defined(UNIX_ENABLED)
	symbol = dlsym(lib, "hostfxr_initialize_for_runtime_config");
	hostfxr_initialize_for_runtime_config = (hostfxr_initialize_for_runtime_config_fn)symbol;

	symbol = dlsym(lib, "hostfxr_get_runtime_delegate");
	hostfxr_get_runtime_delegate = (hostfxr_get_runtime_delegate_fn)symbol;

	symbol = dlsym(lib, "hostfxr_close");
	hostfxr_close = (hostfxr_close_fn)symbol;

	symbol = dlsym(lib, "hostfxr_set_error_writer");
	if (symbol != nullptr) {
		hostfxr_set_error_writer = (hostfxr_set_error_writer_fn)symbol;
		hostfxr_set_error_writer(hostfxr_error_callback);
		write_mono_log(".NET: Bound and registered hostfxr_set_error_writer callback.");
	}
#else
	OS::get_singleton()->get_dynamic_library_symbol_handle(lib, "hostfxr_initialize_for_runtime_config", symbol);
	hostfxr_initialize_for_runtime_config = (hostfxr_initialize_for_runtime_config_fn)symbol;

	OS::get_singleton()->get_dynamic_library_symbol_handle(lib, "hostfxr_get_runtime_delegate", symbol);
	hostfxr_get_runtime_delegate = (hostfxr_get_runtime_delegate_fn)symbol;

	OS::get_singleton()->get_dynamic_library_symbol_handle(lib, "hostfxr_close", symbol);
	hostfxr_close = (hostfxr_close_fn)symbol;
#endif

	if (!hostfxr_initialize_for_runtime_config || !hostfxr_get_runtime_delegate || !hostfxr_close) {
		write_mono_log(".NET: One or more hostfxr entry point symbols were not found.");
		return false;
	}

	write_mono_log(".NET: libhostfxr.so symbols successfully resolved.");
	return true;
}

bool load_coreclr(void *&r_coreclr_dll_handle) {
	if (r_coreclr_dll_handle != nullptr) {
		return true;
	}

	String coreclr_path = find_coreclr();
	if (coreclr_path.is_empty() || !FileAccess::exists(coreclr_path)) {
		write_mono_log(".NET: Neither libcoreclr.so nor libmonosgen-2.0.so found.");
		return false;
	}

	write_mono_log(".NET: Loading runtime library: " + coreclr_path);

#if defined(UNIX_ENABLED)
	dlerror();
	r_coreclr_dll_handle = dlopen(coreclr_path.utf8().get_data(), RTLD_NOW | RTLD_GLOBAL);
	if (!r_coreclr_dll_handle) {
		const char *err = dlerror();
		write_mono_log(String(".NET: CRITICAL - dlopen failed on runtime: ") + (err ? err : "Unknown error"));
		return false;
	}
#else
	Error err = OS::get_singleton()->open_dynamic_library(coreclr_path, r_coreclr_dll_handle);
	if (err != OK) {
		write_mono_log(".NET: Failed to open dynamic library: " + coreclr_path);
		return false;
	}
#endif

	void *lib = r_coreclr_dll_handle;
	void *symbol = nullptr;

#if defined(UNIX_ENABLED)
	symbol = dlsym(lib, "coreclr_initialize");
	coreclr_initialize = (coreclr_initialize_fn)symbol;

	symbol = dlsym(lib, "coreclr_create_delegate");
	coreclr_create_delegate = (coreclr_create_delegate_fn)symbol;
#else
	OS::get_singleton()->get_dynamic_library_symbol_handle(lib, "coreclr_initialize", symbol);
	coreclr_initialize = (coreclr_initialize_fn)symbol;

	OS::get_singleton()->get_dynamic_library_symbol_handle(lib, "coreclr_create_delegate", symbol);
	coreclr_create_delegate = (coreclr_create_delegate_fn)symbol;
#endif

#ifdef ANDROID_ENABLED
	symbol = dlsym(lib, "mono_install_assembly_preload_hook");
	if (symbol) {
		mono_install_assembly_preload_hook = (mono_install_assembly_preload_hook_fn)symbol;
	}
	symbol = dlsym(lib, "mono_assembly_name_get_name");
	if (symbol) {
		mono_assembly_name_get_name = (mono_assembly_name_get_name_fn)symbol;
	}
	symbol = dlsym(lib, "mono_assembly_name_get_culture");
	if (symbol) {
		mono_assembly_name_get_culture = (mono_assembly_name_get_culture_fn)symbol;
	}
	symbol = dlsym(lib, "mono_image_open_from_data_with_name");
	if (symbol) {
		mono_image_open_from_data_with_name = (mono_image_open_from_data_with_name_fn)symbol;
	}
	symbol = dlsym(lib, "mono_assembly_load_from_full");
	if (symbol) {
		mono_assembly_load_from_full = (mono_assembly_load_from_full_fn)symbol;
	}
#endif

	write_mono_log(".NET: CoreCLR/Mono symbols bound successfully.");
	return (coreclr_initialize && coreclr_create_delegate);
}

#ifdef TOOLS_ENABLED
using godot_plugins_initialize_fn = bool (*)(void *, bool, gdmono::PluginCallbacks *, GDMonoCache::ManagedCallbacks *, const void **, int32_t);
#else
using godot_plugins_initialize_fn = bool (*)(void *, GDMonoCache::ManagedCallbacks *, const void **, int32_t);
#endif

static godot_plugins_initialize_fn cached_godot_plugins_initialize = nullptr;

void ensure_runtimeconfig_exists(const String &p_config_path) {
	if (FileAccess::exists(p_config_path)) {
		return;
	}

	Ref<FileAccess> f = FileAccess::open(p_config_path, FileAccess::WRITE);
	if (f.is_valid()) {
		String json_content =
				"{\n"
				"  \"runtimeOptions\": {\n"
				"    \"tfm\": \"net8.0\",\n"
				"    \"framework\": {\n"
				"      \"name\": \"Microsoft.NETCore.App\",\n"
				"      \"version\": \"8.0.0\"\n"
				"    },\n"
				"    \"configProperties\": {\n"
				"      \"System.Reflection.NullabilityInfoContext.IsSupported\": true,\n"
				"      \"System.Runtime.Serialization.EnableUnsafeBinaryFormatterSerialization\": false\n"
				"    }\n"
				"  }\n"
				"}\n";
		f->store_string(json_content);
		f->close();
		write_mono_log(".NET: Auto-generated missing runtimeconfig: " + p_config_path);
	}
}

godot_plugins_initialize_fn initialize_with_hostfxr(bool &r_runtime_initialized) {
	if (s_runtime_bootstrapped && cached_godot_plugins_initialize != nullptr) {
		write_mono_log(".NET: Reusing already initialized hostfxr runtime.");
		r_runtime_initialized = true;
		return cached_godot_plugins_initialize;
	}

	String config_path = "/storage/emulated/0/mono/GodotPlugins.runtimeconfig.json";
	if (!FileAccess::exists(config_path)) {
		config_path = "/storage/emulated/0/mono/assemblies/GodotPlugins.runtimeconfig.json";
	}
	ensure_runtimeconfig_exists(config_path);

	write_mono_log(".NET: Initializing via hostfxr with config: " + config_path);
	HostFxrCharString config_path_host = str_to_hostfxr(config_path);

	hostfxr_handle cxt = nullptr;
	int rc = hostfxr_initialize_for_runtime_config(get_data(config_path_host), nullptr, &cxt);
	if (rc != 0 || cxt == nullptr) {
		write_mono_log(vformat(".NET: hostfxr_initialize_for_runtime_config failed with code: 0x%X", (unsigned int)rc));
		if (cxt) {
			hostfxr_close(cxt);
		}
		return nullptr;
	}

	void *load_assembly_and_get_function_pointer = nullptr;
	rc = hostfxr_get_runtime_delegate(cxt, hdt_load_assembly_and_get_function_pointer, &load_assembly_and_get_function_pointer);
	hostfxr_close(cxt);

	if (rc != 0 || load_assembly_and_get_function_pointer == nullptr) {
		write_mono_log(vformat(".NET: hostfxr_get_runtime_delegate failed with code: 0x%X", (unsigned int)rc));
		return nullptr;
	}

	load_assembly_and_get_function_pointer_fn load_assembly_fn =
			(load_assembly_and_get_function_pointer_fn)load_assembly_and_get_function_pointer;

	String compiler_dll = "/storage/emulated/0/mono/assemblies/GodotAndroidCompiler.dll";
	if (FileAccess::exists(compiler_dll)) {
		load_assembly_fn(
				get_data(str_to_hostfxr(compiler_dll)),
				HOSTFXR_STR("GodotAndroidCompiler.InProcessCompiler, GodotAndroidCompiler"),
				HOSTFXR_STR("CompileProject"),
				UNMANAGEDCALLERSONLY_METHOD,
				nullptr,
				(void **)&roslyn_compile_fn);
		if (roslyn_compile_fn) {
			write_mono_log(".NET: Bound in-process Roslyn compiler via hostfxr!");
		}
	}

	String plugins_dll = "/storage/emulated/0/mono/assemblies/GodotPlugins.dll";
	if (!FileAccess::exists(plugins_dll)) {
		plugins_dll = "/storage/emulated/0/mono/GodotPlugins.dll";
	}

	write_mono_log(".NET: Loading GodotPlugins entry point from: " + plugins_dll);
	HostFxrCharString plugins_dll_host = str_to_hostfxr(plugins_dll);

	godot_plugins_initialize_fn godot_plugins_initialize = nullptr;

#ifdef TOOLS_ENABLED
	rc = load_assembly_fn(
			get_data(plugins_dll_host),
			HOSTFXR_STR("GodotPlugins.Main, GodotPlugins"),
			HOSTFXR_STR("InitializeFromEngine"),
			UNMANAGEDCALLERSONLY_METHOD,
			nullptr,
			(void **)&godot_plugins_initialize);
#else
	String assembly_name = get_csharp_project_name();
	String type_str = "GodotPlugins.Game.Main, " + assembly_name;
	HostFxrCharString type_host = str_to_hostfxr(type_str);
	rc = load_assembly_fn(
			get_data(plugins_dll_host),
			get_data(type_host),
			HOSTFXR_STR("InitializeFromGameProject"),
			UNMANAGEDCALLERSONLY_METHOD,
			nullptr,
			(void **)&godot_plugins_initialize);
#endif

	if (rc != 0 || godot_plugins_initialize == nullptr) {
		write_mono_log(vformat(".NET: load_assembly_and_get_function_pointer failed with code: 0x%X", (unsigned int)rc));
		return nullptr;
	}

	cached_godot_plugins_initialize = godot_plugins_initialize;
	s_runtime_bootstrapped = true;
	r_runtime_initialized = true;
	write_mono_log(".NET: hostfxr initialized successfully. Hostpolicy active.");
	return godot_plugins_initialize;
}

#ifdef ANDROID_ENABLED
MonoAssembly *load_assembly_from_pck(MonoAssemblyName *p_assembly_name, char **p_assemblies_path, void *p_user_data) {
	constexpr bool ref_only = false;
	const char *name = mono_assembly_name_get_name(p_assembly_name);
	const char *culture = mono_assembly_name_get_culture(p_assembly_name);

	String assembly_name;
	if (culture && strcmp(culture, "")) {
		assembly_name += culture;
		assembly_name += "/";
	}
	assembly_name += name;
	if (!assembly_name.ends_with(".dll")) {
		assembly_name += ".dll";
	}

	String path = String("/storage/emulated/0/mono/assemblies").path_join(assembly_name);
	if (!FileAccess::exists(path)) {
		path = String("/storage/emulated/0/mono").path_join(assembly_name);
	}

	if (!FileAccess::exists(path)) {
		return nullptr;
	}

	Vector<uint8_t> data = FileAccess::get_file_as_bytes(path);
	if (data.is_empty()) {
		return nullptr;
	}

	MonoImageOpenStatus status = MONO_IMAGE_OK;
	MonoImage *image = mono_image_open_from_data_with_name(
			reinterpret_cast<char *>(data.ptrw()), data.size(),
			/*need_copy*/ true, &status, ref_only, assembly_name.utf8().get_data());
	if (status != MONO_IMAGE_OK || image == nullptr) {
		return nullptr;
	}

	status = MONO_IMAGE_OK;
	return mono_assembly_load_from_full(image, assembly_name.utf8().get_data(), &status, ref_only);
}
#endif

godot_plugins_initialize_fn initialize_coreclr_fallback(bool &r_runtime_initialized) {
	if (s_runtime_bootstrapped && cached_godot_plugins_initialize != nullptr) {
		write_mono_log(".NET: Reusing already initialized CoreCLR runtime.");
		r_runtime_initialized = true;
		return cached_godot_plugins_initialize;
	}

	write_mono_log(".NET: Falling back to direct coreclr_initialize hosting...");
	active_coreclr_handle = nullptr;
	active_domain_id = 0;

	PackedStringArray tpa_list;
	PackedStringArray app_paths;
	HashSet<String> added_assemblies;

	Vector<String> probe_dirs;
	probe_dirs.push_back("/storage/emulated/0/mono/assemblies");
	probe_dirs.push_back("/storage/emulated/0/mono");
	probe_dirs.push_back(OS::get_singleton()->get_user_data_dir().path_join("mono_libs"));
	probe_dirs.push_back(OS::get_singleton()->get_user_data_dir().path_join("mono/assemblies"));
	probe_dirs.push_back(OS::get_singleton()->get_user_data_dir().path_join("mono"));
	probe_dirs.push_back(GodotSharpDirs::get_api_assemblies_dir());

	for (const String &dir_path : probe_dirs) {
		if (dir_path.is_empty() || !DirAccess::exists(dir_path)) {
			continue;
		}

		if (!app_paths.has(dir_path)) {
			app_paths.append(dir_path);
		}

		Ref<DirAccess> da = DirAccess::open(dir_path);
		if (da.is_valid()) {
			da->list_dir_begin();
			for (String file = da->get_next(); !file.is_empty(); file = da->get_next()) {
				if (!da->current_is_dir() && file.ends_with(".dll") && !file.begins_with("ForTesting") && !file.begins_with("for testing")) {
					if (!added_assemblies.has(file)) {
						added_assemblies.insert(file);
						tpa_list.append(dir_path.path_join(file));
					}
				}
			}
		}
	}

	write_mono_log(".NET: Scanned " + itos(tpa_list.size()) + " TPA assemblies.");

	String tpa_paths_str = String(":").join(tpa_list);
	String app_paths_str = String(":").join(app_paths);

	const char *property_keys[] = {
		"TRUSTED_PLATFORM_ASSEMBLIES",
		"APP_PATHS",
		"APP_NI_PATHS",
		"NATIVE_DLL_SEARCH_DIRECTORIES",
		"NativeDllSearchDirectories"
	};

	CharString tpa_utf8 = tpa_paths_str.utf8();
	CharString app_utf8 = app_paths_str.utf8();
	const char *property_values[] = {
		tpa_utf8.get_data(),
		app_utf8.get_data(),
		app_utf8.get_data(),
		app_utf8.get_data(),
		app_utf8.get_data()
	};

	String exe_dir = OS::get_singleton()->get_executable_path().get_base_dir();
	int rc = coreclr_initialize(
			exe_dir.utf8().get_data(),
			"GodotEngineDomain",
			5,
			property_keys,
			property_values,
			&active_coreclr_handle,
			&active_domain_id);

	if (rc != 0) {
		write_mono_log(vformat(".NET: coreclr_initialize failed with error: 0x%X", (unsigned int)rc));
		return nullptr;
	}

	String compiler_dll = "/storage/emulated/0/mono/assemblies/GodotAndroidCompiler.dll";
	if (FileAccess::exists(compiler_dll)) {
		int comp_rc = coreclr_create_delegate(active_coreclr_handle, active_domain_id,
				"GodotAndroidCompiler",
				"GodotAndroidCompiler.InProcessCompiler",
				"CompileProject",
				(void **)&roslyn_compile_fn);
		if (comp_rc == 0 && roslyn_compile_fn != nullptr) {
			write_mono_log(".NET: Bound in-process Roslyn compiler via direct CoreCLR!");
		}
	}

	godot_plugins_initialize_fn godot_plugins_initialize = nullptr;

#ifdef TOOLS_ENABLED
	int del_rc = coreclr_create_delegate(active_coreclr_handle, active_domain_id,
			"GodotPlugins",
			"GodotPlugins.Main",
			"InitializeFromEngine",
			(void **)&godot_plugins_initialize);
#else
	String assembly_name = get_csharp_project_name();
	int del_rc = coreclr_create_delegate(active_coreclr_handle, active_domain_id,
			assembly_name.utf8().get_data(),
			"GodotPlugins.Game.Main",
			"InitializeFromGameProject",
			(void **)&godot_plugins_initialize);
#endif

	if (del_rc != 0 || godot_plugins_initialize == nullptr) {
		write_mono_log(vformat(".NET: Failed to bind GodotPlugins entry point delegate (code 0x%X).", (unsigned int)del_rc));
		return nullptr;
	}

	cached_godot_plugins_initialize = godot_plugins_initialize;
	s_runtime_bootstrapped = true;
	r_runtime_initialized = true;
	return godot_plugins_initialize;
}

} // namespace

bool GDMono::should_initialize() {
#if defined(ANDROID_ENABLED)
	if (DirAccess::exists("/storage/emulated/0/mono/assemblies") || DirAccess::exists("/storage/emulated/0/mono")) {
		return true;
	}
#endif
	return true;
}

static bool _on_core_api_assembly_loaded() {
	if (!GDMonoCache::godot_api_cache_updated) {
		return false;
	}
	bool debug = false;
#ifdef DEBUG_ENABLED
	debug = true;
#endif
	GDMonoCache::managed_callbacks.GD_OnCoreApiAssemblyLoaded(debug);
	return true;
}

void GDMono::initialize() {
	write_mono_log("================= GDMono::initialize() =================");

#if defined(ANDROID_ENABLED)
	sync_all_android_native_libs();
	setup_android_dotnet_environment();

	int log_fd = open("/storage/emulated/0/mono/mono_log.txt", O_WRONLY | O_CREAT | O_APPEND, 0666);
	if (log_fd >= 0) {
		dup2(log_fd, STDOUT_FILENO);
		dup2(log_fd, STDERR_FILENO);
		close(log_fd);
	}
#endif

	_init_godot_api_hashes();

	if (s_plugins_initialized) {
		write_mono_log(".NET: Reusing already initialized GodotPlugins and callbacks (Android process reuse).");
#ifdef TOOLS_ENABLED
		plugin_callbacks = s_cached_plugin_callbacks;
#endif
		GDMonoCache::update_godot_api_cache(s_cached_managed_callbacks);
		_on_core_api_assembly_loaded();

		initialized = true;
		runtime_initialized = true;

#ifdef TOOLS_ENABLED
		if (!Engine::get_singleton()->is_project_manager_hint()) {
			String current_proj_name = get_csharp_project_name();
			ProjectSettings::get_singleton()->set_setting("dotnet/project/assembly_name", current_proj_name);

			ensure_csharp_project_files_exist();
			_try_load_project_assembly();
		}
#endif
		write_mono_log(".NET: GDMono successfully re-initialized using cached state!");
		return;
	}

	godot_plugins_initialize_fn godot_plugins_initialize = nullptr;

#if defined(ANDROID_ENABLED)
	if (load_hostfxr(hostfxr_lib_handle)) {
		godot_plugins_initialize = initialize_with_hostfxr(runtime_initialized);
	}

	if (godot_plugins_initialize == nullptr) {
		write_mono_log(".NET: hostfxr setup failed or not usable. Trying direct CoreCLR load...");
		if (load_coreclr(coreclr_dll_handle)) {
			if (mono_install_assembly_preload_hook != nullptr && !s_runtime_bootstrapped) {
				mono_install_assembly_preload_hook(&load_assembly_from_pck, nullptr);
				write_mono_log(".NET: Installed mono_install_assembly_preload_hook.");
			}
			godot_plugins_initialize = initialize_coreclr_fallback(runtime_initialized);
		}
	}
#endif

	if (godot_plugins_initialize == nullptr) {
		write_mono_log(".NET: godot_plugins_initialize is NULL. Skipping C# initialization.");
		return;
	}

	int32_t interop_funcs_size = 0;
	const void **interop_funcs = godotsharp::get_runtime_interop_funcs(interop_funcs_size);
	write_mono_log(".NET: Interop functions count: " + itos(interop_funcs_size));

	GDMonoCache::ManagedCallbacks managed_callbacks{};
	void *godot_dll_handle = nullptr;

#if defined(ANDROID_ENABLED)
	Dl_info dl_info;
	if (dladdr((const void *)&_on_core_api_assembly_loaded, &dl_info) && dl_info.dli_fname) {
		godot_dll_handle = dlopen(dl_info.dli_fname, RTLD_NOW | RTLD_GLOBAL);
		write_mono_log(String(".NET: dlopen(dl_info.dli_fname) = ") + (godot_dll_handle ? "SUCCESS" : "FAIL"));
	}
	if (!godot_dll_handle) {
		godot_dll_handle = dlopen("libgodot_android.so", RTLD_NOW | RTLD_GLOBAL);
	}
	if (!godot_dll_handle) {
		godot_dll_handle = dlopen(nullptr, RTLD_NOW | RTLD_GLOBAL);
	}
#if defined(RTLD_DEFAULT)
	if (!godot_dll_handle) {
		godot_dll_handle = RTLD_DEFAULT;
	}
#endif
#elif defined(UNIX_ENABLED)
	godot_dll_handle = dlopen(nullptr, RTLD_NOW);
#endif

	write_mono_log(vformat(".NET: godot_dll_handle resolved to: 0x%X", (uint64_t)godot_dll_handle));

#ifdef DEBUG_ENABLED
	write_mono_log(vformat(".NET: C++ API Core Hash: 0x%X", (uint64_t)get_api_core_hash()));
#ifdef TOOLS_ENABLED
	write_mono_log(vformat(".NET: C++ API Editor Hash: 0x%X", (uint64_t)get_api_editor_hash()));
#endif
#endif

#ifdef TOOLS_ENABLED
	gdmono::PluginCallbacks plugin_callbacks_res;
	write_mono_log(".NET: Calling godot_plugins_initialize()...");

	bool init_ok = godot_plugins_initialize(godot_dll_handle,
			Engine::get_singleton()->is_editor_hint(),
			&plugin_callbacks_res, &managed_callbacks,
			interop_funcs, interop_funcs_size);
	if (!init_ok) {
		write_mono_log(".NET: CRITICAL ERROR - godot_plugins_initialize() RETURNED FALSE!");
		ERR_PRINT(".NET: GodotPlugins initialization failed. Check /storage/emulated/0/mono/mono_log.txt");
		return;
	}

	plugin_callbacks = plugin_callbacks_res;
	s_cached_plugin_callbacks = plugin_callbacks_res;

#else
	bool init_ok = godot_plugins_initialize(godot_dll_handle, &managed_callbacks,
			interop_funcs, interop_funcs_size);
	if (!init_ok) {
		write_mono_log(".NET: CRITICAL ERROR - godot_plugins_initialize() RETURNED FALSE!");
		ERR_PRINT(".NET: GodotPlugins initialization failed. Check /storage/emulated/0/mono/mono_log.txt");
		return;
	}
#endif

	s_cached_managed_callbacks = managed_callbacks;
	s_plugins_initialized = true;

	write_mono_log(".NET: SUCCESS! Updating api cache...");
	GDMonoCache::update_godot_api_cache(managed_callbacks);

	_on_core_api_assembly_loaded();

	initialized = true;
	runtime_initialized = true;

#ifdef TOOLS_ENABLED
	if (!Engine::get_singleton()->is_project_manager_hint()) {
		String current_proj_name = get_csharp_project_name();
		ProjectSettings::get_singleton()->set_setting("dotnet/project/assembly_name", current_proj_name);

		ensure_csharp_project_files_exist();
		_try_load_project_assembly();
	}
#endif

	write_mono_log(".NET: GDMono fully initialized! C# is active and ready.");
}

#ifdef TOOLS_ENABLED
void GDMono::_try_load_project_assembly() {
	if (Engine::get_singleton()->is_project_manager_hint()) {
		return;
	}
	write_mono_log(".NET: Attempting to load project assembly...");

	String project_dir = ProjectSettings::get_singleton()->globalize_path("res://");
	String project_name = get_csharp_project_name();
	String bin_dir = project_dir.path_join(".godot/mono/temp/bin/Debug");
	String output_dll = bin_dir.path_join(project_name + ".dll");

	uint64_t latest_cs_time = get_latest_cs_modified_time(project_dir);

	bool dll_exists = FileAccess::exists(output_dll);
	uint64_t dll_time = 0;

	// I-verify kung ang DLL ay hindi corrupted o 0-byte gamit ang get_file_size()
	if (dll_exists) {
		uint64_t sz = get_file_size(output_dll);
		if (sz < 1024) {
			write_mono_log(".NET: Cleaning up corrupted/truncated output DLL (size < 1KB)...");
			DirAccess::remove_absolute(output_dll);
			dll_exists = false;
		} else {
			dll_time = FileAccess::get_modified_time(output_dll);
		}
	}

	bool need_build = false;
	if (!dll_exists && latest_cs_time > 0) {
		need_build = true;
	} else if (dll_exists && latest_cs_time > dll_time) {
		need_build = true;
	}

#if defined(ANDROID_ENABLED)
	if (need_build) {
		write_mono_log(".NET: Detected modified or missing .cs files! Requesting build...");
		execute_hybrid_csharp_build();
	}
#endif

	if (!_load_project_assembly()) {
#if defined(ANDROID_ENABLED)
		if (latest_cs_time > 0 && !need_build) {
			write_mono_log(".NET: Project assembly missing or failed to load. Retrying compilation...");
			if (execute_hybrid_csharp_build()) {
				if (_load_project_assembly()) {
					return;
				}
			}
		}
#endif
		write_mono_log(".NET: Notice - Project assembly not yet loaded.");
		return;
	}
}
#endif

void GDMono::_init_godot_api_hashes() {
#ifdef DEBUG_ENABLED
	get_api_core_hash();
#ifdef TOOLS_ENABLED
	get_api_editor_hash();
#endif
#endif
}

#ifdef TOOLS_ENABLED
bool GDMono::_load_project_assembly() {
	if (!runtime_initialized) {
		return false;
	}

	if (!plugin_callbacks.LoadProjectAssemblyCallback) {
		write_mono_log(".NET: LoadProjectAssemblyCallback is NULL. Skipping safely.");
		return false;
	}

	String base_name = get_csharp_project_name();
	write_mono_log(".NET: Searching assembly for project: '" + base_name + "'");

	Vector<String> name_variations;
	name_variations.push_back(base_name);
	name_variations.push_back(base_name.replace(" ", "-"));
	name_variations.push_back(base_name.replace(" ", "_"));
	name_variations.push_back(base_name.replace("-", " "));
	name_variations.push_back(base_name.replace("_", " "));

	Vector<String> probe_directories;
#if defined(ANDROID_ENABLED)
	probe_directories.push_back(ProjectSettings::get_singleton()->globalize_path("res://.godot/mono/temp/bin/Debug"));
	probe_directories.push_back("/storage/emulated/0/Documents/" + base_name + "/.godot/mono/temp/bin/Debug");
	probe_directories.push_back("/storage/emulated/0/Documents/" + base_name.replace(" ", "-") + "/.godot/mono/temp/bin/Debug");
	probe_directories.push_back("/storage/emulated/0/Documents/for testing/.godot/mono/temp/bin/Debug");
	probe_directories.push_back("/storage/emulated/0/mono/assemblies");
	probe_directories.push_back(OS::get_singleton()->get_user_data_dir().path_join("mono/assemblies"));
#else
	probe_directories.push_back(ProjectSettings::get_singleton()->globalize_path("res://.godot/mono/temp/bin/Debug"));
#endif

	String found_path;
	for (int i = 0; i < probe_directories.size(); i++) {
		String dir = probe_directories[i];
		if (!DirAccess::exists(dir)) {
			continue;
		}
		for (int j = 0; j < name_variations.size(); j++) {
			String candidate = dir.path_join(name_variations[j] + ".dll");
			if (FileAccess::exists(candidate)) {
				// ANTI-CORRUPT CHECK: Huwag pansinin ang file na 0 bytes o sira gamit ang get_file_size()
				if (get_file_size(candidate) < 1024) {
					write_mono_log(".NET: Discarding corrupted/0-byte assembly candidate: " + candidate);
					DirAccess::remove_absolute(candidate);
					continue;
				}
				found_path = candidate;
				break;
			}
		}
		if (!found_path.is_empty()) {
			break;
		}
	}

	if (found_path.is_empty()) {
		write_mono_log(".NET: Warning - Could not find assembly for '" + base_name + "'.");
		return false;
	}

	write_mono_log(".NET: SUCCESS! Found project assembly: " + found_path);
	write_mono_log(".NET: Invoking LoadProjectAssemblyCallback...");

	fflush(stdout);
	fflush(stderr);

	Char16String path_utf16 = found_path.utf16();
	String loaded_assembly_path;

	bool success = plugin_callbacks.LoadProjectAssemblyCallback(
			(const char16_t *)path_utf16.get_data(),
			&loaded_assembly_path);

	fflush(stdout);
	fflush(stderr);

	if (success) {
		project_assembly_path = loaded_assembly_path.simplify_path();
		project_assembly_modified_time = FileAccess::get_modified_time(loaded_assembly_path);
		write_mono_log(".NET: FULL SUCCESS! Project assembly bound and active: " + project_assembly_path);
	} else {
		write_mono_log(".NET: Callback failed to bind assembly: " + found_path);
		// Kung nag-fail i-load ang corrupted file, alisin ito upang hindi paulit-ulit mag-BadImageFormatException
		DirAccess::remove_absolute(found_path);
	}
	return success;
}

Error GDMono::reload_project_assemblies() {
	if (!runtime_initialized) {
		return ERR_BUG;
	}

#if defined(ANDROID_ENABLED) && defined(TOOLS_ENABLED)
	write_mono_log(".NET: [Pre-Run/Reload Hook] Checking if build is needed before running...");
	String project_dir = ProjectSettings::get_singleton()->globalize_path("res://");
	uint64_t latest_cs_time = get_latest_cs_modified_time(project_dir);

	if (latest_cs_time > 0 && (project_assembly_modified_time == 0 || latest_cs_time > project_assembly_modified_time)) {
		execute_hybrid_csharp_build();
	}
#endif

	if (project_assembly_path.is_empty()) {
		if (!_load_project_assembly()) {
			return ERR_CANT_OPEN;
		}
	}
	return OK;
}
#endif

GDMono::GDMono() {
	singleton = this;
}

GDMono::~GDMono() {
	finalizing_scripts_domain = true;
	finalizing_scripts_domain = false;
	initialized = false;
	singleton = nullptr;
}

namespace mono_bind {
GodotSharp *GodotSharp::singleton = nullptr;
void GodotSharp::reload_assemblies(bool p_soft_reload) {
#ifdef TOOLS_ENABLED
	if (GDMono::get_singleton() && GDMono::get_singleton()->is_initialized()) {
		GDMono::get_singleton()->reload_project_assemblies();
	}
#endif
}
GodotSharp::GodotSharp() { singleton = this; }
GodotSharp::~GodotSharp() { singleton = nullptr; }
} // namespace mono_bind
