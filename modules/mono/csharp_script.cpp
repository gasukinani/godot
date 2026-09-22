/**************************************************************************/
/*  csharp_script.cpp                                                     */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#include "csharp_script.h"

#include "godotsharp_defs.h"
#include "godotsharp_dirs.h"
#include "mono_gd/gd_mono_cache.h"
#include "signal_awaiter_utils.h"
#include "utils/macros.h"
#include "utils/naming_utils.h"
#include "utils/string_utils.h"

#ifdef GD_MONO_HOT_RELOAD
#include "managed_callable.h"
#include "utils/path_utils.h"
#endif

#ifdef DEBUG_ENABLED
#include "class_db_api_json.h"
#endif // DEBUG_ENABLED

#ifdef TOOLS_ENABLED
#include "editor/editor_internal_calls.h"
#include "editor/script_templates/templates.gen.h"
#endif

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/debugger/engine_debugger.h"
#include "core/debugger/script_debugger.h"
#include "core/io/file_access.h"
#include "core/object/class_db.h"
#include "core/os/mutex.h"
#include "core/os/os.h"
#include "core/os/thread.h"
#include "servers/text/text_server.h"

#ifdef TOOLS_ENABLED
#include "core/os/keyboard.h"
#include "editor/editor_node.h"
#include "editor/file_system/editor_file_system.h"
#include "editor/settings/editor_settings.h"

#ifdef GD_MONO_HOT_RELOAD
#include "editor/docks/inspector_dock.h"
#include "editor/docks/signals_dock.h"
#endif
#endif

const Vector<String> ignored_types = {};

CSharpLanguage *CSharpLanguage::singleton = nullptr;

GDExtensionInstanceBindingCallbacks CSharpLanguage::_instance_binding_callbacks = {
	&_instance_binding_create_callback,
	&_instance_binding_free_callback,
	&_instance_binding_reference_callback
};

String CSharpLanguage::get_name() const {
	return "C#";
}

String CSharpLanguage::get_type() const {
	return "CSharpScript";
}

String CSharpLanguage::get_extension() const {
	return "cs";
}

void CSharpLanguage::init() {
#ifdef TOOLS_ENABLED
	if (OS::get_singleton()->get_cmdline_args().find("--generate-mono-glue")) {
		print_verbose(".NET: Skipping runtime initialization because glue generation is enabled.");
		return;
	}
#endif
#ifdef DEBUG_ENABLED
	if (OS::get_singleton()->get_cmdline_args().find("--class-db-json")) {
		class_db_api_to_json("user://class_db_api.json", ClassDB::API_CORE);
#ifdef TOOLS_ENABLED
		class_db_api_to_json("user://class_db_api_editor.json", ClassDB::API_EDITOR);
#endif
	}
#endif // DEBUG_ENABLED

	GLOBAL_DEF("dotnet/project/assembly_name", "");
#ifdef TOOLS_ENABLED
	GLOBAL_DEF("dotnet/project/solution_directory", "");
	GLOBAL_DEF(PropertyInfo(Variant::INT, "dotnet/project/assembly_reload_attempts", PROPERTY_HINT_RANGE, "1,16,1,or_greater"), 3);
#endif

#ifdef TOOLS_ENABLED
	EditorNode::add_init_callback(&_editor_init_callback);
#endif

	gdmono = memnew(GDMono);

	if (gdmono->should_initialize()) {
		gdmono->initialize();
	}
}

void CSharpLanguage::finish() {
	finalize();
}

void CSharpLanguage::finalize() {
	if (finalized) {
		return;
	}

	if (gdmono && gdmono->is_runtime_initialized() && GDMonoCache::godot_api_cache_updated && GDMonoCache::managed_callbacks.DisposablesTracker_OnGodotShuttingDown) {
		GDMonoCache::managed_callbacks.DisposablesTracker_OnGodotShuttingDown();
	}

	finalizing = true;

	for (KeyValue<Object *, CSharpScriptBinding> &E : script_bindings) {
		CSharpScriptBinding &script_binding = E.value;

		if (!script_binding.gchandle.is_released()) {
			script_binding.gchandle.release();
			script_binding.inited = false;
		}

		script_binding.owner->free_instance_binding(this);
	}

	if (gdmono) {
		memdelete(gdmono);
		gdmono = nullptr;
	}

	script_bindings.clear();

#ifdef DEBUG_ENABLED
	for (const KeyValue<ObjectID, int> &E : unsafe_object_references) {
		const ObjectID &id = E.key;
		Object *obj = ObjectDB::get_instance(id);

		if (obj) {
			ERR_PRINT("Leaked unsafe reference to object: " + obj->to_string());
		} else {
			ERR_PRINT("Leaked unsafe reference to deleted object: " + itos(id));
		}
	}
#endif // DEBUG_ENABLED

	memdelete(managed_callable_middleman);

	finalizing = false;
	finalized = true;
}

Vector<String> CSharpLanguage::get_reserved_words() const {
	static const Vector<String> ret = {
		"abstract", "as", "base", "bool", "break", "byte", "case", "catch", "char", "checked", "class",
		"const", "continue", "decimal", "default", "delegate", "do", "double", "else", "enum", "event",
		"explicit", "extern", "false", "finally", "fixed", "float", "for", "foreach", "goto", "if",
		"implicit", "in", "int", "interface", "internal", "is", "lock", "long", "namespace", "new",
		"null", "object", "operator", "out", "override", "params", "private", "protected", "public",
		"readonly", "ref", "return", "sbyte", "sealed", "short", "sizeof", "stackalloc", "static",
		"string", "struct", "switch", "this", "throw", "true", "try", "typeof", "uint", "ulong",
		"unchecked", "unsafe", "ushort", "using", "virtual", "void", "volatile", "while",
		"add", "alias", "ascending", "async", "await", "by", "descending", "dynamic", "equals",
		"from", "get", "global", "group", "into", "join", "let", "nameof", "on", "orderby",
		"partial", "remove", "select", "set", "value", "var", "when", "where", "yield"
	};
	return ret;
}

bool CSharpLanguage::is_control_flow_keyword(const String &p_keyword) const {
	return p_keyword == "break" || p_keyword == "case" || p_keyword == "catch" ||
			p_keyword == "continue" || p_keyword == "default" || p_keyword == "do" ||
			p_keyword == "else" || p_keyword == "finally" || p_keyword == "for" ||
			p_keyword == "foreach" || p_keyword == "goto" || p_keyword == "if" ||
			p_keyword == "return" || p_keyword == "switch" || p_keyword == "throw" ||
			p_keyword == "try" || p_keyword == "while";
}

Vector<String> CSharpLanguage::get_comment_delimiters() const {
	static const Vector<String> delimiters = { "//", "/* */" };
	return delimiters;
}

Vector<String> CSharpLanguage::get_doc_comment_delimiters() const {
	static const Vector<String> delimiters = { "///", "/** */" };
	return delimiters;
}

Vector<String> CSharpLanguage::get_string_delimiters() const {
	static const Vector<String> delimiters = { "' '", "\" \"", "@\" \"" };
	return delimiters;
}

static String get_base_class_name(const String &p_base_class_name, const String p_class_name) {
	String base_class = pascal_to_pascal_case(p_base_class_name);
	if (p_class_name == base_class) {
		base_class = "Godot." + base_class;
	}
	return base_class;
}

bool CSharpLanguage::is_using_templates() {
	return true;
}

Ref<Script> CSharpLanguage::make_template(const String &p_template, const String &p_class_name, const String &p_base_class_name) const {
	Ref<CSharpScript> scr;
	scr.instantiate();

	String class_name_no_spaces = p_class_name.replace_char(' ', '_');
	String base_class_name = get_base_class_name(p_base_class_name, class_name_no_spaces);
	String processed_template = p_template;
	processed_template = processed_template.replace("_BINDINGS_NAMESPACE_", BINDINGS_NAMESPACE)
								 .replace("_BASE_", base_class_name)
								 .replace("_CLASS_", class_name_no_spaces)
								 .replace("_TS_", _get_indentation());
	scr->set_source_code(processed_template);
	return scr;
}

Vector<ScriptLanguage::ScriptTemplate> CSharpLanguage::get_built_in_templates(const StringName &p_object) {
	Vector<ScriptLanguage::ScriptTemplate> templates;
#ifdef TOOLS_ENABLED
	for (int i = 0; i < TEMPLATES_ARRAY_SIZE; i++) {
		if (TEMPLATES[i].inherit == p_object) {
			templates.append(TEMPLATES[i]);
		}
	}
#endif
	return templates;
}

String CSharpLanguage::validate_path(const String &p_path) const {
	String class_name = p_path.get_file().get_basename();
	if (get_reserved_words().has(class_name)) {
		return RTR("Class name can't be a reserved keyword");
	}
	if (!TS->is_valid_identifier(class_name)) {
		return RTR("Class name must be a valid identifier");
	}
	return "";
}

bool CSharpLanguage::supports_builtin_mode() const {
	return false;
}

ScriptLanguage::ScriptNameCasing CSharpLanguage::preferred_file_name_casing() const {
	return SCRIPT_NAME_CASING_PASCAL_CASE;
}

#ifdef TOOLS_ENABLED
String CSharpLanguage::make_function(const String &, const String &p_name, const PackedStringArray &p_args) const {
	return String();
}
#else
String CSharpLanguage::make_function(const String &, const String &, const PackedStringArray &) const {
	return String();
}
#endif

String CSharpLanguage::_get_indentation() const {
#ifdef TOOLS_ENABLED
	if (Engine::get_singleton()->is_editor_hint()) {
		bool use_space_indentation = EDITOR_GET("text_editor/behavior/indent/type");
		if (use_space_indentation) {
			int indent_size = EDITOR_GET("text_editor/behavior/indent/size");
			return String(" ").repeat(indent_size);
		}
	}
#endif
	return "\t";
}

bool CSharpLanguage::handles_global_class_type(const String &p_type) const {
	return p_type == get_type();
}

String CSharpLanguage::get_global_class_name(const String &p_path, String *r_base_type, String *r_icon_path, bool *r_is_abstract, bool *r_is_tool) const {
	String class_name;
	// Safe Guard: Huwag tawagin kung hindi pa na-initialize ang runtime para maiwasan ang SIGSEGV
	if (!gdmono || !gdmono->is_runtime_initialized() || !GDMonoCache::godot_api_cache_updated || !GDMonoCache::managed_callbacks.ScriptManagerBridge_GetGlobalClassName) {
		return class_name;
	}
	GDMonoCache::managed_callbacks.ScriptManagerBridge_GetGlobalClassName(&p_path, r_base_type, r_icon_path, r_is_abstract, r_is_tool, &class_name);
	return class_name;
}

String CSharpLanguage::debug_get_error() const {
	return _debug_error;
}

int CSharpLanguage::debug_get_stack_level_count() const {
	return 1;
}

int CSharpLanguage::debug_get_stack_level_line(int p_level) const {
	if (_debug_parse_err_line >= 0) {
		return _debug_parse_err_line;
	}
	return 1;
}

String CSharpLanguage::debug_get_stack_level_function(int p_level) const {
	return String();
}

String CSharpLanguage::debug_get_stack_level_source(int p_level) const {
	if (_debug_parse_err_line >= 0) {
		return _debug_parse_err_file;
	}
	return String();
}

Vector<ScriptLanguage::StackInfo> CSharpLanguage::debug_get_current_stack_info() {
	static thread_local bool _recursion_flag_ = false;
	if (_recursion_flag_) {
		return Vector<StackInfo>();
	}
	_recursion_flag_ = true;
	SCOPE_EXIT {
		_recursion_flag_ = false;
	};

	if (!gdmono || !gdmono->is_runtime_initialized()) {
		return Vector<StackInfo>();
	}

	Vector<StackInfo> si;
	if (GDMonoCache::godot_api_cache_updated && GDMonoCache::managed_callbacks.DebuggingUtils_GetCurrentStackInfo) {
		GDMonoCache::managed_callbacks.DebuggingUtils_GetCurrentStackInfo(&si);
	}

	return si;
}

void CSharpLanguage::post_unsafe_reference(Object *p_obj) {
#ifdef DEBUG_ENABLED
	MutexLock lock(unsafe_object_references_lock);
	ObjectID id = p_obj->get_instance_id();
	unsafe_object_references[id]++;
#endif // DEBUG_ENABLED
}

void CSharpLanguage::pre_unsafe_unreference(Object *p_obj) {
#ifdef DEBUG_ENABLED
	MutexLock lock(unsafe_object_references_lock);
	ObjectID id = p_obj->get_instance_id();
	HashMap<ObjectID, int>::Iterator elem = unsafe_object_references.find(id);
	ERR_FAIL_NULL(elem);
	if (--elem->value == 0) {
		unsafe_object_references.remove(elem);
	}
#endif // DEBUG_ENABLED
}

void CSharpLanguage::frame() {
	if (gdmono && gdmono->is_runtime_initialized() && GDMonoCache::godot_api_cache_updated && GDMonoCache::managed_callbacks.ScriptManagerBridge_FrameCallback) {
		GDMonoCache::managed_callbacks.ScriptManagerBridge_FrameCallback();
	}
}

struct CSharpScriptDepSort {
	bool operator()(const Ref<CSharpScript> &A, const Ref<CSharpScript> &B) const {
		if (A == B) {
			return false;
		}
		const Script *I = B->get_base_script().ptr();
		while (I) {
			if (I == A.ptr()) {
				return true;
			}
			I = I->get_base_script().ptr();
		}
		return false;
	}
};

void CSharpLanguage::reload_all_scripts() {
#ifdef GD_MONO_HOT_RELOAD
	if (is_assembly_reloading_needed()) {
		reload_assemblies();
	}
#endif
}

void CSharpLanguage::reload_scripts(const Array &p_scripts) {
#ifdef GD_MONO_HOT_RELOAD
	if (is_assembly_reloading_needed()) {
		reload_assemblies();
	}
#endif
}

void CSharpLanguage::reload_tool_script(const Ref<Script> &p_script) {
	CRASH_COND(!Engine::get_singleton()->is_editor_hint());

#ifdef TOOLS_ENABLED
	if (get_godotsharp_editor() && get_godotsharp_editor()->has_node(NodePath("HotReloadAssemblyWatcher"))) {
		get_godotsharp_editor()->get_node(NodePath("HotReloadAssemblyWatcher"))->call("RestartTimer");
	}
#endif

#ifdef GD_MONO_HOT_RELOAD
	if (is_assembly_reloading_needed()) {
		reload_assemblies();
	}
#endif
}

#ifdef GD_MONO_HOT_RELOAD
bool CSharpLanguage::is_assembly_reloading_needed() {
	ERR_FAIL_NULL_V(gdmono, false);
	if (!gdmono->is_runtime_initialized()) {
		return false;
	}

	String assembly_path = gdmono->get_project_assembly_path();

	if (!assembly_path.is_empty()) {
		if (!FileAccess::exists(assembly_path)) {
			return false;
		}

		if (FileAccess::get_modified_time(assembly_path) <= gdmono->get_project_assembly_modified_time()) {
			return false;
		}
	} else {
		String assembly_name = Path::get_csharp_project_name();
		assembly_path = GodotSharpDirs::get_res_temp_assemblies_dir().path_join(assembly_name + ".dll");
		assembly_path = ProjectSettings::get_singleton()->globalize_path(assembly_path);

#if defined(ANDROID_ENABLED)
		// Inayos: Naka-wrap na sa String(...) para walang build error
		if (!FileAccess::exists(assembly_path)) {
			String ext_path = String("/storage/emulated/0/mono/assemblies").path_join(assembly_name + ".dll");
			if (FileAccess::exists(ext_path)) {
				assembly_path = ext_path;
			} else if (FileAccess::exists(String("/storage/emulated/0/mono").path_join(assembly_name + ".dll"))) {
				assembly_path = String("/storage/emulated/0/mono").path_join(assembly_name + ".dll");
			}
		}
#endif

		if (!FileAccess::exists(assembly_path)) {
			return false;
		}
	}

	return true;
}

void CSharpLanguage::reload_assemblies() {
	ERR_FAIL_NULL(gdmono);
	if (!gdmono->is_runtime_initialized() || !GDMonoCache::godot_api_cache_updated) {
		return;
	}

	if (!Engine::get_singleton()->is_editor_hint()) {
		return;
	}

	print_verbose(".NET: Reloading assemblies...");

	List<Ref<CSharpScript>> scripts;

	{
		MutexLock lock(script_instances_mutex);

		for (CSharpScript &script : script_list) {
			bool is_reloadable = script.instances.is_empty();
			for (Object *obj : script.instances) {
				ERR_CONTINUE(!obj->get_script_instance());
				CSharpInstance *csi = static_cast<CSharpInstance *>(obj->get_script_instance());
				if (GDMonoCache::managed_callbacks.GCHandleBridge_GCHandleIsTargetCollectible(csi->get_gchandle_intptr())) {
					is_reloadable = true;
					break;
				}
			}
			if (is_reloadable) {
				scripts.push_back(Ref<CSharpScript>(&script));
			}
		}
	}

	scripts.sort_custom<CSharpScriptDepSort>();

	{
		MutexLock lock(ManagedCallable::instances_mutex);

		for (ManagedCallable &managed_callable : ManagedCallable::instances) {
			ERR_CONTINUE(managed_callable.delegate_handle.value == nullptr);

			if (!GDMonoCache::managed_callbacks.GCHandleBridge_GCHandleIsTargetCollectible(managed_callable.delegate_handle)) {
				continue;
			}

			Array serialized_data;
			bool success = GDMonoCache::managed_callbacks.DelegateUtils_TrySerializeDelegateWithGCHandle(
					managed_callable.delegate_handle, &serialized_data);

			if (success) {
				ManagedCallable::instances_pending_reload.insert(&managed_callable, serialized_data);
			} else {
				if (OS::get_singleton()->is_stdout_verbose()) {
					OS::get_singleton()->print("Failed to serialize delegate.\n");
				}
				managed_callable.release_delegate_handle();
			}
		}
	}

	List<Ref<CSharpScript>> to_reload;
	List<Ref<RefCounted>> rc_instances;

	for (const KeyValue<Object *, CSharpScriptBinding> &E : script_bindings) {
		const CSharpScriptBinding &script_binding = E.value;
		RefCounted *rc = Object::cast_to<RefCounted>(script_binding.owner);
		if (rc) {
			rc_instances.push_back(Ref<RefCounted>(rc));
		}
	}

	for (Ref<CSharpScript> &scr : scripts) {
		if (scr->get_path().is_empty() && !scr->valid) {
			continue;
		}

		to_reload.push_back(scr);

		for (Object *obj : scr->instances) {
			scr->pending_reload_instances.insert(obj->get_instance_id());
			scr->pending_replace_placeholders.insert(obj->get_instance_id());

			RefCounted *rc = Object::cast_to<RefCounted>(obj);
			if (rc) {
				rc_instances.push_back(Ref<RefCounted>(rc));
			}
		}

#ifdef TOOLS_ENABLED
		for (PlaceHolderScriptInstance *instance : scr->placeholders) {
			Object *obj = instance->get_owner();
			scr->pending_reload_instances.insert(obj->get_instance_id());

			RefCounted *rc = Object::cast_to<RefCounted>(obj);
			if (rc) {
				rc_instances.push_back(Ref<RefCounted>(rc));
			}
		}
#endif

		RBMap<ObjectID, CSharpScript::StateBackup> &owners_map = scr->pending_reload_state;

		for (Object *obj : scr->instances) {
			ERR_CONTINUE(!obj->get_script_instance());

			CSharpInstance *csi = static_cast<CSharpInstance *>(obj->get_script_instance());
			CSharpScript::StateBackup state;
			Dictionary properties;

			GDMonoCache::managed_callbacks.CSharpInstanceBridge_SerializeState(
					csi->get_gchandle_intptr(), &properties, &state.event_signals);

			for (const Variant *s = properties.next(nullptr); s != nullptr; s = properties.next(s)) {
				StringName name = *s;
				Variant value = properties[*s];
				state.properties.push_back(Pair<StringName, Variant>(name, value));
			}

			owners_map[obj->get_instance_id()] = state;
		}
	}

	for (Ref<CSharpScript> &scr : scripts) {
		while (scr->instances.begin()) {
			Object *obj = *scr->instances.begin();
			obj->set_script(Ref<RefCounted>());
		}

		scr->was_tool_before_reload = scr->type_info.is_tool;
		scr->_clear();
	}

	{
		MutexLock lock(ManagedCallable::instances_mutex);

		for (KeyValue<ManagedCallable *, Array> &kv : ManagedCallable::instances_pending_reload) {
			kv.key->release_delegate_handle();
		}
	}

	if (gdmono->reload_project_assemblies() != OK) {
		for (Ref<CSharpScript> &scr : to_reload) {
			for (const KeyValue<ObjectID, CSharpScript::StateBackup> &F : scr->pending_reload_state) {
				Object *obj = ObjectDB::get_instance(F.key);
				if (!obj) {
					continue;
				}

				ObjectID obj_id = obj->get_instance_id();
				PlaceHolderScriptInstance *placeholder = scr->placeholder_instance_create(obj);
				obj->set_script_instance(placeholder);

#ifdef TOOLS_ENABLED
				scr->placeholder_fallback_enabled = true;
#endif

				for (const Pair<StringName, Variant> &G : scr->pending_reload_state[obj_id].properties) {
					placeholder->property_set_fallback(G.first, G.second, nullptr);
				}

				scr->pending_reload_state.erase(obj_id);
			}

			scr->pending_reload_instances.clear();
			scr->pending_reload_state.clear();
		}
		return;
	}

	for (Ref<CSharpScript> &scr : to_reload) {
		if (!scr->get_path().is_empty() && !scr->get_path().begins_with("csharp://")) {
			String script_path = scr->get_path();

			bool valid = GDMonoCache::managed_callbacks.ScriptManagerBridge_AddScriptBridge(scr.ptr(), &script_path);
			if (valid) {
				scr->valid = true;
				CSharpScript::update_script_class_info(scr);
				scr->reload_invalidated = true;
			}
		}
	}

	List<Ref<CSharpScript>> to_reload_state;

	for (Ref<CSharpScript> &scr : to_reload) {
#ifdef TOOLS_ENABLED
		scr->exports_invalidated = true;
#endif

		if (!scr->get_path().is_empty() && !scr->get_path().begins_with("csharp://")) {
			scr->reload();

			if (!scr->valid) {
				scr->pending_reload_instances.clear();
				scr->pending_reload_state.clear();
				continue;
			}
		} else {
			bool success = GDMonoCache::managed_callbacks.ScriptManagerBridge_TryReloadRegisteredScriptWithClass(scr.ptr());
			if (!success) {
				scr->pending_reload_instances.clear();
				scr->pending_reload_state.clear();
				continue;
			}
		}

		StringName native_name = scr->get_instance_base_type();

		{
			for (const ObjectID &obj_id : scr->pending_reload_instances) {
				Object *obj = ObjectDB::get_instance(obj_id);

				if (!obj) {
					scr->pending_reload_state.erase(obj_id);
					continue;
				}

				if (!obj->is_class(native_name)) {
					scr->pending_reload_state.erase(obj_id);
					continue;
				}

				ScriptInstance *si = obj->get_script_instance();
				bool replace_placeholder = scr->pending_replace_placeholders.has(obj->get_instance_id());
				if (!scr->is_tool() && scr->was_tool_before_reload) {
					replace_placeholder = false;
					scr->pending_replace_placeholders.erase(obj->get_instance_id());
				}

#ifdef TOOLS_ENABLED
				if (si) {
					CRASH_COND(!si->is_placeholder());

					if (replace_placeholder || scr->is_tool() || ScriptServer::is_scripting_enabled()) {
						CSharpScript::StateBackup &state_backup = scr->pending_reload_state[obj_id];
						si->get_property_state(state_backup.properties);

						ScriptInstance *instance = scr->instance_create(obj);
						if (instance) {
							scr->placeholders.erase(static_cast<PlaceHolderScriptInstance *>(si));
							scr->pending_replace_placeholders.erase(obj->get_instance_id());
							obj->set_script_instance(instance);
						}
					}
					continue;
				}
#else
				CRASH_COND(si != nullptr);
#endif

				if (replace_placeholder || scr->is_tool() || ScriptServer::is_scripting_enabled()) {
					ScriptInstance *instance = scr->instance_create(obj);
					if (instance) {
						scr->pending_replace_placeholders.erase(obj->get_instance_id());
						obj->set_script_instance(instance);
						continue;
					}
				}
				obj->set_script(scr);
			}
		}

		to_reload_state.push_back(scr);
	}

	{
		MutexLock lock(ManagedCallable::instances_mutex);

		for (const KeyValue<ManagedCallable *, Array> &elem : ManagedCallable::instances_pending_reload) {
			ManagedCallable *managed_callable = elem.key;
			const Array &serialized_data = elem.value;

			GCHandleIntPtr delegate = { nullptr };
			bool success = GDMonoCache::managed_callbacks.DelegateUtils_TryDeserializeDelegateWithGCHandle(
					&serialized_data, &delegate);

			if (success) {
				ERR_CONTINUE(delegate.value == nullptr);
				managed_callable->delegate_handle = delegate;
			} else if (OS::get_singleton()->is_stdout_verbose()) {
				OS::get_singleton()->print("Failed to deserialize delegate\n");
			}
		}

		ManagedCallable::instances_pending_reload.clear();
	}

	for (Ref<CSharpScript> &scr : to_reload_state) {
		for (const ObjectID &obj_id : scr->pending_reload_instances) {
			Object *obj = ObjectDB::get_instance(obj_id);
			if (!obj) {
				scr->pending_reload_state.erase(obj_id);
				continue;
			}

			ERR_CONTINUE(!obj->get_script_instance());

			CSharpScript::StateBackup &state_backup = scr->pending_reload_state[obj_id];
			CSharpInstance *csi = CAST_CSHARP_INSTANCE(obj->get_script_instance());

			if (csi) {
				Dictionary properties;
				for (const Pair<StringName, Variant> &G : state_backup.properties) {
					properties[G.first] = G.second;
				}

				GDMonoCache::managed_callbacks.CSharpInstanceBridge_DeserializeState(
						csi->get_gchandle_intptr(), &properties, &state_backup.event_signals);
			}
		}

		scr->pending_reload_instances.clear();
		scr->pending_reload_state.clear();
	}

#ifdef TOOLS_ENABLED
	if (Engine::get_singleton()->is_editor_hint()) {
		InspectorDock::get_inspector_singleton()->update_tree();
		SignalsDock::get_singleton()->update_lists();
	}
#endif
}
#endif

#ifdef TOOLS_ENABLED
Error CSharpLanguage::open_in_external_editor(const Ref<Script> &p_script, int p_line, int p_col) {
	if (!get_godotsharp_editor()) {
		return ERR_UNCONFIGURED;
	}
	return (Error)(int)get_godotsharp_editor()->call("OpenInExternalEditor", p_script, p_line, p_col);
}

bool CSharpLanguage::overrides_external_editor() {
	if (!get_godotsharp_editor()) {
		return false;
	}
	return get_godotsharp_editor()->call("OverridesExternalEditor");
}
#endif

bool CSharpLanguage::debug_break_parse(const String &p_file, int p_line, const String &p_error) {
	if (EngineDebugger::is_active() && Thread::get_caller_id() == Thread::get_main_id()) {
		_debug_parse_err_line = p_line;
		_debug_parse_err_file = p_file;
		_debug_error = p_error;
		EngineDebugger::get_script_debugger()->debug(this, false, true);
		return true;
	}
	return false;
}

bool CSharpLanguage::debug_break(const String &p_error, bool p_allow_continue) {
	if (EngineDebugger::is_active() && Thread::get_caller_id() == Thread::get_main_id()) {
		_debug_parse_err_line = -1;
		_debug_parse_err_file = "";
		_debug_error = p_error;
		EngineDebugger::get_script_debugger()->debug(this, p_allow_continue);
		return true;
	}
	return false;
}

#ifdef TOOLS_ENABLED
void CSharpLanguage::_editor_init_callback() {
	// Safe Guard: Siguraduhing initialized ang GDMono at may valid na LoadToolsAssemblyCallback bago tumawag
	if (!GDMono::get_singleton() || !GDMono::get_singleton()->is_runtime_initialized() || GDMono::get_singleton()->get_plugin_callbacks().LoadToolsAssemblyCallback == nullptr) {
		print_verbose(".NET: Runtime not initialized or LoadToolsAssemblyCallback missing, skipping GodotTools plugin loading.");
		return;
	}

	int32_t interop_funcs_size = 0;
	const void **interop_funcs = godotsharp::get_editor_interop_funcs(interop_funcs_size);
	String tools_path = GodotSharpDirs::get_data_editor_tools_dir().path_join("GodotTools.dll");

#if defined(ANDROID_ENABLED)
	if (!FileAccess::exists(tools_path)) {
		if (FileAccess::exists("/storage/emulated/0/mono/Tools/GodotTools.dll")) {
			tools_path = "/storage/emulated/0/mono/Tools/GodotTools.dll";
		} else if (FileAccess::exists("/storage/emulated/0/mono/GodotTools.dll")) {
			tools_path = "/storage/emulated/0/mono/GodotTools.dll";
		}
	}
#endif

	print_verbose(".NET: Loading GodotTools assembly from: " + tools_path);

	Object *editor_plugin_obj = GDMono::get_singleton()->get_plugin_callbacks().LoadToolsAssemblyCallback(
			tools_path.utf16().get_data(),
			interop_funcs, interop_funcs_size);

	if (editor_plugin_obj == nullptr) {
		ERR_PRINT(".NET: Failed to load GodotTools.dll assembly.");
		return;
	}

	EditorPlugin *godotsharp_editor = Object::cast_to<EditorPlugin>(editor_plugin_obj);
	if (godotsharp_editor == nullptr) {
		ERR_PRINT(".NET: GodotTools is not a valid EditorPlugin.");
		return;
	}

	EditorNode::add_editor_plugin(godotsharp_editor);
	godotsharp_editor->enable_plugin();

	get_singleton()->godotsharp_editor = godotsharp_editor;
}
#endif

void CSharpLanguage::set_language_index(int p_idx) {
	ERR_FAIL_COND(lang_idx != -1);
	lang_idx = p_idx;
}

void CSharpLanguage::release_script_gchandle(MonoGCHandleData &p_gchandle) {
	if (!p_gchandle.is_released()) {
		MutexLock lock(get_singleton()->script_gchandle_release_mutex);
		p_gchandle.release();
	}
}

void CSharpLanguage::release_script_gchandle_thread_safe(GCHandleIntPtr p_gchandle_to_free, MonoGCHandleData &r_gchandle) {
	if (!r_gchandle.is_released() && r_gchandle.get_intptr() == p_gchandle_to_free) {
		MutexLock lock(get_singleton()->script_gchandle_release_mutex);
		if (!r_gchandle.is_released() && r_gchandle.get_intptr() == p_gchandle_to_free) {
			r_gchandle.release();
		}
	}
}

void CSharpLanguage::release_binding_gchandle_thread_safe(GCHandleIntPtr p_gchandle_to_free, CSharpScriptBinding &r_script_binding) {
	MonoGCHandleData &gchandle = r_script_binding.gchandle;
	if (!gchandle.is_released() && gchandle.get_intptr() == p_gchandle_to_free) {
		MutexLock lock(get_singleton()->script_gchandle_release_mutex);
		if (!gchandle.is_released() && gchandle.get_intptr() == p_gchandle_to_free) {
			gchandle.release();
			r_script_binding.inited = false;
		}
	}
}

CSharpLanguage::CSharpLanguage() {
	ERR_FAIL_COND_MSG(singleton, "C# singleton already exists.");
	singleton = this;
}

CSharpLanguage::~CSharpLanguage() {
	finalize();
	singleton = nullptr;
}

bool CSharpLanguage::setup_csharp_script_binding(CSharpScriptBinding &r_script_binding, Object *p_object) {
	if (!GDMonoCache::godot_api_cache_updated || !GDMonoCache::managed_callbacks.ScriptManagerBridge_CreateManagedForGodotObjectBinding) {
		return false;
	}

	StringName type_name = p_object->get_class_name();
	const ClassDB::ClassInfo *classinfo = ClassDB::classes.getptr(type_name);

	while (classinfo && (!classinfo->exposed || classinfo->gdextension || ignored_types.has(classinfo->gdtype->get_name()))) {
		classinfo = classinfo->inherits_ptr;
	}

	ERR_FAIL_NULL_V(classinfo, false);
	type_name = classinfo->gdtype->get_name();

	bool parent_is_object_class = p_object->is_class(type_name);
	ERR_FAIL_COND_V_MSG(!parent_is_object_class, false,
			"Type inherits from native type '" + type_name + "', so it can't be instantiated in object of type: '" + p_object->get_class() + "'.");

	GCHandleIntPtr strong_gchandle =
			GDMonoCache::managed_callbacks.ScriptManagerBridge_CreateManagedForGodotObjectBinding(
					&type_name, p_object);

	ERR_FAIL_NULL_V(strong_gchandle.value, false);

	r_script_binding.inited = true;
	r_script_binding.type_name = type_name;
	r_script_binding.gchandle = MonoGCHandleData(strong_gchandle, gdmono::GCHandleType::STRONG_HANDLE);
	r_script_binding.owner = p_object;

	RefCounted *rc = Object::cast_to<RefCounted>(p_object);
	if (rc) {
		rc->reference();
		CSharpLanguage::get_singleton()->post_unsafe_reference(rc);
	}

	return true;
}

RBMap<Object *, CSharpScriptBinding>::Element *CSharpLanguage::insert_script_binding(Object *p_object, const CSharpScriptBinding &p_script_binding) {
	return script_bindings.insert(p_object, p_script_binding);
}

void *CSharpLanguage::_instance_binding_create_callback(void *, void *p_instance) {
	CSharpLanguage *csharp_lang = CSharpLanguage::get_singleton();
	MutexLock lock(csharp_lang->language_bind_mutex);

	RBMap<Object *, CSharpScriptBinding>::Element *match = csharp_lang->script_bindings.find((Object *)p_instance);
	if (match) {
		return (void *)match;
	}

	CSharpScriptBinding script_binding;
	return (void *)csharp_lang->insert_script_binding((Object *)p_instance, script_binding);
}

void CSharpLanguage::_instance_binding_free_callback(void *, void *, void *p_binding) {
	CSharpLanguage *csharp_lang = CSharpLanguage::get_singleton();

	if (GDMono::get_singleton() == nullptr) {
		return;
	}

	if (csharp_lang->finalizing) {
		return;
	}

	{
		MutexLock lock(csharp_lang->language_bind_mutex);

		RBMap<Object *, CSharpScriptBinding>::Element *data = (RBMap<Object *, CSharpScriptBinding>::Element *)p_binding;
		CSharpScriptBinding &script_binding = data->value();

		if (script_binding.inited) {
			if (GDMonoCache::godot_api_cache_updated && GDMonoCache::managed_callbacks.ScriptManagerBridge_SetGodotObjectPtr) {
				GDMonoCache::managed_callbacks.ScriptManagerBridge_SetGodotObjectPtr(
						script_binding.gchandle.get_intptr(), nullptr);
			}
			script_binding.gchandle.release();
			script_binding.inited = false;
		}

		csharp_lang->script_bindings.erase(data);
	}
}

GDExtensionBool CSharpLanguage::_instance_binding_reference_callback(void *p_token, void *p_binding, GDExtensionBool p_reference) {
	DEV_ASSERT(CSharpLanguage::get_singleton() != nullptr);
	CRASH_COND(!p_binding);

	CSharpScriptBinding &script_binding = ((RBMap<Object *, CSharpScriptBinding>::Element *)p_binding)->get();
	RefCounted *rc_owner = Object::cast_to<RefCounted>(script_binding.owner);

	MonoGCHandleData &gchandle = script_binding.gchandle;
	int refcount = rc_owner->get_reference_count();

	if (!script_binding.inited) {
		return refcount == 0;
	}

	if (p_reference) {
		if (refcount > 1 && gchandle.is_weak()) {
			GCHandleIntPtr old_gchandle = gchandle.get_intptr();
			gchandle.handle = { nullptr };

			GCHandleIntPtr new_gchandle = { nullptr };
			bool create_weak = false;
			bool target_alive = GDMonoCache::managed_callbacks.ScriptManagerBridge_SwapGCHandleForType(
					old_gchandle, &new_gchandle, create_weak);

			if (!target_alive) {
				return false;
			}

			gchandle = MonoGCHandleData(new_gchandle, gdmono::GCHandleType::STRONG_HANDLE);
		}
		return false;
	} else {
		if (refcount == 1 && !gchandle.is_released() && !gchandle.is_weak()) {
			GCHandleIntPtr old_gchandle = gchandle.get_intptr();
			gchandle.handle = { nullptr };

			GCHandleIntPtr new_gchandle = { nullptr };
			bool create_weak = true;
			bool target_alive = GDMonoCache::managed_callbacks.ScriptManagerBridge_SwapGCHandleForType(
					old_gchandle, &new_gchandle, create_weak);

			if (!target_alive) {
				return refcount == 0;
			}

			gchandle = MonoGCHandleData(new_gchandle, gdmono::GCHandleType::WEAK_HANDLE);
			return false;
		}
		return refcount == 0;
	}
}

void *CSharpLanguage::get_instance_binding(Object *p_object) {
	return p_object->get_instance_binding(get_singleton(), &_instance_binding_callbacks);
}

void *CSharpLanguage::get_instance_binding_with_setup(Object *p_object) {
	void *binding = get_instance_binding(p_object);
	if (binding) {
		CSharpScriptBinding &script_binding = ((RBMap<Object *, CSharpScriptBinding>::Element *)binding)->value();
		if (!script_binding.inited) {
			MutexLock lock(CSharpLanguage::get_singleton()->get_language_bind_mutex());
			if (!script_binding.inited) {
				CSharpLanguage::get_singleton()->setup_csharp_script_binding(script_binding, p_object);
			}
		}
	}
	return binding;
}

void *CSharpLanguage::get_existing_instance_binding(Object *p_object) {
	return get_instance_binding(p_object);
}

bool CSharpLanguage::has_instance_binding(Object *p_object) {
	return p_object->has_instance_binding(get_singleton());
}

void CSharpLanguage::tie_native_managed_to_unmanaged(GCHandleIntPtr p_gchandle_intptr, Object *p_unmanaged, const StringName *p_native_name, bool p_ref_counted) {
	CRASH_COND(!p_unmanaged);

	RefCounted *rc = Object::cast_to<RefCounted>(p_unmanaged);
	CRASH_COND(p_ref_counted != (bool)rc);

	MonoGCHandleData gchandle = MonoGCHandleData(p_gchandle_intptr,
			p_ref_counted ? gdmono::GCHandleType::WEAK_HANDLE : gdmono::GCHandleType::STRONG_HANDLE);

	if (p_ref_counted) {
		if (rc->init_ref()) {
			CSharpLanguage::get_singleton()->post_unsafe_reference(rc);
		}
	}

	CRASH_COND(CSharpLanguage::has_instance_binding(p_unmanaged));

	void *binding = CSharpLanguage::get_singleton()->get_instance_binding(p_unmanaged);
	CSharpScriptBinding &script_binding = ((RBMap<Object *, CSharpScriptBinding>::Element *)binding)->value();
	script_binding.inited = true;
	script_binding.type_name = *p_native_name;
	script_binding.gchandle = gchandle;
	script_binding.owner = p_unmanaged;
}

void CSharpLanguage::tie_user_managed_to_unmanaged(GCHandleIntPtr p_gchandle_intptr, Object *p_unmanaged, Ref<CSharpScript> *p_script, bool p_ref_counted) {
	Ref<CSharpScript> script = *p_script;
	p_script->~Ref();

	CRASH_COND(!p_unmanaged);

	RefCounted *rc = Object::cast_to<RefCounted>(p_unmanaged);
	CRASH_COND(p_ref_counted != (bool)rc);

	MonoGCHandleData gchandle = MonoGCHandleData(p_gchandle_intptr,
			p_ref_counted ? gdmono::GCHandleType::WEAK_HANDLE : gdmono::GCHandleType::STRONG_HANDLE);

	CRASH_COND(script.is_null());

	CSharpInstance *csharp_instance = CSharpInstance::create_for_managed_type(p_unmanaged, script.ptr(), gchandle);
	p_unmanaged->set_script_instance(csharp_instance);
	csharp_instance->connect_event_signals();
}

void CSharpLanguage::tie_managed_to_unmanaged_with_pre_setup(GCHandleIntPtr p_gchandle_intptr, Object *p_unmanaged) {
	CRASH_COND(!p_unmanaged);

	CSharpInstance *instance = CAST_CSHARP_INSTANCE(p_unmanaged->get_script_instance());
	if (!instance) {
		return;
	}

	CRASH_COND(!instance->gchandle.is_released());

	instance->gchandle = MonoGCHandleData(p_gchandle_intptr, gdmono::GCHandleType::STRONG_HANDLE);

	if (instance->base_ref_counted) {
		instance->_reference_owner_unsafe();
	}

	{
		MutexLock lock(CSharpLanguage::get_singleton()->get_script_instances_mutex());
		instance->script->instances.insert(instance->owner);
	}

	instance->connect_event_signals();
}

CSharpInstance *CSharpInstance::create_for_managed_type(Object *p_owner, CSharpScript *p_script, const MonoGCHandleData &p_gchandle) {
	CSharpInstance *instance = memnew(CSharpInstance(Ref<CSharpScript>(p_script)));
	RefCounted *rc = Object::cast_to<RefCounted>(p_owner);

	instance->base_ref_counted = rc != nullptr;
	instance->owner = p_owner;
	instance->gchandle = p_gchandle;

	if (instance->base_ref_counted) {
		instance->_reference_owner_unsafe();
	}

	{
		MutexLock lock(CSharpLanguage::get_singleton()->get_script_instances_mutex());
		p_script->instances.insert(p_owner);
	}

	return instance;
}

Object *CSharpInstance::get_owner() {
	return owner;
}

bool CSharpInstance::set(const StringName &p_name, const Variant &p_value) {
	ERR_FAIL_COND_V(script.is_null(), false);
	if (!GDMonoCache::godot_api_cache_updated || !GDMonoCache::managed_callbacks.CSharpInstanceBridge_Set) {
		return false;
	}
	return GDMonoCache::managed_callbacks.CSharpInstanceBridge_Set(
			gchandle.get_intptr(), &p_name, &p_value);
}

bool CSharpInstance::get(const StringName &p_name, Variant &r_ret) const {
	ERR_FAIL_COND_V(script.is_null(), false);
	if (!GDMonoCache::godot_api_cache_updated || !GDMonoCache::managed_callbacks.CSharpInstanceBridge_Get) {
		return false;
	}

	Variant ret_value;
	bool ret = GDMonoCache::managed_callbacks.CSharpInstanceBridge_Get(
			gchandle.get_intptr(), &p_name, &ret_value);

	if (ret) {
		r_ret = ret_value;
		return true;
	}
	return false;
}

void CSharpInstance::get_property_list(List<PropertyInfo> *r_properties) const {
	List<PropertyInfo> props;
	ERR_FAIL_COND(script.is_null());
#ifdef TOOLS_ENABLED
	for (const PropertyInfo &prop : script->exported_members_cache) {
		props.push_back(prop);
	}
#else
	for (const KeyValue<StringName, PropertyInfo> &E : script->member_info) {
		props.push_front(E.value);
	}
#endif

	for (PropertyInfo &prop : props) {
		validate_property(prop);
		r_properties->push_back(prop);
	}

	if (!GDMonoCache::godot_api_cache_updated || !GDMonoCache::managed_callbacks.CSharpInstanceBridge_Call) {
		return;
	}

	StringName method = SNAME("_get_property_list");
	Variant ret;
	Callable::CallError call_error;
	bool ok = GDMonoCache::managed_callbacks.CSharpInstanceBridge_Call(
			gchandle.get_intptr(), &method, nullptr, 0, &call_error, &ret);

	if (call_error.error != Callable::CallError::CALL_ERROR_INVALID_METHOD) {
		if (call_error.error != Callable::CallError::CALL_OK) {
			ERR_PRINT("Error calling '_get_property_list': " + Variant::get_call_error_text(method, nullptr, 0, call_error));
		} else if (ok) {
			Array array = ret;
			for (int i = 0, size = array.size(); i < size; i++) {
				r_properties->push_back(PropertyInfo::from_dict(array.get(i)));
			}
		}
	}

	CSharpScript *top = script.ptr()->base_script.ptr();
	while (top != nullptr) {
		props.clear();
#ifdef TOOLS_ENABLED
		for (const PropertyInfo &prop : top->exported_members_cache) {
			props.push_back(prop);
		}
#else
		for (const KeyValue<StringName, PropertyInfo> &E : top->member_info) {
			props.push_front(E.value);
		}
#endif

		for (PropertyInfo &prop : props) {
			validate_property(prop);
			r_properties->push_back(prop);
		}

		top = top->base_script.ptr();
	}
}

Variant::Type CSharpInstance::get_property_type(const StringName &p_name, bool *r_is_valid) const {
	if (script->member_info.has(p_name)) {
		if (r_is_valid) {
			*r_is_valid = true;
		}
		return script->member_info[p_name].type;
	}

	if (r_is_valid) {
		*r_is_valid = false;
	}
	return Variant::NIL;
}

bool CSharpInstance::property_can_revert(const StringName &p_name) const {
	ERR_FAIL_COND_V(script.is_null(), false);
	if (!GDMonoCache::godot_api_cache_updated || !GDMonoCache::managed_callbacks.CSharpInstanceBridge_Call) {
		return false;
	}

	Variant name_arg = p_name;
	const Variant *args[1] = { &name_arg };
	Variant ret;
	Callable::CallError call_error;
	GDMonoCache::managed_callbacks.CSharpInstanceBridge_Call(
			gchandle.get_intptr(), &SNAME("_property_can_revert"), args, 1, &call_error, &ret);

	if (call_error.error != Callable::CallError::CALL_OK) {
		return false;
	}
	return (bool)ret;
}

void CSharpInstance::validate_property(PropertyInfo &p_property) const {
	ERR_FAIL_COND(script.is_null());
	if (!GDMonoCache::godot_api_cache_updated || !GDMonoCache::managed_callbacks.CSharpInstanceBridge_Call) {
		return;
	}

	Variant property_arg = (Dictionary)p_property;
	const Variant *args[1] = { &property_arg };
	Variant ret;
	Callable::CallError call_error;
	GDMonoCache::managed_callbacks.CSharpInstanceBridge_Call(
			gchandle.get_intptr(), &SNAME("_validate_property"), args, 1, &call_error, &ret);

	if (call_error.error != Callable::CallError::CALL_OK) {
		return;
	}
	p_property = PropertyInfo::from_dict(property_arg);
}

bool CSharpInstance::property_get_revert(const StringName &p_name, Variant &r_ret) const {
	ERR_FAIL_COND_V(script.is_null(), false);
	if (!GDMonoCache::godot_api_cache_updated || !GDMonoCache::managed_callbacks.CSharpInstanceBridge_Call) {
		return false;
	}

	Variant name_arg = p_name;
	const Variant *args[1] = { &name_arg };
	Variant ret;
	Callable::CallError call_error;
	GDMonoCache::managed_callbacks.CSharpInstanceBridge_Call(
			gchandle.get_intptr(), &SNAME("_property_get_revert"), args, 1, &call_error, &ret);

	if (call_error.error != Callable::CallError::CALL_OK) {
		return false;
	}
	r_ret = ret;
	return true;
}

void CSharpInstance::get_method_list(List<MethodInfo> *r_list) const {
	if (!script->is_script_valid() || !script->valid) {
		return;
	}
	script->get_script_method_list(r_list);
}

bool CSharpInstance::has_method(const StringName &p_method) const {
	if (script.is_null() || !GDMonoCache::godot_api_cache_updated || !GDMonoCache::managed_callbacks.CSharpInstanceBridge_HasMethodUnknownParams) {
		return false;
	}
	return GDMonoCache::managed_callbacks.CSharpInstanceBridge_HasMethodUnknownParams(
			gchandle.get_intptr(), &p_method);
}

int CSharpInstance::get_method_argument_count(const StringName &p_method, bool *r_is_valid) const {
	if (!script->is_script_valid() || !script->valid) {
		if (r_is_valid) {
			*r_is_valid = false;
		}
		return 0;
	}

	const CSharpScript *top = script.ptr();
	while (top != nullptr) {
		for (const CSharpScript::CSharpMethodInfo &E : top->methods) {
			if (E.name == p_method) {
				if (r_is_valid) {
					*r_is_valid = true;
				}
				return E.method_info.arguments.size();
			}
		}
		top = top->base_script.ptr();
	}

	if (r_is_valid) {
		*r_is_valid = false;
	}
	return 0;
}

Variant CSharpInstance::callp(const StringName &p_method, const Variant **p_args, int p_argcount, Callable::CallError &r_error) {
	ERR_FAIL_COND_V(script.is_null(), Variant());
	if (!GDMonoCache::godot_api_cache_updated || !GDMonoCache::managed_callbacks.CSharpInstanceBridge_Call) {
		r_error.error = Callable::CallError::CALL_ERROR_INVALID_METHOD;
		return Variant();
	}

	Variant ret;
	GDMonoCache::managed_callbacks.CSharpInstanceBridge_Call(
			gchandle.get_intptr(), &p_method, p_args, p_argcount, &r_error, &ret);
	return ret;
}

bool CSharpInstance::_reference_owner_unsafe() {
	if (static_cast<RefCounted *>(owner)->init_ref()) {
		CSharpLanguage::get_singleton()->post_unsafe_reference(owner);
		unsafe_referenced = true;
	}
	return unsafe_referenced;
}

bool CSharpInstance::_unreference_owner_unsafe() {
	if (!unsafe_referenced) {
		return false;
	}
	unsafe_referenced = false;
	CSharpLanguage::get_singleton()->pre_unsafe_unreference(owner);
	return static_cast<RefCounted *>(owner)->unreference();
}

bool CSharpInstance::_internal_new_managed() {
	CSharpLanguage::get_singleton()->release_script_gchandle(gchandle);

	ERR_FAIL_NULL_V(owner, false);
	ERR_FAIL_COND_V(script.is_null(), false);
	ERR_FAIL_COND_V(!script->can_instantiate(), false);

	if (!GDMonoCache::godot_api_cache_updated || !GDMonoCache::managed_callbacks.ScriptManagerBridge_CreateManagedForGodotObjectScriptInstance) {
		return false;
	}

	bool ok = GDMonoCache::managed_callbacks.ScriptManagerBridge_CreateManagedForGodotObjectScriptInstance(
			script.ptr(), owner, nullptr, 0);

	if (!ok) {
		script = Ref<CSharpScript>();
		owner = nullptr;
		return false;
	}

	CRASH_COND(gchandle.is_released());
	return true;
}

void CSharpInstance::mono_object_disposed(GCHandleIntPtr p_gchandle_to_free) {
	disconnect_event_signals();
	CSharpLanguage::get_singleton()->release_script_gchandle_thread_safe(p_gchandle_to_free, gchandle);
}

void CSharpInstance::mono_object_disposed_baseref(GCHandleIntPtr p_gchandle_to_free, bool p_is_finalizer, bool &r_delete_owner, bool &r_remove_script_instance) {
	disconnect_event_signals();
	r_remove_script_instance = false;

	if (_unreference_owner_unsafe()) {
		r_delete_owner = true;
	} else {
		r_delete_owner = false;
		CSharpLanguage::get_singleton()->release_script_gchandle_thread_safe(p_gchandle_to_free, gchandle);

		if (!p_is_finalizer) {
			r_remove_script_instance = true;
		} else if (!GDMono::get_singleton()->is_finalizing_scripts_domain()) {
			if (!_internal_new_managed()) {
				r_remove_script_instance = true;
			}
		}
	}
}

void CSharpInstance::connect_event_signals() {
	const CSharpScript *top = script.ptr();
	while (top != nullptr && top->valid) {
		for (const CSharpScript::EventSignalInfo &signal : top->event_signals) {
			String signal_name = signal.name;
			EventSignalCallable *event_signal_callable = memnew(EventSignalCallable(owner, signal_name));
			Callable callable(event_signal_callable);
			connected_event_signals.push_back(callable);
			owner->connect(signal_name, callable);
		}
		top = top->base_script.ptr();
	}
}

void CSharpInstance::disconnect_event_signals() {
	for (const Callable &callable : connected_event_signals) {
		const EventSignalCallable *event_signal_callable = static_cast<const EventSignalCallable *>(callable.get_custom());
		owner->disconnect(event_signal_callable->get_signal(), callable);
	}
	connected_event_signals.clear();
}

void CSharpInstance::refcount_incremented() {
	RefCounted *rc_owner = Object::cast_to<RefCounted>(owner);
	if (rc_owner->get_reference_count() > 1 && gchandle.is_weak()) {
		GCHandleIntPtr old_gchandle = gchandle.get_intptr();
		gchandle.handle = { nullptr };

		GCHandleIntPtr new_gchandle = { nullptr };
		bool create_weak = false;
		bool target_alive = GDMonoCache::managed_callbacks.ScriptManagerBridge_SwapGCHandleForType(
				old_gchandle, &new_gchandle, create_weak);

		if (!target_alive) {
			return;
		}
		gchandle = MonoGCHandleData(new_gchandle, gdmono::GCHandleType::STRONG_HANDLE);
	}
}

bool CSharpInstance::refcount_decremented() {
	RefCounted *rc_owner = Object::cast_to<RefCounted>(owner);
	int refcount = rc_owner->get_reference_count();

	if (refcount == 1 && !gchandle.is_released() && !gchandle.is_weak()) {
		GCHandleIntPtr old_gchandle = gchandle.get_intptr();
		gchandle.handle = { nullptr };

		GCHandleIntPtr new_gchandle = { nullptr };
		bool create_weak = true;
		bool target_alive = GDMonoCache::managed_callbacks.ScriptManagerBridge_SwapGCHandleForType(
				old_gchandle, &new_gchandle, create_weak);

		if (!target_alive) {
			return refcount == 0;
		}

		gchandle = MonoGCHandleData(new_gchandle, gdmono::GCHandleType::WEAK_HANDLE);
		return false;
	}

	ref_dying = (refcount == 0);
	return ref_dying;
}

const Variant CSharpInstance::get_rpc_config() const {
	return script->get_rpc_config();
}

void CSharpInstance::notification(int p_notification, bool p_reversed) {
	if (p_notification == Object::NOTIFICATION_PREDELETE) {
		if (base_ref_counted) {
			return;
		}
	} else if (p_notification == Object::NOTIFICATION_PREDELETE_CLEANUP) {
		predelete_notified = true;
		if (base_ref_counted) {
			return;
		}
		if (GDMonoCache::godot_api_cache_updated && GDMonoCache::managed_callbacks.CSharpInstanceBridge_CallDispose) {
			GDMonoCache::managed_callbacks.CSharpInstanceBridge_CallDispose(
					gchandle.get_intptr(), false);
		}
		return;
	}

	_call_notification(p_notification, p_reversed);
}

void CSharpInstance::_call_notification(int p_notification, bool p_reversed) {
	if (!GDMonoCache::godot_api_cache_updated || !GDMonoCache::managed_callbacks.CSharpInstanceBridge_Call) {
		return;
	}
	Variant arg = p_notification;
	const Variant *args[1] = { &arg };
	Variant ret;
	Callable::CallError call_error;
	GDMonoCache::managed_callbacks.CSharpInstanceBridge_Call(
			gchandle.get_intptr(), &SNAME("_notification"), args, 1, &call_error, &ret);
}

String CSharpInstance::to_string(bool *r_valid) {
	String res;
	bool valid = false;
	if (GDMonoCache::godot_api_cache_updated && GDMonoCache::managed_callbacks.CSharpInstanceBridge_CallToString) {
		GDMonoCache::managed_callbacks.CSharpInstanceBridge_CallToString(
				gchandle.get_intptr(), &res, &valid);
	}
	if (r_valid) {
		*r_valid = valid;
	}
	return res;
}

Ref<Script> CSharpInstance::get_script() const {
	return script;
}

ScriptLanguage *CSharpInstance::get_language() {
	return CSharpLanguage::get_singleton();
}

CSharpInstance::CSharpInstance(const Ref<CSharpScript> &p_script) :
		script(p_script) {
}

CSharpInstance::~CSharpInstance() {
	destructing_script_instance = true;
	disconnect_event_signals();

	if (!gchandle.is_released()) {
		if (!predelete_notified && !ref_dying) {
			if (GDMonoCache::godot_api_cache_updated && GDMonoCache::managed_callbacks.CSharpInstanceBridge_CallDispose) {
				GDMonoCache::managed_callbacks.CSharpInstanceBridge_CallDispose(
						gchandle.get_intptr(), true);
			}
		}
		gchandle.release();
	}

	if (base_ref_counted && !ref_dying && owner && unsafe_referenced) {
		RefCounted *rc_owner = static_cast<RefCounted *>(owner);
		Ref<RefCounted> scope_keep_owner_alive(rc_owner);
		(void)scope_keep_owner_alive;

		bool die = _unreference_owner_unsafe();
		CRASH_COND(die);

		void *data = CSharpLanguage::get_instance_binding_with_setup(owner);
		CRASH_COND(data == nullptr);
		CSharpScriptBinding &script_binding = ((RBMap<Object *, CSharpScriptBinding>::Element *)data)->get();
		CRASH_COND(!script_binding.inited);
	}

	if (script.is_valid() && owner) {
		MutexLock lock(CSharpLanguage::get_singleton()->script_instances_mutex);
#ifdef DEBUG_ENABLED
		HashSet<Object *>::Iterator match = script->instances.find(owner);
		CRASH_COND(!match);
		script->instances.remove(match);
#else
		script->instances.erase(owner);
#endif // DEBUG_ENABLED
	}
}

#ifdef TOOLS_ENABLED
void CSharpScript::_placeholder_erased(PlaceHolderScriptInstance *p_placeholder) {
	placeholders.erase(p_placeholder);
}

void CSharpScript::_update_exports_values(HashMap<StringName, Variant> &values, List<PropertyInfo> &propnames) {
	for (const KeyValue<StringName, Variant> &E : exported_members_defval_cache) {
		values[E.key] = E.value;
	}

	for (const PropertyInfo &prop_info : exported_members_cache) {
		propnames.push_back(prop_info);
	}

	if (base_script.is_valid()) {
		base_script->_update_exports_values(values, propnames);
	}
}
#endif

void GD_CLR_STDCALL CSharpScript::_add_property_info_list_callback(CSharpScript *p_script, const String *p_current_class_name, void *p_props, int32_t p_count) {
	GDMonoCache::godotsharp_property_info *props = (GDMonoCache::godotsharp_property_info *)p_props;

#ifdef TOOLS_ENABLED
	p_script->exported_members_cache.push_back(PropertyInfo(
			Variant::NIL, p_script->type_info.class_name, PROPERTY_HINT_NONE,
			p_script->get_path(), PROPERTY_USAGE_CATEGORY));
#endif

	for (int i = 0; i < p_count; i++) {
		const GDMonoCache::godotsharp_property_info &prop = props[i];
		StringName name = *reinterpret_cast<const StringName *>(&prop.name);
		String hint_string = *reinterpret_cast<const String *>(&prop.hint_string);

		PropertyInfo pinfo(prop.type, name, prop.hint, hint_string, prop.usage);
		p_script->member_info[name] = pinfo;

		if (prop.exported) {
#ifdef TOOLS_ENABLED
			p_script->exported_members_cache.push_back(pinfo);
#endif
#if defined(TOOLS_ENABLED) || defined(DEBUG_ENABLED)
			p_script->exported_members_names.insert(name);
#endif // DEBUG_ENABLED
		}
	}
}

#ifdef TOOLS_ENABLED
void GD_CLR_STDCALL CSharpScript::_add_property_default_values_callback(CSharpScript *p_script, void *p_def_vals, int32_t p_count) {
	GDMonoCache::godotsharp_property_def_val_pair *def_vals = (GDMonoCache::godotsharp_property_def_val_pair *)p_def_vals;

	for (int i = 0; i < p_count; i++) {
		const GDMonoCache::godotsharp_property_def_val_pair &def_val_pair = def_vals[i];
		StringName name = *reinterpret_cast<const StringName *>(&def_val_pair.name);
		Variant value = *reinterpret_cast<const Variant *>(&def_val_pair.value);
		p_script->exported_members_defval_cache[name] = value;
	}
}
#endif

bool CSharpScript::_update_exports(PlaceHolderScriptInstance *p_instance_to_update) {
#ifdef TOOLS_ENABLED
	bool is_editor = Engine::get_singleton()->is_editor_hint();
	if (is_editor) {
		placeholder_fallback_enabled = true;
	}
#endif
	if (!valid) {
		return false;
	}

	bool changed = false;

#ifdef TOOLS_ENABLED
	if (exports_invalidated)
#endif
	{
#ifdef TOOLS_ENABLED
		exports_invalidated = false;
#endif
		changed = true;
		member_info.clear();

#ifdef TOOLS_ENABLED
		exported_members_cache.clear();
		exported_members_defval_cache.clear();
#endif

		if (GDMonoCache::godot_api_cache_updated) {
			if (GDMonoCache::managed_callbacks.ScriptManagerBridge_GetPropertyInfoList) {
				GDMonoCache::managed_callbacks.ScriptManagerBridge_GetPropertyInfoList(this, &_add_property_info_list_callback);
			}
#ifdef TOOLS_ENABLED
			if (GDMonoCache::managed_callbacks.ScriptManagerBridge_GetPropertyDefaultValues) {
				GDMonoCache::managed_callbacks.ScriptManagerBridge_GetPropertyDefaultValues(this, &_add_property_default_values_callback);
			}
#endif
		}
	}

#ifdef TOOLS_ENABLED
	if (is_editor) {
		placeholder_fallback_enabled = false;

		if ((changed || p_instance_to_update) && placeholders.size()) {
			HashMap<StringName, Variant> values;
			List<PropertyInfo> propnames;
			_update_exports_values(values, propnames);

			if (changed) {
				for (PlaceHolderScriptInstance *instance : placeholders) {
					instance->update(propnames, values);
				}
			} else {
				p_instance_to_update->update(propnames, values);
			}
		} else if (placeholders.size()) {
			uint64_t script_modified_time = FileAccess::get_modified_time(get_path());
			uint64_t last_valid_build_time = GDMono::get_singleton() ? GDMono::get_singleton()->get_project_assembly_modified_time() : 0;
			if (script_modified_time > last_valid_build_time) {
				for (PlaceHolderScriptInstance *instance : placeholders) {
					Object *owner = instance->get_owner();
					if (owner->get_script_instance() == instance) {
						owner->notify_property_list_changed();
					}
				}
			}
		}
	}
#endif

	return changed;
}

bool CSharpScript::_get(const StringName &p_name, Variant &r_ret) const {
	if (p_name == SNAME("script/source")) {
		r_ret = get_source_code();
		return true;
	}
	return false;
}

bool CSharpScript::_set(const StringName &p_name, const Variant &p_value) {
	if (p_name == SNAME("script/source")) {
		set_source_code(p_value);
		reload();
		return true;
	}
	return false;
}

void CSharpScript::_get_property_list(List<PropertyInfo> *p_properties) const {
	p_properties->push_back(PropertyInfo(Variant::STRING, SNAME("script/source"), PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR | PROPERTY_USAGE_INTERNAL));
}

void CSharpScript::_bind_methods() {
	ClassDB::bind_vararg_method(METHOD_FLAGS_DEFAULT, "new", &CSharpScript::_new, MethodInfo("new"));
}

void CSharpScript::reload_registered_script(Ref<CSharpScript> p_script) {
	ERR_FAIL_COND(!p_script->reload_invalidated);

	p_script->valid = true;
	p_script->reload_invalidated = false;

	update_script_class_info(p_script);
	p_script->_update_exports();

#ifdef TOOLS_ENABLED
	EditorFileSystem *efs = EditorFileSystem::get_singleton();
	if (efs && !p_script->get_path().is_empty()) {
		efs->update_file(p_script->get_path());
	}
#endif
}

void CSharpScript::update_script_class_info(Ref<CSharpScript> p_script) {
	if (!GDMonoCache::godot_api_cache_updated || !GDMonoCache::managed_callbacks.ScriptManagerBridge_UpdateScriptClassInfo) {
		return;
	}

	TypeInfo type_info;
	Array methods_array;
	Dictionary rpc_functions_dict;
	Dictionary signals_dict;
	Ref<CSharpScript> base_script;

	GDMonoCache::managed_callbacks.ScriptManagerBridge_UpdateScriptClassInfo(
			p_script.ptr(), &type_info,
			&methods_array, &rpc_functions_dict, &signals_dict, &base_script);

	p_script->type_info = type_info;
	p_script->rpc_config.clear();
	p_script->rpc_config = rpc_functions_dict;

	p_script->methods.clear();
	p_script->methods.resize(methods_array.size());
	int push_index = 0;

	for (int i = 0; i < methods_array.size(); i++) {
		Dictionary method_info_dict = methods_array[i];
		StringName name = method_info_dict["name"];

		MethodInfo mi;
		mi.name = name;
		mi.return_val = PropertyInfo::from_dict(method_info_dict["return_val"]);

		Array params = method_info_dict["params"];
		for (int j = 0; j < params.size(); j++) {
			Dictionary param = params[j];
			Variant::Type param_type = (Variant::Type)(int)param["type"];
			PropertyInfo arg_info = PropertyInfo(param_type, (String)param["name"]);
			arg_info.usage = (uint32_t)param["usage"];
			if (param.has("class_name")) {
				arg_info.class_name = (StringName)param["class_name"];
			}
			mi.arguments.push_back(arg_info);
		}

		mi.flags = (uint32_t)method_info_dict["flags"];
		p_script->methods.set(push_index++, CSharpMethodInfo{ name, mi });
	}

	p_script->event_signals.clear();
	p_script->event_signals.resize(signals_dict.size());
	push_index = 0;

	for (const Variant *s = signals_dict.next(nullptr); s != nullptr; s = signals_dict.next(s)) {
		StringName name = *s;
		MethodInfo mi;
		mi.name = name;

		Array params = signals_dict[*s];
		for (int i = 0; i < params.size(); i++) {
			Dictionary param = params[i];
			Variant::Type param_type = (Variant::Type)(int)param["type"];
			PropertyInfo arg_info = PropertyInfo(param_type, (String)param["name"]);
			arg_info.usage = (uint32_t)param["usage"];
			if (param.has("class_name")) {
				arg_info.class_name = (StringName)param["class_name"];
			}
			mi.arguments.push_back(arg_info);
		}

		p_script->event_signals.set(push_index++, EventSignalInfo{ name, mi });
	}

	p_script->base_script = base_script;
}

bool CSharpScript::can_instantiate() const {
#ifdef TOOLS_ENABLED
	bool extra_cond = (type_info.is_tool || ScriptServer::is_scripting_enabled()) && !Engine::get_singleton()->is_recovery_mode_hint();
#else
	bool extra_cond = true;
#endif

	if (extra_cond && !valid) {
		ERR_FAIL_V_MSG(false, "Cannot instantiate C# script because the associated class could not be found. Script: '" + get_path() + "'.");
	}

	return valid && type_info.can_instantiate() && extra_cond;
}

StringName CSharpScript::get_instance_base_type() const {
	return type_info.native_base_name;
}

CSharpInstance *CSharpScript::_create_instance(const Variant **p_args, int p_argcount, Object *p_owner, bool p_is_ref_counted, Callable::CallError &r_error) {
	ERR_FAIL_COND_V_MSG(!type_info.can_instantiate(), nullptr, "Cannot instantiate C# script. Script: '" + get_path() + "'.");

	Ref<RefCounted> ref;
	if (p_is_ref_counted) {
		ref = Ref<RefCounted>(static_cast<RefCounted *>(p_owner));
	}

	if (CSharpLanguage::has_instance_binding(p_owner)) {
		void *data = CSharpLanguage::get_existing_instance_binding(p_owner);
		CRASH_COND(data == nullptr);

		CSharpScriptBinding &script_binding = ((RBMap<Object *, CSharpScriptBinding>::Element *)data)->get();
		if (script_binding.inited && !script_binding.gchandle.is_released()) {
			if (GDMonoCache::godot_api_cache_updated && GDMonoCache::managed_callbacks.CSharpInstanceBridge_CallDispose) {
				GDMonoCache::managed_callbacks.CSharpInstanceBridge_CallDispose(
						script_binding.gchandle.get_intptr(), true);
			}
			script_binding.gchandle.release();
			script_binding.inited = false;
		}
	}

	CSharpInstance *instance = memnew(CSharpInstance(Ref<CSharpScript>(this)));
	instance->base_ref_counted = p_is_ref_counted;
	instance->owner = p_owner;
	instance->owner->set_script_instance(instance);

	if (!GDMonoCache::godot_api_cache_updated || !GDMonoCache::managed_callbacks.ScriptManagerBridge_CreateManagedForGodotObjectScriptInstance) {
		instance->script = Ref<CSharpScript>();
		p_owner->set_script_instance(nullptr);
		instance->owner = nullptr;
		return nullptr;
	}

	bool ok = GDMonoCache::managed_callbacks.ScriptManagerBridge_CreateManagedForGodotObjectScriptInstance(
			this, p_owner, p_args, p_argcount);

	if (!ok) {
		instance->script = Ref<CSharpScript>();
		p_owner->set_script_instance(nullptr);
		instance->owner = nullptr;
		return nullptr;
	}

	CRASH_COND(instance->gchandle.is_released());
	return instance;
}

Variant CSharpScript::_new(const Variant **p_args, int p_argcount, Callable::CallError &r_error) {
	if (!valid || !GDMonoCache::godot_api_cache_updated || !GDMonoCache::managed_callbacks.ScriptManagerBridge_GetScriptNativeName) {
		r_error.error = Callable::CallError::CALL_ERROR_INVALID_METHOD;
		return Variant();
	}

	r_error.error = Callable::CallError::CALL_OK;

	StringName native_name;
	GDMonoCache::managed_callbacks.ScriptManagerBridge_GetScriptNativeName(this, &native_name);

	ERR_FAIL_COND_V(native_name == StringName(), Variant());

	Object *owner = ClassDB::instantiate(native_name);

	Ref<RefCounted> ref;
	RefCounted *r = Object::cast_to<RefCounted>(owner);
	if (r) {
		ref = Ref<RefCounted>(r);
	}

	CSharpInstance *instance = _create_instance(p_args, p_argcount, owner, r != nullptr, r_error);
	if (!instance) {
		if (ref.is_null()) {
			memdelete(owner);
		}
		return Variant();
	}

	if (ref.is_valid()) {
		return ref;
	} else {
		return owner;
	}
}

ScriptInstance *CSharpScript::instance_create(Object *p_this) {
	if (!valid || !GDMonoCache::godot_api_cache_updated || !GDMonoCache::managed_callbacks.ScriptManagerBridge_GetScriptNativeName) {
		return nullptr;
	}

	StringName native_name;
	GDMonoCache::managed_callbacks.ScriptManagerBridge_GetScriptNativeName(this, &native_name);

	ERR_FAIL_COND_V(native_name == StringName(), nullptr);

	if (!p_this->is_class(native_name)) {
		if (EngineDebugger::is_active()) {
			CSharpLanguage::get_singleton()->debug_break_parse(get_path(), 0,
					"Script inherits from native type '" + String(native_name) +
							"', so it can't be assigned to an object of type: '" + p_this->get_class() + "'");
		}
		ERR_FAIL_V_MSG(nullptr, "Script inherits from native type '" + String(native_name) + "', so it can't be assigned to an object of type: '" + p_this->get_class() + "'.");
	}

	Callable::CallError unchecked_error;
	return _create_instance(nullptr, 0, p_this, Object::cast_to<RefCounted>(p_this) != nullptr, unchecked_error);
}

PlaceHolderScriptInstance *CSharpScript::placeholder_instance_create(Object *p_this) {
#ifdef TOOLS_ENABLED
	PlaceHolderScriptInstance *si = memnew(PlaceHolderScriptInstance(CSharpLanguage::get_singleton(), Ref<Script>(this), p_this));
	placeholders.insert(si);
	_update_exports(si);
	return si;
#else
	return nullptr;
#endif
}

bool CSharpScript::has_source_code() const {
	return !source.is_empty();
}

String CSharpScript::get_source_code() const {
	return source;
}

void CSharpScript::set_source_code(const String &p_code) {
	if (source == p_code) {
		return;
	}
	source = p_code;
#ifdef TOOLS_ENABLED
	source_changed_cache = true;
#endif
}

void CSharpScript::get_script_method_list(List<MethodInfo> *p_list) const {
	if (!valid) {
		return;
	}

	const CSharpScript *top = this;
	while (top != nullptr) {
		for (const CSharpMethodInfo &E : top->methods) {
			p_list->push_back(E.method_info);
		}
		top = top->base_script.ptr();
	}
}

bool CSharpScript::has_method(const StringName &p_method) const {
	if (!valid) {
		return false;
	}

	for (const CSharpMethodInfo &E : methods) {
		if (E.name == p_method) {
			return true;
		}
	}
	return false;
}

int CSharpScript::get_script_method_argument_count(const StringName &p_method, bool *r_is_valid) const {
	if (!valid) {
		if (r_is_valid) {
			*r_is_valid = false;
		}
		return 0;
	}

	for (const CSharpMethodInfo &E : methods) {
		if (E.name == p_method) {
			if (r_is_valid) {
				*r_is_valid = true;
			}
			return E.method_info.arguments.size();
		}
	}

	if (r_is_valid) {
		*r_is_valid = false;
	}
	return 0;
}

MethodInfo CSharpScript::get_method_info(const StringName &p_method) const {
	if (!valid) {
		return MethodInfo();
	}

	MethodInfo mi;
	for (const CSharpMethodInfo &E : methods) {
		if (E.name == p_method) {
			if (mi.name == p_method) {
				return MethodInfo();
			}
			mi = E.method_info;
		}
	}
	return mi;
}

Variant CSharpScript::callp(const StringName &p_method, const Variant **p_args, int p_argcount, Callable::CallError &r_error) {
	if (valid && GDMonoCache::godot_api_cache_updated && GDMonoCache::managed_callbacks.ScriptManagerBridge_CallStatic) {
		Variant ret;
		bool ok = GDMonoCache::managed_callbacks.ScriptManagerBridge_CallStatic(this, &p_method, p_args, p_argcount, &r_error, &ret);
		if (ok) {
			return ret;
		}
	}
	return Script::callp(p_method, p_args, p_argcount, r_error);
}

Error CSharpScript::reload(bool p_keep_state) {
	if (!reload_invalidated) {
		return OK;
	}

	reload_invalidated = false;
	String script_path = get_path();

	if (!GDMonoCache::godot_api_cache_updated || !GDMonoCache::managed_callbacks.ScriptManagerBridge_AddScriptBridge) {
		return OK;
	}

	valid = GDMonoCache::managed_callbacks.ScriptManagerBridge_AddScriptBridge(this, &script_path);

	if (valid) {
		update_script_class_info(this);
		_update_exports();

#ifdef TOOLS_ENABLED
		EditorFileSystem *efs = EditorFileSystem::get_singleton();
		if (efs) {
			efs->update_file(script_path);
		}
#endif
	}

	return OK;
}

ScriptLanguage *CSharpScript::get_language() const {
	return CSharpLanguage::get_singleton();
}

bool CSharpScript::get_property_default_value(const StringName &p_property, Variant &r_value) const {
#ifdef TOOLS_ENABLED
	HashMap<StringName, Variant>::ConstIterator E = exported_members_defval_cache.find(p_property);
	if (E) {
		r_value = E->value;
		return true;
	}

	if (base_script.is_valid()) {
		return base_script->get_property_default_value(p_property, r_value);
	}
#endif
	return false;
}

void CSharpScript::update_exports() {
#ifdef TOOLS_ENABLED
	_update_exports();
#endif
}

bool CSharpScript::has_script_signal(const StringName &p_signal) const {
	if (!valid || !GDMonoCache::godot_api_cache_updated) {
		return false;
	}

	for (const EventSignalInfo &signal : event_signals) {
		if (signal.name == p_signal) {
			return true;
		}
	}

	if (base_script.is_valid()) {
		return base_script->has_script_signal(p_signal);
	}
	return false;
}

void CSharpScript::_get_script_signal_list(List<MethodInfo> *r_signals, bool p_include_base) const {
	if (!valid) {
		return;
	}

	for (const EventSignalInfo &signal : event_signals) {
		r_signals->push_back(signal.method_info);
	}

	if (!p_include_base) {
		return;
	}

	if (base_script.is_valid()) {
		base_script->get_script_signal_list(r_signals);
	}
}

void CSharpScript::get_script_signal_list(List<MethodInfo> *r_signals) const {
	_get_script_signal_list(r_signals, true);
}

bool CSharpScript::inherits_script(const Ref<Script> &p_script) const {
	Ref<CSharpScript> cs = p_script;
	if (cs.is_null() || !valid || !cs->valid || !GDMonoCache::godot_api_cache_updated || !GDMonoCache::managed_callbacks.ScriptManagerBridge_ScriptIsOrInherits) {
		return false;
	}
	return GDMonoCache::managed_callbacks.ScriptManagerBridge_ScriptIsOrInherits(this, cs.ptr());
}

Ref<Script> CSharpScript::get_base_script() const {
	return base_script;
}

StringName CSharpScript::get_global_name() const {
	return type_info.is_global_class ? StringName(type_info.class_name) : StringName();
}

void CSharpScript::get_script_property_list(List<PropertyInfo> *r_list) const {
#ifdef TOOLS_ENABLED
	const CSharpScript *top = this;
	while (top != nullptr) {
		for (const PropertyInfo &E : top->exported_members_cache) {
			r_list->push_back(E);
		}
		top = top->base_script.ptr();
	}
#else
	const CSharpScript *top = this;
	while (top != nullptr) {
		List<PropertyInfo> props;
		for (const KeyValue<StringName, PropertyInfo> &E : top->member_info) {
			props.push_front(E.value);
		}
		for (const PropertyInfo &prop : props) {
			r_list->push_back(prop);
		}
		top = top->base_script.ptr();
	}
#endif
}

int CSharpScript::get_member_line(const StringName &p_member) const {
	return -1;
}

const Variant CSharpScript::get_rpc_config() const {
	return rpc_config;
}

Error CSharpScript::load_source_code(const String &p_path) {
	Error ferr = read_all_file_utf8(p_path, source);
	ERR_FAIL_COND_V_MSG(ferr != OK, ferr, "Failed to read file: '" + p_path + "'.");

#ifdef TOOLS_ENABLED
	source_changed_cache = true;
#endif
	return OK;
}

void CSharpScript::_clear() {
	type_info = TypeInfo();
	valid = false;
	reload_invalidated = true;
}

CSharpScript::CSharpScript() {
	_clear();
#ifdef DEBUG_ENABLED
	{
		MutexLock lock(CSharpLanguage::get_singleton()->script_instances_mutex);
		CSharpLanguage::get_singleton()->script_list.add(&script_list);
	}
#endif // DEBUG_ENABLED
}

CSharpScript::~CSharpScript() {
#ifdef DEBUG_ENABLED
	{
		MutexLock lock(CSharpLanguage::get_singleton()->script_instances_mutex);
		CSharpLanguage::get_singleton()->script_list.remove(&script_list);
	}
#endif // DEBUG_ENABLED

	if (GDMonoCache::godot_api_cache_updated && GDMonoCache::managed_callbacks.ScriptManagerBridge_RemoveScriptBridge) {
		GDMonoCache::managed_callbacks.ScriptManagerBridge_RemoveScriptBridge(this);
	}
}

void CSharpScript::get_members(HashSet<StringName> *r_members) {
#ifdef DEBUG_ENABLED
	if (r_members) {
		for (const StringName &member_name : exported_members_names) {
			r_members->insert(member_name);
		}
	}
#endif // DEBUG_ENABLED
}
