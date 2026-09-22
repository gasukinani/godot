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

#ifdef UNIX_ENABLED
#include <dlfcn.h>
#endif

#ifdef ANDROID_ENABLED
#include "../thirdparty/mono_delegates.h"
#endif

GDMono *GDMono::singleton = nullptr;

namespace {
hostfxr_initialize_for_dotnet_command_line_fn hostfxr_initialize_for_dotnet_command_line = nullptr;
hostfxr_initialize_for_runtime_config_fn hostfxr_initialize_for_runtime_config = nullptr;
hostfxr_get_runtime_delegate_fn hostfxr_get_runtime_delegate = nullptr;
hostfxr_close_fn hostfxr_close = nullptr;

// Pinapayagan na ang CoreCLR/Mono delegates maging sa Editor (TOOLS_ENABLED) para sa Android
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
#if defined(ANDROID_ENABLED)
	return false; // Walang desktop `dotnet` CLI sa Android
#else
	String pipe;
	List<String> args;
	args.push_back("--list-sdks");

	int exitcode;
	Error err = OS::get_singleton()->execute("dotnet", args, &pipe, &exitcode, true);

	ERR_FAIL_COND_V_MSG(err != OK, false, String(".NET failed to get list of installed SDKs. Error: ") + error_names[err]);
	ERR_FAIL_COND_V_MSG(exitcode != 0, false, pipe);

	Vector<String> sdks = pipe.strip_edges().replace("\r\n", "\n").split("\n", false);
	godotsharp::SemVerParser sem_ver_parser;
	godotsharp::SemVer latest_sdk_version;
	String latest_sdk_path;

	for (const String &sdk : sdks) {
		String version_string = sdk.get_slice(" ", 0);
		String path = sdk.get_slice(" ", 1);
		path = path.substr(1, path.length() - 2);

		godotsharp::SemVer version;
		if (!sem_ver_parser.parse(version_string, version)) {
			continue;
		}
		if (!DirAccess::exists(path)) {
			continue;
		}
		if (version > latest_sdk_version) {
			latest_sdk_version = version;
			latest_sdk_path = path;
		}
	}

	if (!latest_sdk_path.is_empty()) {
		r_dotnet_root = latest_sdk_path.path_join("..").simplify_path();
		return true;
	}
	return false;
#endif
}
#endif

String find_hostfxr() {
#if defined(ANDROID_ENABLED)
	String ext_fxr = "/storage/emulated/0/mono/libhostfxr.so";
	if (FileAccess::exists(ext_fxr)) {
		return ext_fxr;
	}
	return "libhostfxr.so";
#else
#ifdef TOOLS_ENABLED
	String dotnet_root;
	String fxr_path;
	if (godotsharp::hostfxr_resolver::try_get_path(dotnet_root, fxr_path)) {
		return fxr_path;
	}
	if (try_get_dotnet_root_from_command_line(dotnet_root)) {
		if (godotsharp::hostfxr_resolver::try_get_path_from_dotnet_root(dotnet_root, fxr_path)) {
			return fxr_path;
		}
	}
	return String();
#else
#if defined(WINDOWS_ENABLED)
	String probe_path = GodotSharpDirs::get_api_assemblies_dir().path_join("hostfxr.dll");
#elif defined(MACOS_ENABLED)
	String probe_path = GodotSharpDirs::get_api_assemblies_dir().path_join("libhostfxr.dylib");
#elif defined(UNIX_ENABLED)
	String probe_path = GodotSharpDirs::get_api_assemblies_dir().path_join("libhostfxr.so");
#endif
	if (FileAccess::exists(probe_path)) {
		return probe_path;
	}
	return String();
#endif
#endif
}

String find_monosgen() {
#if defined(ANDROID_ENABLED)
	String external_path = "/storage/emulated/0/mono/libmonosgen-2.0.so";
	if (FileAccess::exists(external_path)) {
		return external_path;
	}
	return "libmonosgen-2.0.so";
#else
#if defined(WINDOWS_ENABLED)
	String probe_path = GodotSharpDirs::get_api_assemblies_dir().path_join("monosgen-2.0.dll");
#elif defined(MACOS_ENABLED)
	String probe_path = GodotSharpDirs::get_api_assemblies_dir().path_join("libmonosgen-2.0.dylib");
#elif defined(UNIX_ENABLED)
	String probe_path = GodotSharpDirs::get_api_assemblies_dir().path_join("libmonosgen-2.0.so");
#endif
	if (FileAccess::exists(probe_path)) {
		return probe_path;
	}
	return String();
#endif
}

String find_coreclr() {
#if defined(ANDROID_ENABLED)
	String external_path = "/storage/emulated/0/mono/libcoreclr.so";
	if (FileAccess::exists(external_path)) {
		return external_path;
	}
	return "libcoreclr.so";
#else
#if defined(WINDOWS_ENABLED)
	String probe_path = GodotSharpDirs::get_api_assemblies_dir().path_join("coreclr.dll");
#elif defined(MACOS_ENABLED)
	String probe_path = GodotSharpDirs::get_api_assemblies_dir().path_join("libcoreclr.dylib");
#elif defined(UNIX_ENABLED)
	String probe_path = GodotSharpDirs::get_api_assemblies_dir().path_join("libcoreclr.so");
#endif
	if (FileAccess::exists(probe_path)) {
		return probe_path;
	}
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
	bool is_monovm = false;
	if (coreclr_path.is_empty() || !FileAccess::exists(coreclr_path)) {
		coreclr_path = find_monosgen();
		is_monovm = true;
	}

	if (coreclr_path.is_empty()) {
		return false;
	}

	Error err = OS::get_singleton()->open_dynamic_library(coreclr_path, r_coreclr_dll_handle);
	if (err != OK) {
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

	return (coreclr_initialize && coreclr_create_delegate);
}

#ifdef TOOLS_ENABLED
load_assembly_and_get_function_pointer_fn initialize_hostfxr_for_config(const char_t *p_config_path) {
	hostfxr_handle cxt = nullptr;
	int rc = hostfxr_initialize_for_runtime_config(p_config_path, nullptr, &cxt);
	if (rc != 0 || cxt == nullptr) {
		hostfxr_close(cxt);
		ERR_FAIL_V_MSG(nullptr, "hostfxr_initialize_for_runtime_config failed with code: " + itos(rc));
	}

	void *load_assembly_and_get_function_pointer = nullptr;
	rc = hostfxr_get_runtime_delegate(cxt, hdt_load_assembly_and_get_function_pointer, &load_assembly_and_get_function_pointer);
	if (rc != 0 || load_assembly_and_get_function_pointer == nullptr) {
		ERR_FAIL_V_MSG(nullptr, "hostfxr_get_runtime_delegate failed with code: " + itos(rc));
	}

	hostfxr_close(cxt);
	return (load_assembly_and_get_function_pointer_fn)load_assembly_and_get_function_pointer;
}
#else
load_assembly_and_get_function_pointer_fn initialize_hostfxr_self_contained(const char_t *p_main_assembly_path) {
	hostfxr_handle cxt = nullptr;
	List<String> cmdline_args = OS::get_singleton()->get_cmdline_args();

	List<HostFxrCharString> argv_store;
	Vector<const char_t *> argv;
	argv.resize(cmdline_args.size() + 1);
	argv.write[0] = p_main_assembly_path;

	int i = 1;
	for (const String &E : cmdline_args) {
		HostFxrCharString &stored = argv_store.push_back(str_to_hostfxr(E))->get();
		argv.write[i] = get_data(stored);
		i++;
	}

	int rc = hostfxr_initialize_for_dotnet_command_line(argv.size(), argv.ptrw(), nullptr, &cxt);
	if (rc != 0 || cxt == nullptr) {
		hostfxr_close(cxt);
		ERR_FAIL_V_MSG(nullptr, "hostfxr_initialize_for_dotnet_command_line failed with code: " + itos(rc));
	}

	void *load_assembly_and_get_function_pointer = nullptr;
	rc = hostfxr_get_runtime_delegate(cxt, hdt_load_assembly_and_get_function_pointer, &load_assembly_and_get_function_pointer);
	if (rc != 0 || load_assembly_and_get_function_pointer == nullptr) {
		ERR_FAIL_V_MSG(nullptr, "hostfxr_get_runtime_delegate failed with code: " + itos(rc));
	}

	hostfxr_close(cxt);
	return (load_assembly_and_get_function_pointer_fn)load_assembly_and_get_function_pointer;
}
#endif

#ifdef TOOLS_ENABLED
using godot_plugins_initialize_fn = bool (*)(void *, bool, gdmono::PluginCallbacks *, GDMonoCache::ManagedCallbacks *, const void **, int32_t);
#else
using godot_plugins_initialize_fn = bool (*)(void *, GDMonoCache::ManagedCallbacks *, const void **, int32_t);
#endif

#ifdef TOOLS_ENABLED
godot_plugins_initialize_fn initialize_hostfxr_and_godot_plugins(bool &r_runtime_initialized) {
	godot_plugins_initialize_fn godot_plugins_initialize = nullptr;

	HostFxrCharString godot_plugins_path = str_to_hostfxr(GodotSharpDirs::get_api_assemblies_dir().path_join("GodotPlugins.dll"));
	HostFxrCharString config_path = str_to_hostfxr(GodotSharpDirs::get_api_assemblies_dir().path_join("GodotPlugins.runtimeconfig.json"));

	load_assembly_and_get_function_pointer_fn load_assembly_and_get_function_pointer =
			initialize_hostfxr_for_config(get_data(config_path));
	if (load_assembly_and_get_function_pointer == nullptr) {
		return nullptr;
	}

	r_runtime_initialized = true;
	int rc = load_assembly_and_get_function_pointer(get_data(godot_plugins_path),
			HOSTFXR_STR("GodotPlugins.Main, GodotPlugins"),
			HOSTFXR_STR("InitializeFromEngine"),
			UNMANAGEDCALLERSONLY_METHOD,
			nullptr,
			(void **)&godot_plugins_initialize);
	ERR_FAIL_COND_V_MSG(rc != 0, nullptr, ".NET: Failed to get GodotPlugins initialization function pointer");

	return godot_plugins_initialize;
}
#else
godot_plugins_initialize_fn initialize_hostfxr_and_godot_plugins(bool &r_runtime_initialized) {
	godot_plugins_initialize_fn godot_plugins_initialize = nullptr;
	String assembly_name = Path::get_csharp_project_name();

	HostFxrCharString assembly_path = str_to_hostfxr(GodotSharpDirs::get_api_assemblies_dir().path_join(assembly_name + ".dll"));
	load_assembly_and_get_function_pointer_fn load_assembly_and_get_function_pointer =
			initialize_hostfxr_self_contained(get_data(assembly_path));
	ERR_FAIL_NULL_V(load_assembly_and_get_function_pointer, nullptr);

	r_runtime_initialized = true;
	int rc = load_assembly_and_get_function_pointer(get_data(assembly_path),
			get_data(str_to_hostfxr("GodotPlugins.Game.Main, " + assembly_name)),
			HOSTFXR_STR("InitializeFromGameProject"),
			UNMANAGEDCALLERSONLY_METHOD,
			nullptr,
			(void **)&godot_plugins_initialize);
	ERR_FAIL_COND_V_MSG(rc != 0, nullptr, ".NET: Failed to get GodotPlugins initialization function pointer");

	return godot_plugins_initialize;
}
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

	String ext_path_assemblies = "/storage/emulated/0/mono/assemblies/".path_join(assembly_name);
	String ext_path_root = "/storage/emulated/0/mono/".path_join(assembly_name);
	String path;

	if (FileAccess::exists(ext_path_assemblies)) {
		path = ext_path_assemblies;
	} else if (FileAccess::exists(ext_path_root)) {
		path = ext_path_root;
	} else {
		path = GodotSharpDirs::get_api_assemblies_dir().path_join(assembly_name);
	}

	if (!FileAccess::exists(path)) {
		return nullptr;
	}

	Vector<uint8_t> data = FileAccess::get_file_as_bytes(path);
	ERR_FAIL_COND_V_MSG(data.is_empty(), nullptr, ".NET: Could not read assembly in '" + path + "'.");

	MonoImageOpenStatus status = MONO_IMAGE_OK;
	MonoImage *image = mono_image_open_from_data_with_name(
			reinterpret_cast<char *>(data.ptrw()), data.size(),
			/*need_copy*/ true, &status, ref_only, assembly_name.utf8().get_data());
	ERR_FAIL_COND_V_MSG(status != MONO_IMAGE_OK || image == nullptr, nullptr, ".NET: Failed to open assembly image.");

	status = MONO_IMAGE_OK;
	MonoAssembly *assembly = mono_assembly_load_from_full(image, assembly_name.utf8().get_data(), &status, ref_only);
	ERR_FAIL_COND_V_MSG(status != MONO_IMAGE_OK || assembly == nullptr, nullptr, ".NET: Failed to load assembly from image.");

	return assembly;
}
#endif

godot_plugins_initialize_fn initialize_coreclr_and_godot_plugins(bool &r_runtime_initialized) {
	godot_plugins_initialize_fn godot_plugins_initialize = nullptr;

#ifdef ANDROID_ENABLED
	if (mono_install_assembly_preload_hook != nullptr) {
		mono_install_assembly_preload_hook(&load_assembly_from_pck, nullptr);
	}
#endif

	void *coreclr_handle = nullptr;
	unsigned int domain_id = 0;
	int rc = coreclr_initialize(nullptr, nullptr, 0, nullptr, nullptr, &coreclr_handle, &domain_id);
	ERR_FAIL_COND_V_MSG(rc != 0, nullptr, ".NET: Failed to initialize CoreCLR/Mono.");

	r_runtime_initialized = true;

#ifdef TOOLS_ENABLED
	coreclr_create_delegate(coreclr_handle, domain_id,
			"GodotPlugins",
			"GodotPlugins.Main",
			"InitializeFromEngine",
			(void **)&godot_plugins_initialize);
#else
	String assembly_name = Path::get_csharp_project_name();
	coreclr_create_delegate(coreclr_handle, domain_id,
			assembly_name.utf8().get_data(),
			"GodotPlugins.Game.Main",
			"InitializeFromGameProject",
			(void **)&godot_plugins_initialize);
#endif
	ERR_FAIL_NULL_V_MSG(godot_plugins_initialize, nullptr, ".NET: Failed to get GodotPlugins initialization function pointer");

	return godot_plugins_initialize;
}

} // namespace

bool GDMono::should_initialize() {
#ifdef TOOLS_ENABLED
	return true;
#else
	return OS::get_singleton()->has_feature("dotnet");
#endif
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
	print_verbose(".NET: Initializing module...");
	_init_godot_api_hashes();

	godot_plugins_initialize_fn godot_plugins_initialize = nullptr;

#if !defined(APPLE_EMBEDDED_ENABLED)
	String assemblies_dir = GodotSharpDirs::get_api_assemblies_dir();
	bool dir_exists = DirAccess::exists(assemblies_dir);

#if defined(ANDROID_ENABLED)
	if (!dir_exists && DirAccess::exists("/storage/emulated/0/mono")) {
		dir_exists = true;
	}
#endif

	if (!dir_exists) {
		OS::get_singleton()->alert(vformat(RTR("Unable to find the .NET assemblies directory.\nMake sure the '%s' directory exists and contains the .NET assemblies."), assemblies_dir), RTR(".NET assemblies not found"));
		ERR_FAIL_MSG(".NET: Assemblies not found");
	}
#endif

	// 1. Subukan munang mag-load gamit ang hostfxr
	if (load_hostfxr(hostfxr_dll_handle)) {
		godot_plugins_initialize = initialize_hostfxr_and_godot_plugins(runtime_initialized);
	}

	// 2. Fallback sa CoreCLR / Mono (Gagana kapwa sa Editor at Game sa Android)
	if (godot_plugins_initialize == nullptr && load_coreclr(coreclr_dll_handle)) {
		godot_plugins_initialize = initialize_coreclr_and_godot_plugins(runtime_initialized);
	}

	if (godot_plugins_initialize == nullptr) {
#ifdef TOOLS_ENABLED
		OS::get_singleton()->alert(TTR("Hindi ma-load ang .NET runtime (hostfxr o libmonosgen-2.0.so).\nPakisigurado na nabigyan ng storage permission ang Godot at may runtime libraries sa /storage/emulated/0/mono/."), TTR("Failed to load .NET runtime"));
#endif
		ERR_FAIL_MSG(".NET: Failed to load .NET runtime");
	}

	int32_t interop_funcs_size = 0;
	const void **interop_funcs = godotsharp::get_runtime_interop_funcs(interop_funcs_size);

	GDMonoCache::ManagedCallbacks managed_callbacks{};
	void *godot_dll_handle = nullptr;

#if defined(UNIX_ENABLED) && !defined(MACOS_ENABLED) && !defined(APPLE_EMBEDDED_ENABLED)
	godot_dll_handle = dlopen(nullptr, RTLD_NOW);
#endif

#ifdef TOOLS_ENABLED
	gdmono::PluginCallbacks plugin_callbacks_res;
	bool init_ok = godot_plugins_initialize(godot_dll_handle,
			Engine::get_singleton()->is_editor_hint(),
			&plugin_callbacks_res, &managed_callbacks,
			interop_funcs, interop_funcs_size);
	ERR_FAIL_COND_MSG(!init_ok, ".NET: GodotPlugins initialization failed");
	plugin_callbacks = plugin_callbacks_res;
#else
	bool init_ok = godot_plugins_initialize(godot_dll_handle, &managed_callbacks,
			interop_funcs, interop_funcs_size);
	ERR_FAIL_COND_MSG(!init_ok, ".NET: GodotPlugins initialization failed");
#endif

	GDMonoCache::update_godot_api_cache(managed_callbacks);
	print_verbose(".NET: GodotPlugins initialized");

	_on_core_api_assembly_loaded();

#ifdef TOOLS_ENABLED
	_try_load_project_assembly();
#endif

	initialized = true;
}

#ifdef TOOLS_ENABLED
void GDMono::_try_load_project_assembly() {
	if (Engine::get_singleton()->is_project_manager_hint()) {
		return;
	}
	if (!_load_project_assembly()) {
		if (OS::get_singleton()->is_stdout_verbose()) {
			print_error(".NET: Failed to load project assembly");
		}
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
	String assembly_name = Path::get_csharp_project_name();
	String assembly_path = GodotSharpDirs::get_res_temp_assemblies_dir().path_join(assembly_name + ".dll");
	assembly_path = ProjectSettings::get_singleton()->globalize_path(assembly_path);

#if defined(ANDROID_ENABLED)
	if (!FileAccess::exists(assembly_path)) {
		String ext_path = "/storage/emulated/0/mono/assemblies/".path_join(assembly_name + ".dll");
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

#ifdef GD_MONO_HOT_RELOAD
void GDMono::reload_failure() {
	if (++project_load_failure_count >= (int)GLOBAL_GET("dotnet/project/assembly_reload_attempts")) {
		project_load_failure_count = 0;
		ERR_PRINT_ED(".NET: Giving up on assembly reloading.");

		String assembly_name = Path::get_csharp_project_name();
		String assembly_path = GodotSharpDirs::get_res_temp_assemblies_dir().path_join(assembly_name + ".dll");
		assembly_path = ProjectSettings::get_singleton()->globalize_path(assembly_path);
		project_assembly_path = assembly_path.simplify_path();
		project_assembly_modified_time = FileAccess::get_modified_time(assembly_path);
	}
}

Error GDMono::reload_project_assemblies() {
	ERR_FAIL_COND_V(!runtime_initialized, ERR_BUG);
	finalizing_scripts_domain = true;

	if (!get_plugin_callbacks().UnloadProjectPluginCallback()) {
		ERR_PRINT_ED(".NET: Failed to unload assemblies.");
		reload_failure();
		return FAILED;
	}

	finalizing_scripts_domain = false;
	if (!_load_project_assembly()) {
		ERR_PRINT_ED(".NET: Failed to load project assembly.");
		reload_failure();
		return ERR_CANT_OPEN;
	}

	if (project_load_failure_count > 0) {
		project_load_failure_count = 0;
	}
	return OK;
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
void GodotSharp::reload_assemblies() {
#ifdef GD_MONO_HOT_RELOAD
	CRASH_COND(CSharpLanguage::get_singleton() == nullptr);
	if (CSharpLanguage::get_singleton()->is_assembly_reloading_needed()) {
		CSharpLanguage::get_singleton()->reload_assemblies();
	}
#endif
}
GodotSharp::GodotSharp() {
	singleton = this;
}
GodotSharp::~GodotSharp() {
	singleton = nullptr;
}
} // namespace MonoBind
