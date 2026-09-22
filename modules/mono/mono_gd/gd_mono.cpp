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
#endif

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/os/os.h"
#include "core/os/thread.h"
#include "core/templates/hash_set.h"

#ifdef UNIX_ENABLED
#include <dlfcn.h>
#endif

#ifdef ANDROID_ENABLED
#include "../thirdparty/mono_delegates.h"
#endif

GDMono *GDMono::singleton = nullptr;

namespace {

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

hostfxr_initialize_for_dotnet_command_line_fn hostfxr_initialize_for_dotnet_command_line = nullptr;
hostfxr_initialize_for_runtime_config_fn hostfxr_initialize_for_runtime_config = nullptr;
hostfxr_get_runtime_delegate_fn hostfxr_get_runtime_delegate = nullptr;
hostfxr_close_fn hostfxr_close = nullptr;

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
#endif

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

#ifdef TOOLS_ENABLED
bool try_get_dotnet_root_from_command_line(String &r_dotnet_root) {
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

String find_monosgen() {
#if defined(ANDROID_ENABLED)
	return prepare_android_executable_lib("libmonosgen-2.0.so");
#else
	return String();
#endif
}

String find_coreclr() {
#if defined(ANDROID_ENABLED)
	return prepare_android_executable_lib("libcoreclr.so");
#else
	return String();
#endif
}

bool load_hostfxr(void *&r_hostfxr_dll_handle) {
	String hostfxr_path = find_hostfxr();
	if (hostfxr_path.is_empty()) {
		return false;
	}

	Error err = OS::get_singleton()->open_dynamic_library(hostfxr_path, r_hostfxr_dll_handle);
	if (err != OK) {
		return false;
	}

	void *lib = r_hostfxr_dll_handle;
	void *symbol = nullptr;

	err = OS::get_singleton()->get_dynamic_library_symbol_handle(lib, "hostfxr_initialize_for_dotnet_command_line", symbol);
	ERR_FAIL_COND_V(err != OK, false);
	hostfxr_initialize_for_dotnet_command_line = (hostfxr_initialize_for_dotnet_command_line_fn)symbol;

	err = OS::get_singleton()->get_dynamic_library_symbol_handle(lib, "hostfxr_initialize_for_runtime_config", symbol);
	ERR_FAIL_COND_V(err != OK, false);
	hostfxr_initialize_for_runtime_config = (hostfxr_initialize_for_runtime_config_fn)symbol;

	err = OS::get_singleton()->get_dynamic_library_symbol_handle(lib, "hostfxr_get_runtime_delegate", symbol);
	ERR_FAIL_COND_V(err != OK, false);
	hostfxr_get_runtime_delegate = (hostfxr_get_runtime_delegate_fn)symbol;

	err = OS::get_singleton()->get_dynamic_library_symbol_handle(lib, "hostfxr_close", symbol);
	ERR_FAIL_COND_V(err != OK, false);
	hostfxr_close = (hostfxr_close_fn)symbol;

	return (hostfxr_initialize_for_runtime_config && hostfxr_get_runtime_delegate && hostfxr_close);
}

bool load_coreclr(void *&r_coreclr_dll_handle) {
	String coreclr_path = find_coreclr();
	if (coreclr_path.is_empty() || !FileAccess::exists(coreclr_path)) {
		coreclr_path = find_monosgen();
	}

	if (coreclr_path.is_empty()) {
		write_mono_log(".NET: Neither libcoreclr.so nor libmonosgen-2.0.so found.");
		return false;
	}

	write_mono_log(".NET: Loading runtime library: " + coreclr_path);
	Error err = OS::get_singleton()->open_dynamic_library(coreclr_path, r_coreclr_dll_handle);
	if (err != OK) {
		write_mono_log(".NET: Failed to open dynamic library: " + coreclr_path);
		return false;
	}

	void *lib = r_coreclr_dll_handle;
	void *symbol = nullptr;

	err = OS::get_singleton()->get_dynamic_library_symbol_handle(lib, "coreclr_initialize", symbol);
	ERR_FAIL_COND_V(err != OK, false);
	coreclr_initialize = (coreclr_initialize_fn)symbol;

	err = OS::get_singleton()->get_dynamic_library_symbol_handle(lib, "coreclr_create_delegate", symbol);
	ERR_FAIL_COND_V(err != OK, false);
	coreclr_create_delegate = (coreclr_create_delegate_fn)symbol;

#ifdef ANDROID_ENABLED
	err = OS::get_singleton()->get_dynamic_library_symbol_handle(lib, "mono_install_assembly_preload_hook", symbol);
	if (err == OK) {
		mono_install_assembly_preload_hook = (mono_install_assembly_preload_hook_fn)symbol;
	}
	err = OS::get_singleton()->get_dynamic_library_symbol_handle(lib, "mono_assembly_name_get_name", symbol);
	if (err == OK) {
		mono_assembly_name_get_name = (mono_assembly_name_get_name_fn)symbol;
	}
	err = OS::get_singleton()->get_dynamic_library_symbol_handle(lib, "mono_assembly_name_get_culture", symbol);
	if (err == OK) {
		mono_assembly_name_get_culture = (mono_assembly_name_get_culture_fn)symbol;
	}
	err = OS::get_singleton()->get_dynamic_library_symbol_handle(lib, "mono_image_open_from_data_with_name", symbol);
	if (err == OK) {
		mono_image_open_from_data_with_name = (mono_image_open_from_data_with_name_fn)symbol;
	}
	err = OS::get_singleton()->get_dynamic_library_symbol_handle(lib, "mono_assembly_load_from_full", symbol);
	if (err == OK) {
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

godot_plugins_initialize_fn initialize_coreclr_and_godot_plugins(bool &r_runtime_initialized) {
	godot_plugins_initialize_fn godot_plugins_initialize = nullptr;

#ifdef ANDROID_ENABLED
	if (mono_install_assembly_preload_hook != nullptr) {
		mono_install_assembly_preload_hook(&load_assembly_from_pck, nullptr);
		write_mono_log(".NET: Installed mono_install_assembly_preload_hook.");
	}
#endif

	void *coreclr_handle = nullptr;
	unsigned int domain_id = 0;

	PackedStringArray tpa_list;
	PackedStringArray app_paths;
	HashSet<String> added_assemblies;

	Vector<String> probe_dirs;
	probe_dirs.push_back("/storage/emulated/0/mono/assemblies");
	probe_dirs.push_back("/storage/emulated/0/mono");
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
				if (!da->current_is_dir() && file.ends_with(".dll")) {
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
			&coreclr_handle,
			&domain_id);

	if (rc != 0) {
		write_mono_log(vformat(".NET: coreclr_initialize failed with error: 0x%X", (unsigned int)rc));
		return nullptr;
	}

	r_runtime_initialized = true;
	write_mono_log(".NET: CoreCLR/Mono initialized successfully.");

#ifdef TOOLS_ENABLED
	int del_rc = coreclr_create_delegate(coreclr_handle, domain_id,
			"GodotPlugins",
			"GodotPlugins.Main",
			"InitializeFromEngine",
			(void **)&godot_plugins_initialize);

	if (del_rc != 0 || godot_plugins_initialize == nullptr) {
		del_rc = coreclr_create_delegate(coreclr_handle, domain_id,
				"GodotPlugins, Version=4.3.0.0, Culture=neutral, PublicKeyToken=null",
				"GodotPlugins.Main",
				"InitializeFromEngine",
				(void **)&godot_plugins_initialize);
	}
#else
	String assembly_name = Path::get_csharp_project_name();
	int del_rc = coreclr_create_delegate(coreclr_handle, domain_id,
			assembly_name.utf8().get_data(),
			"GodotPlugins.Game.Main",
			"InitializeFromGameProject",
			(void **)&godot_plugins_initialize);
#endif

	if (del_rc != 0 || godot_plugins_initialize == nullptr) {
		write_mono_log(vformat(".NET: Failed to bind GodotPlugins entry point delegate (code 0x%X).", (unsigned int)del_rc));
		return nullptr;
	}

	write_mono_log(".NET: Successfully acquired GodotPlugins initialize pointer.");
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
	_init_godot_api_hashes();

	godot_plugins_initialize_fn godot_plugins_initialize = nullptr;

#if defined(ANDROID_ENABLED)
	if (load_coreclr(coreclr_dll_handle)) {
		godot_plugins_initialize = initialize_coreclr_and_godot_plugins(runtime_initialized);
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
#else
	bool init_ok = godot_plugins_initialize(godot_dll_handle, &managed_callbacks,
			interop_funcs, interop_funcs_size);
	if (!init_ok) {
		write_mono_log(".NET: CRITICAL ERROR - godot_plugins_initialize() RETURNED FALSE!");
		ERR_PRINT(".NET: GodotPlugins initialization failed. Check /storage/emulated/0/mono/mono_log.txt");
		return;
	}
#endif

	write_mono_log(".NET: SUCCESS! Updating api cache...");
	GDMonoCache::update_godot_api_cache(managed_callbacks);

	_on_core_api_assembly_loaded();

#ifdef TOOLS_ENABLED
	_try_load_project_assembly();
#endif

	initialized = true;
	write_mono_log(".NET: GDMono fully initialized! C# is active and ready.");
}

#ifdef TOOLS_ENABLED
void GDMono::_try_load_project_assembly() {
	if (Engine::get_singleton()->is_project_manager_hint()) {
		return;
	}
	if (!_load_project_assembly()) {
		write_mono_log(".NET: Notice - Project assembly not yet loaded.");
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

#ifdef DEBUG_ENABLED
uint64_t GDMono::get_api_core_hash() {
	if (api_core_hash == 0) {
		api_core_hash = ClassDB::get_api_hash(ClassDB::API_CORE);
	}
	return api_core_hash;
}
#ifdef TOOLS_ENABLED
uint64_t GDMono::get_api_editor_hash() {
	if (api_editor_hash == 0) {
		api_editor_hash = ClassDB::get_api_hash(ClassDB::API_EDITOR);
	}
	return api_editor_hash;
}
#endif
#endif

#ifdef TOOLS_ENABLED
bool GDMono::_load_project_assembly() {
	if (!initialized) {
		return false;
	}

	String assembly_name = Path::get_csharp_project_name();
	String assembly_path = GodotSharpDirs::get_res_temp_assemblies_dir().path_join(assembly_name + ".dll");
	assembly_path = ProjectSettings::get_singleton()->globalize_path(assembly_path);

#if defined(ANDROID_ENABLED)
	if (!FileAccess::exists(assembly_path)) {
		String ext_path = String("/storage/emulated/0/mono/assemblies").path_join(assembly_name + ".dll");
		if (FileAccess::exists(ext_path)) {
			assembly_path = ext_path;
		}
	}
#endif

	if (!FileAccess::exists(assembly_path)) {
		return false;
	}

	String loaded_assembly_path;
	bool success = plugin_callbacks.LoadProjectAssemblyCallback(assembly_path.utf16().get_data(), &loaded_assembly_path);

	if (success) {
		project_assembly_path = loaded_assembly_path.simplify_path();
		project_assembly_modified_time = FileAccess::get_modified_time(loaded_assembly_path);
	}
	return success;
}
#endif

GDMono::GDMono() {
	singleton = this;
}

GDMono::~GDMono() {
	finalizing_scripts_domain = true;
	if (hostfxr_dll_handle) {
		OS::get_singleton()->close_dynamic_library(hostfxr_dll_handle);
	}
	if (coreclr_dll_handle) {
		OS::get_singleton()->close_dynamic_library(coreclr_dll_handle);
	}
	finalizing_scripts_domain = false;
	runtime_initialized = false;
	singleton = nullptr;
}

namespace MonoBind {
GodotSharp *GodotSharp::singleton = nullptr;
void GodotSharp::reload_assemblies() {}
GodotSharp::GodotSharp() { singleton = this; }
GodotSharp::~GodotSharp() { singleton = nullptr; }
} // namespace MonoBind
