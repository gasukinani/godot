/**************************************************************************/
/*  editor_theme_manager.cpp                                              */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "editor_theme_manager.h"

#include "core/error/error_macros.h"
#include "core/io/resource_loader.h"
#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "editor/editor_string_names.h"
#include "editor/file_system/editor_paths.h"
#include "editor/settings/editor_settings.h"
#include "editor/themes/editor_color_map.h"
#include "editor/themes/editor_fonts.h"
#include "editor/themes/editor_icons.h"
#include "editor/themes/editor_scale.h"
#include "editor/themes/editor_theme.h"
#include "editor/themes/theme_classic.h"
#include "editor/themes/theme_modern.h"
#include "scene/resources/style_box_flat.h"
#include "scene/resources/style_box_line.h"
#include "scene/resources/style_box_texture.h"
#include "scene/resources/texture.h"
#include "scene/scene_string_names.h"
#include "servers/display/display_server.h"

// Theme configuration.

uint32_t EditorThemeManager::ThemeConfiguration::hash() {
	uint32_t hash = hash_murmur3_one_float(EDSCALE);

	// Basic properties.

	hash = hash_murmur3_one_32(style.hash(), hash);
	hash = hash_murmur3_one_32(preset.hash(), hash);
	hash = hash_murmur3_one_32(spacing_preset.hash(), hash);

	hash = hash_murmur3_one_32(base_color.to_rgba32(), hash);
	hash = hash_murmur3_one_32(accent_color.to_rgba32(), hash);
	hash = hash_murmur3_one_float(contrast, hash);
	hash = hash_murmur3_one_float(icon_saturation, hash);

	// Extra properties.

	hash = hash_murmur3_one_32(base_spacing, hash);
	hash = hash_murmur3_one_32(extra_spacing, hash);
	hash = hash_murmur3_one_32(border_width, hash);
	hash = hash_murmur3_one_32(corner_radius, hash);

	hash = hash_murmur3_one_32((int)draw_extra_borders, hash);
	hash = hash_murmur3_one_float(relationship_line_opacity, hash);
	hash = hash_murmur3_one_32(thumb_size, hash);
	hash = hash_murmur3_one_32(class_icon_size, hash);
	hash = hash_murmur3_one_32((int)enable_touch_optimizations, hash);
	hash = hash_murmur3_one_float(gizmo_handle_scale, hash);
	hash = hash_murmur3_one_32(inspector_property_height, hash);
	hash = hash_murmur3_one_float(subresource_hue_tint, hash);

	hash = hash_murmur3_one_float(default_contrast, hash);

	// Generated properties.

	hash = hash_murmur3_one_32((int)dark_theme, hash);
	hash = hash_murmur3_one_32((int)dark_icon_and_font, hash);

	return hash;
}

uint32_t EditorThemeManager::ThemeConfiguration::hash_fonts() {
	uint32_t hash = hash_murmur3_one_float(EDSCALE);

	// TODO: Implement the hash based on what editor_register_fonts() uses.

	return hash;
}

uint32_t EditorThemeManager::ThemeConfiguration::hash_icons() {
	uint32_t hash = hash_murmur3_one_float(EDSCALE);

	hash = hash_murmur3_one_32(accent_color.to_rgba32(), hash);
	hash = hash_murmur3_one_float(icon_saturation, hash);

	hash = hash_murmur3_one_32(thumb_size, hash);
	hash = hash_murmur3_one_float(gizmo_handle_scale, hash);

	hash = hash_murmur3_one_32((int)dark_icon_and_font, hash);

	return hash;
}

// Benchmarks.

int EditorThemeManager::benchmark_run = 0;

String EditorThemeManager::get_benchmark_key() {
	if (benchmark_run == 0) {
		return "EditorTheme (Startup)";
	}

	return vformat("EditorTheme (Run %d)", benchmark_run);
}

// Generation helper methods.

Ref<StyleBoxTexture> EditorThemeManager::make_stylebox(Ref<Texture2D> p_texture, float p_left, float p_top, float p_right, float p_bottom, float p_margin_left, float p_margin_top, float p_margin_right, float p_margin_bottom, bool p_draw_center) {
	Ref<StyleBoxTexture> style(memnew(StyleBoxTexture));
	style->set_texture(p_texture);
	style->set_texture_margin_individual(p_left * EDSCALE, p_top * EDSCALE, p_right * EDSCALE, p_bottom * EDSCALE);
	style->set_content_margin_individual((p_left + p_margin_left) * EDSCALE, (p_top + p_margin_top) * EDSCALE, (p_right + p_margin_right) * EDSCALE, (p_bottom + p_margin_bottom) * EDSCALE);
	style->set_draw_center(p_draw_center);
	return style;
}

Ref<StyleBoxEmpty> EditorThemeManager::make_empty_stylebox(float p_margin_left, float p_margin_top, float p_margin_right, float p_margin_bottom) {
	Ref<StyleBoxEmpty> style(memnew(StyleBoxEmpty));
	style->set_content_margin_individual(p_margin_left * EDSCALE, p_margin_top * EDSCALE, p_margin_right * EDSCALE, p_margin_bottom * EDSCALE);
	return style;
}

Ref<StyleBoxFlat> EditorThemeManager::make_flat_stylebox(Color p_color, float p_margin_left, float p_margin_top, float p_margin_right, float p_margin_bottom, int p_corner_width) {
	Ref<StyleBoxFlat> style(memnew(StyleBoxFlat));
	style->set_bg_color(p_color);
	// Adjust level of detail based on the corners' effective sizes.
	style->set_corner_detail(Math::ceil(1.0 * p_corner_width * EDSCALE));
	style->set_corner_radius_all(p_corner_width * EDSCALE);
	style->set_content_margin_individual(p_margin_left * EDSCALE, p_margin_top * EDSCALE, p_margin_right * EDSCALE, p_margin_bottom * EDSCALE);
	style->set_anti_aliased(true);
	return style;
}

Ref<StyleBoxLine> EditorThemeManager::make_line_stylebox(Color p_color, int p_thickness, float p_grow_begin, float p_grow_end, bool p_vertical) {
	Ref<StyleBoxLine> style(memnew(StyleBoxLine));
	style->set_color(p_color);
	style->set_grow_begin(p_grow_begin);
	style->set_grow_end(p_grow_end);
	style->set_thickness(p_thickness);
	style->set_vertical(p_vertical);
	return style;
}

// Theme generation and population routines.

Ref<EditorTheme> EditorThemeManager::_create_base_theme(const Ref<EditorTheme> &p_old_theme) {
	OS::get_singleton()->benchmark_begin_measure(get_benchmark_key(), "Create Base Theme");

	Ref<EditorTheme> theme = memnew(EditorTheme);
	ThemeConfiguration config = _create_theme_config();
	theme->set_generated_hash(config.hash());
	theme->set_generated_fonts_hash(config.hash_fonts());
	theme->set_generated_icons_hash(config.hash_icons());

	print_verbose(vformat("EditorTheme: Generating new theme for the config '%d'.", theme->get_generated_hash()));

	bool is_default_style = config.style == "Modern";
	if (is_default_style) {
		ThemeModern::populate_shared_styles(theme, config);
	} else {
		ThemeClassic::populate_shared_styles(theme, config);
	}

	// Register icons.
	{
		OS::get_singleton()->benchmark_begin_measure(get_benchmark_key(), "Register Icons");

		// External functions, see editor_icons.cpp.
		editor_configure_icons(config.dark_icon_and_font);

		// If settings are comparable to the old theme, then just copy existing icons over.
		// Otherwise, regenerate them.
		bool keep_old_icons = (p_old_theme.is_valid() && theme->get_generated_icons_hash() == p_old_theme->get_generated_icons_hash());
		if (keep_old_icons) {
			print_verbose("EditorTheme: Can keep old icons, copying.");
			editor_copy_icons(theme, p_old_theme);
		} else {
			print_verbose("EditorTheme: Generating new icons.");
			editor_register_icons(theme, config.dark_icon_and_font, config.icon_saturation, config.thumb_size, config.gizmo_handle_scale);
		}

		OS::get_singleton()->benchmark_end_measure(get_benchmark_key(), "Register Icons");
	}

	// Register fonts.
	{
		OS::get_singleton()->benchmark_begin_measure(get_benchmark_key(), "Register Fonts");

		// TODO: Check if existing font definitions from the old theme are usable and copy them.

		// External function, see editor_fonts.cpp.
		print_verbose("EditorTheme: Generating new fonts.");
		editor_register_fonts(theme);

		OS::get_singleton()->benchmark_end_measure(get_benchmark_key(), "Register Fonts");
	}

	// TODO: Check if existing style definitions from the old theme are usable and copy them.

	print_verbose("EditorTheme: Generating new styles.");

	if (is_default_style) {
		ThemeModern::populate_standard_styles(theme, config);
		ThemeModern::populate_editor_styles(theme, config);
	} else {
		ThemeClassic::populate_standard_styles(theme, config);
		ThemeClassic::populate_editor_styles(theme, config);
	}

	_populate_text_editor_styles(theme, config);
	_populate_visual_shader_styles(theme, config);

	OS::get_singleton()->benchmark_end_measure(get_benchmark_key(), "Create Base Theme");
	return theme;
}

EditorThemeManager::ThemeConfiguration EditorThemeManager::_create_theme_config() {
	ThemeConfiguration config;

	// Basic properties.

	config.style = EDITOR_GET("interface/theme/style");
	config.preset = EDITOR_GET("interface/theme/color_preset");
	config.spacing_preset = EDITOR_GET("interface/theme/spacing_preset");

	config.base_color = EDITOR_GET("interface/theme/base_color");
	config.accent_color = EDITOR_GET("interface/theme/accent_color");
	config.contrast = EDITOR_GET("interface/theme/contrast");
	config.icon_saturation = EDITOR_GET("interface/theme/icon_saturation");
	config.corner_radius = EDITOR_GET("interface/theme/corner_radius");

	// Extra properties.

	config.base_spacing = EDITOR_GET("interface/theme/base_spacing");
	config.extra_spacing = EDITOR_GET("interface/theme/additional_spacing");
	// Ensure borders are visible when using an editor scale below 100%.
	config.border_width = CLAMP((int)EDITOR_GET("interface/theme/border_size"), 0, 2) * MAX(1, EDSCALE);

	config.draw_extra_borders = EDITOR_GET("interface/theme/draw_extra_borders");
	config.draw_relationship_lines = EDITOR_GET("interface/theme/draw_relationship_lines");
	config.relationship_line_opacity = EDITOR_GET("interface/theme/relationship_line_opacity");
	config.thumb_size = EDITOR_GET("filesystem/file_dialog/thumbnail_size");
	config.class_icon_size = 16 * EDSCALE;
	config.enable_touch_optimizations = EDITOR_GET("interface/touchscreen/enable_touch_optimizations");
	config.gizmo_handle_scale = EDITOR_GET("interface/touchscreen/scale_gizmo_handles");
	config.subresource_hue_tint = EDITOR_GET("docks/property_editor/subresource_hue_tint");
	config.dragging_hover_wait_msec = (float)EDITOR_GET("interface/editor/timers/dragging_hover_wait_seconds") * 1000;
	config.max_sticky_tree_items = EDITOR_GET("interface/editor/appearance/max_sticky_tree_items");

	// Handle theme style.
	if (config.preset != "Custom") {
		if (config.style == "Classic") {
			config.draw_relationship_lines = RELATIONSHIP_ALL;
			config.corner_radius = 3;
		} else { // Modern Rounded
			config.draw_relationship_lines = RELATIONSHIP_ALL;
			config.corner_radius = 6;
		}

		EditorSettings::get_singleton()->set_initial_value("interface/theme/draw_relationship_lines", config.draw_relationship_lines);
		EditorSettings::get_singleton()->set_initial_value("interface/theme/corner_radius", config.corner_radius);

		// Enforce values in case they were adjusted or overridden.
		EditorSettings::get_singleton()->set_manually("interface/theme/draw_relationship_lines", config.draw_relationship_lines);
		EditorSettings::get_singleton()->set_manually("interface/theme/corner_radius", config.corner_radius);
	}

	// Handle color presets (kasama ang mga bagong custom themes).
	{
		const bool follow_system_theme = EDITOR_GET("interface/theme/follow_system_theme");
		const bool use_system_accent_color = EDITOR_GET("interface/theme/use_system_accent_color");
		DisplayServer *display_server = DisplayServer::get_singleton();
		Color system_base_color = display_server->get_base_color();
		Color system_accent_color = display_server->get_accent_color();

		if (follow_system_theme) {
			String dark_theme = "Default";
			String light_theme = "Light";

			config.preset = light_theme;

			if (system_base_color == Color(0, 0, 0, 0)) {
				if (display_server->is_dark_mode_supported() && display_server->is_dark_mode()) {
					config.preset = dark_theme;
				}
			} else {
				if (system_base_color.get_luminance() < 0.5) {
					config.preset = dark_theme;
				}
			}
		}

		if (config.preset != "Custom") {
			Color preset_accent_color;
			Color preset_base_color;
			float preset_contrast = 0.26;
			bool preset_draw_extra_borders = false;
			float preset_icon_saturation = 1.35;

			const float light_contrast = -0.06;

			// MGA BAGONG CUSTOM THEME PRESETS:
			if (config.preset == "Catppuccin Mocha") {
				preset_accent_color = Color(0.796, 0.651, 0.969); // Mauve (#cba6f7)
				preset_base_color = Color(0.118, 0.118, 0.180);   // Deep Slate (#1e1e2e)
				preset_contrast = 0.28;
				preset_icon_saturation = 1.3;
			} else if (config.preset == "Tokyo Night") {
				preset_accent_color = Color(0.478, 0.635, 0.969); // Electric Blue (#7aa2f7)
				preset_base_color = Color(0.102, 0.106, 0.149);   // Midnight Slate (#1a1b26)
				preset_contrast = 0.32;
				preset_icon_saturation = 1.4;
			} else if (config.preset == "Cyberpunk Neon") {
				preset_accent_color = Color(0.00, 0.95, 0.85);    // Neon Cyan (#00f2d8)
				preset_base_color = Color(0.06, 0.06, 0.09);      // Dark Synth (#0f0f17)
				preset_contrast = 0.38;
				preset_draw_extra_borders = true;
				preset_icon_saturation = 1.7;
			} else if (config.preset == "Nord Frost") {
				preset_accent_color = Color(0.533, 0.753, 0.816); // Frost Blue (#88c0d0)
				preset_base_color = Color(0.180, 0.204, 0.251);   // Polar Night (#2e3440)
				preset_contrast = 0.25;
				preset_icon_saturation = 1.2;
			} else if (config.preset == "Black (OLED)") {
				preset_accent_color = Color(0.45, 0.75, 1.0);
				preset_base_color = Color(0, 0, 0);
				preset_contrast = 0.0;
				preset_draw_extra_borders = true;
			} else if (config.preset == "Breeze Dark") {
				preset_accent_color = Color(0.239, 0.682, 0.914);
				preset_base_color = Color(0.1255, 0.1373, 0.149);
			} else if (config.preset == "Godot 2") {
				preset_accent_color = Color(0.53, 0.67, 0.89);
				preset_base_color = Color(0.24, 0.23, 0.27);
				preset_icon_saturation = 1;
			} else if (config.preset == "Godot 3") {
				preset_accent_color = Color(0.44, 0.73, 0.98);
				preset_base_color = Color(0.21, 0.24, 0.29);
				preset_icon_saturation = 1;
			} else if (config.preset == "Gray") {
				preset_accent_color = Color(0.44, 0.73, 0.98);
				preset_base_color = Color(0.24, 0.24, 0.24);
			} else if (config.preset == "Light") {
				preset_accent_color = Color(0.18, 0.50, 1.00);
				preset_base_color = Color(0.9, 0.9, 0.9);
				preset_contrast = light_contrast;
				preset_icon_saturation = 1;
			} else if (config.preset == "Solarized (Dark)") {
				preset_accent_color = Color(0.15, 0.55, 0.82);
				preset_base_color = Color(0.03, 0.21, 0.26);
				preset_contrast = 0.23;
			} else if (config.preset == "Solarized (Light)") {
				preset_accent_color = Color(0.15, 0.55, 0.82);
				preset_base_color = Color(0.89, 0.86, 0.79);
				preset_contrast = light_contrast;
			} else { // Default - Modern Catppuccin / Tokyo Hybrid Look
				preset_accent_color = Color(0.537, 0.706, 0.980); // #89b4fa
				preset_base_color = Color(0.118, 0.122, 0.180);   // #1e1e2e
				preset_contrast = 0.26;
				preset_icon_saturation = 1.35;
			}

			config.accent_color = preset_accent_color;
			config.base_color = preset_base_color;
			config.contrast = preset_contrast;
			config.draw_extra_borders = preset_draw_extra_borders;
			config.icon_saturation = preset_icon_saturation;

			EditorSettings::get_singleton()->set_initial_value("interface/theme/accent_color", config.accent_color);
			EditorSettings::get_singleton()->set_initial_value("interface/theme/base_color", config.base_color);
			EditorSettings::get_singleton()->set_initial_value("interface/theme/contrast", config.contrast);
			EditorSettings::get_singleton()->set_initial_value("interface/theme/draw_extra_borders", config.draw_extra_borders);
			EditorSettings::get_singleton()->set_initial_value("interface/theme/icon_saturation", config.icon_saturation);
		}

		if (use_system_accent_color && system_accent_color != Color(0, 0, 0, 0)) {
			config.accent_color = system_accent_color;
			config.preset = "Custom";
		}

		// Enforce values in case they were adjusted or overridden.
		EditorSettings::get_singleton()->set_manually("interface/theme/color_preset", config.preset);
		EditorSettings::get_singleton()->set_manually("interface/theme/accent_color", config.accent_color);
		EditorSettings::get_singleton()->set_manually("interface/theme/base_color", config.base_color);
		EditorSettings::get_singleton()->set_manually("interface/theme/contrast", config.contrast);
		EditorSettings::get_singleton()->set_manually("interface/theme/draw_extra_borders", config.draw_extra_borders);
		EditorSettings::get_singleton()->set_manually("interface/theme/icon_saturation", config.icon_saturation);
	}

	// Handle theme spacing preset (kasama ang bagong Touch / Mobile Spacing).
	{
		if (config.spacing_preset != "Custom") {
			int preset_base_spacing = 0;
			int preset_extra_spacing = 0;
			Size2 preset_dialogs_buttons_min_size;

			if (config.spacing_preset == "Compact") {
				preset_base_spacing = 2;
				preset_extra_spacing = 2;
				preset_dialogs_buttons_min_size = Size2(90, 26);
			} else if (config.spacing_preset == "Touch (Mobile)") {
				// Malalaking buttons at comfortable spacing para sa Android touchscreen
				preset_base_spacing = 7;
				preset_extra_spacing = 4;
				preset_dialogs_buttons_min_size = Size2(120, 42);
			} else if (config.spacing_preset == "Spacious") {
				preset_base_spacing = 6;
				preset_extra_spacing = 2;
				preset_dialogs_buttons_min_size = Size2(112, 36);
			} else { // Default
				preset_base_spacing = 5;
				preset_extra_spacing = 1;
				preset_dialogs_buttons_min_size = Size2(108, 36);
			}

			config.base_spacing = preset_base_spacing;
			config.extra_spacing = preset_extra_spacing;
			config.dialogs_buttons_min_size = preset_dialogs_buttons_min_size;

			EditorSettings::get_singleton()->set_initial_value("interface/theme/base_spacing", config.base_spacing);
			EditorSettings::get_singleton()->set_initial_value("interface/theme/additional_spacing", config.extra_spacing);
		}

		// Enforce values in case they were adjusted or overridden.
		EditorSettings::get_singleton()->set_manually("interface/theme/spacing_preset", config.spacing_preset);
		EditorSettings::get_singleton()->set_manually("interface/theme/base_spacing", config.base_spacing);
		EditorSettings::get_singleton()->set_manually("interface/theme/additional_spacing", config.extra_spacing);
	}

	// Generated properties.

	config.dark_theme = is_dark_theme();
	config.dark_icon_and_font = is_dark_icon_and_font();

	config.base_margin = config.base_spacing;
	config.increased_margin = config.base_spacing + config.extra_spacing * 0.75;
	config.separation_margin = (config.base_spacing + config.extra_spacing / 2) * EDSCALE;
	config.popup_margin = config.base_margin * 2.4 * EDSCALE;
	config.window_border_margin = MAX(1, config.base_margin * EDSCALE);
	config.top_bar_separation = MAX(1, config.base_margin * EDSCALE);

	const int separation_base = config.increased_margin + 6;
	config.forced_even_separation = separation_base + (separation_base % 2);

	return config;
}

void _load_text_editor_theme() {
	EditorSettings *settings = EditorSettings::get_singleton();
	const String theme_name = settings->get_setting("text_editor/theme/color_theme");

	ERR_FAIL_COND(EditorSettings::is_default_text_editor_theme(theme_name.get_file().to_lower()));

	const String theme_path = EditorPaths::get_singleton()->get_text_editor_themes_dir().path_join(theme_name + ".tet");

	Ref<ConfigFile> cf;
	cf.instantiate();
	Error err = cf->load(theme_path);
	ERR_FAIL_COND_MSG(err != OK, vformat("Failed to load text editor theme file '%s': %s", theme_name, error_names[err]));

	const PackedStringArray keys = cf->get_section_keys("color_theme");

	for (const String &key : keys) {
		const String setting_key = "text_editor/theme/highlighting/" + key;
		if (!settings->has_setting(setting_key) || !key.contains("color")) {
			continue;
		}
		const String val = cf->get_value("color_theme", key);
		if (val.is_valid_html_color()) {
			const Color color_value = Color::html(val);
			settings->set_initial_value(setting_key, color_value);
			settings->set_manually(setting_key, color_value);
		}
	}
}

void EditorThemeManager::_populate_text_editor_styles(const Ref<EditorTheme> &p_theme, ThemeConfiguration &p_config) {
	const String text_editor_color_theme = EDITOR_GET("text_editor/theme/color_theme");
	const bool is_default_theme = text_editor_color_theme == "Default";
	const bool is_godot2_theme = text_editor_color_theme == "Godot 2";
	const bool is_custom_theme = text_editor_color_theme == "Custom";
	if (is_default_theme || is_godot2_theme || is_custom_theme) {
		HashMap<StringName, Color> colors;
		if (is_default_theme || is_custom_theme) {
			// BAGONG MODERN SYNTAX TOKEN COLORS (Catppuccin Pastel Palette):
			colors["text_editor/theme/highlighting/symbol_color"] = Color(0.557, 0.827, 0.925);           // Operators (#89dceb)
			colors["text_editor/theme/highlighting/keyword_color"] = Color(0.796, 0.651, 0.969);          // Keywords (#cba6f7 - Mauve)
			colors["text_editor/theme/highlighting/control_flow_keyword_color"] = Color(0.953, 0.545, 0.659); // Control Flow (#f38ba8 - Rose)
			colors["text_editor/theme/highlighting/base_type_color"] = Color(0.976, 0.886, 0.686);       // Classes (#f9e2af - Warm Sand)
			colors["text_editor/theme/highlighting/engine_type_color"] = Color(0.980, 0.702, 0.529);     // Built-in (#fab387)
			colors["text_editor/theme/highlighting/user_type_color"] = Color(0.976, 0.886, 0.686);
			colors["text_editor/theme/highlighting/comment_color"] = Color(0.424, 0.439, 0.525);         // Muted Lavender Slate (#6c7086)
			colors["text_editor/theme/highlighting/doc_comment_color"] = Color(0.55, 0.57, 0.67, 0.9);
			colors["text_editor/theme/highlighting/string_color"] = Color(0.651, 0.890, 0.631);          // Strings (#a6e3a1 - Mint)
			colors["text_editor/theme/highlighting/string_placeholder_color"] = Color(0.980, 0.702, 0.529);
			colors["text_editor/theme/highlighting/number_color"] = Color(0.980, 0.702, 0.529);          // Numbers (#fab387)
			colors["text_editor/theme/highlighting/function_color"] = Color(0.537, 0.706, 0.980);        // Functions (#89b4fa - Sky Blue)
			colors["text_editor/theme/highlighting/member_variable_color"] = Color(0.965, 0.765, 0.859); // Variables (#f5c2e7)

			// Canvas & Interface
			colors["text_editor/theme/highlighting/background_color"] = Color(0.094, 0.094, 0.145);      // Crust Background (#181825)
			colors["text_editor/theme/highlighting/completion_background_color"] = Color(0.12, 0.12, 0.18);
			colors["text_editor/theme/highlighting/completion_selected_color"] = Color(0.24, 0.25, 0.35, 0.7);
			colors["text_editor/theme/highlighting/completion_existing_color"] = Color(0.89, 0.71, 0.98, 0.2);
			colors["text_editor/theme/highlighting/completion_scroll_color"] = Color(1, 1, 1, 0.3);
			colors["text_editor/theme/highlighting/completion_scroll_hovered_color"] = Color(1, 1, 1, 0.5);
			colors["text_editor/theme/highlighting/completion_font_color"] = Color(0.85, 0.88, 0.95);
			colors["text_editor/theme/highlighting/text_color"] = Color(0.804, 0.839, 0.957);            // Crisp text (#cdd6f4)
			colors["text_editor/theme/highlighting/line_number_color"] = Color(0.424, 0.439, 0.525, 0.6);
			colors["text_editor/theme/highlighting/safe_line_number_color"] = Color(0.651, 0.890, 0.631, 0.8);
			colors["text_editor/theme/highlighting/caret_color"] = Color(0.961, 0.859, 0.961);
			colors["text_editor/theme/highlighting/caret_background_color"] = Color(0, 0, 0);
			colors["text_editor/theme/highlighting/text_selected_color"] = Color(0, 0, 0, 0);
			colors["text_editor/theme/highlighting/selection_color"] = Color(0.345, 0.392, 0.549, 0.45);
			colors["text_editor/theme/highlighting/brace_mismatch_color"] = Color(0.953, 0.545, 0.659);
			colors["text_editor/theme/highlighting/current_line_color"] = Color(0.20, 0.21, 0.30, 0.35);
			colors["text_editor/theme/highlighting/line_length_guideline_color"] = Color(0.3, 0.35, 0.5, 0.2);
			colors["text_editor/theme/highlighting/word_highlighted_color"] = Color(0.537, 0.706, 0.980, 0.2);
			colors["text_editor/theme/highlighting/mark_color"] = Color(0.953, 0.545, 0.659, 0.4);
			colors["text_editor/theme/highlighting/warning_color"] = Color(0.980, 0.702, 0.529, 0.2);
			colors["text_editor/theme/highlighting/bookmark_color"] = Color(0.537, 0.706, 0.980);
			colors["text_editor/theme/highlighting/breakpoint_color"] = Color(0.953, 0.545, 0.659);
			colors["text_editor/theme/highlighting/executing_line_color"] = Color(0.976, 0.886, 0.686, 0.7);
			colors["text_editor/theme/highlighting/code_folding_color"] = Color(0.7, 0.75, 0.9, 0.35);
			colors["text_editor/theme/highlighting/folded_code_region_color"] = Color(0.68, 0.46, 0.77, 0.25);
			colors["text_editor/theme/highlighting/search_result_color"] = Color(0.537, 0.706, 0.980, 0.3);
			colors["text_editor/theme/highlighting/search_result_border_color"] = Color(0.537, 0.706, 0.980, 0.85);
			colors["text_editor/theme/highlighting/warning_underline_color"] = Color(0.976, 0.886, 0.686);
			colors["text_editor/theme/highlighting/error_underline_color"] = Color(0.953, 0.545, 0.659);

			colors["text_editor/theme/highlighting/gdscript/function_definition_color"] = Color(0.537, 0.706, 0.980);
			colors["text_editor/theme/highlighting/gdscript/global_function_color"] = Color(0.557, 0.827, 0.925);
			colors["text_editor/theme/highlighting/gdscript/node_path_color"] = Color(0.965, 0.765, 0.859);
			colors["text_editor/theme/highlighting/gdscript/node_reference_color"] = Color(0.651, 0.890, 0.631);
			colors["text_editor/theme/highlighting/gdscript/annotation_color"] = Color(0.980, 0.702, 0.529);
			colors["text_editor/theme/highlighting/gdscript/string_name_color"] = Color(0.651, 0.890, 0.631);
			colors["text_editor/theme/highlighting/comment_markers/critical_color"] = Color(0.953, 0.545, 0.659);
			colors["text_editor/theme/highlighting/comment_markers/warning_color"] = Color(0.980, 0.702, 0.529);
			colors["text_editor/theme/highlighting/comment_markers/notice_color"] = Color(0.557, 0.827, 0.925);

		} else if (is_godot2_theme) {
			colors = EditorSettings::get_godot2_text_editor_theme();
		}

		EditorSettings *settings = EditorSettings::get_singleton();
		for (const KeyValue<StringName, Color> &setting : colors) {
			settings->set_initial_value(setting.key, setting.value);
			if (is_default_theme || is_godot2_theme) {
				settings->set_manually(setting.key, setting.value);
			}
		}
	} else {
		_load_text_editor_theme();
	}

	p_theme->set_font(SceneStringName(font), "CodeEdit", p_theme->get_font(SNAME("source"), EditorStringName(EditorFonts)));
	p_theme->set_font_size(SceneStringName(font_size), "CodeEdit", p_theme->get_font_size(SNAME("source_size"), EditorStringName(EditorFonts)));

	/* clang-format off */
	p_theme->set_icon("tab",                  "CodeEdit", p_theme->get_icon(SNAME("GuiTab"), EditorStringName(EditorIcons)));
	p_theme->set_icon("space",                "CodeEdit", p_theme->get_icon(SNAME("GuiSpace"), EditorStringName(EditorIcons)));
	p_theme->set_icon("folded",               "CodeEdit", p_theme->get_icon(SNAME("CodeFoldedRightArrow"), EditorStringName(EditorIcons)));
	p_theme->set_icon("can_fold",             "CodeEdit", p_theme->get_icon(SNAME("CodeFoldDownArrow"), EditorStringName(EditorIcons)));
	p_theme->set_icon("folded_code_region",   "CodeEdit", p_theme->get_icon(SNAME("CodeRegionFoldedRightArrow"), EditorStringName(EditorIcons)));
	p_theme->set_icon("can_fold_code_region", "CodeEdit", p_theme->get_icon(SNAME("CodeRegionFoldDownArrow"), EditorStringName(EditorIcons)));
	p_theme->set_icon("executing_line",       "CodeEdit", p_theme->get_icon(SNAME("TextEditorPlay"), EditorStringName(EditorIcons)));
	p_theme->set_icon("breakpoint",           "CodeEdit", p_theme->get_icon(SNAME("Breakpoint"), EditorStringName(EditorIcons)));
	/* clang-format on */

	p_theme->set_constant("line_spacing", "CodeEdit", EDITOR_GET("text_editor/appearance/whitespace/line_spacing"));

	const Color background_color = EDITOR_GET("text_editor/theme/highlighting/background_color");
	Ref<StyleBoxFlat> code_edit_stylebox = make_flat_stylebox(background_color, 0, p_config.widget_margin.y, p_config.widget_margin.x, p_config.widget_margin.y, p_config.corner_radius);
	p_theme->set_stylebox(CoreStringName(normal), "CodeEdit", code_edit_stylebox);
	p_theme->set_stylebox("read_only", "CodeEdit", code_edit_stylebox);
	p_theme->set_stylebox("focus", "CodeEdit", memnew(StyleBoxEmpty));

	/* clang-format off */
	p_theme->set_color("completion_background_color",     "CodeEdit", EDITOR_GET("text_editor/theme/highlighting/completion_background_color"));
	p_theme->set_color("completion_selected_color",       "CodeEdit", EDITOR_GET("text_editor/theme/highlighting/completion_selected_color"));
	p_theme->set_color("completion_existing_color",       "CodeEdit", EDITOR_GET("text_editor/theme/highlighting/completion_existing_color"));
	p_theme->set_color("completion_scroll_color",         "CodeEdit", EDITOR_GET("text_editor/theme/highlighting/completion_scroll_color"));
	p_theme->set_color("completion_scroll_hovered_color", "CodeEdit", EDITOR_GET("text_editor/theme/highlighting/completion_scroll_hovered_color"));
	p_theme->set_color(SceneStringName(font_color),                      "CodeEdit", EDITOR_GET("text_editor/theme/highlighting/text_color"));
	p_theme->set_color("line_number_color",               "CodeEdit", EDITOR_GET("text_editor/theme/highlighting/line_number_color"));
	p_theme->set_color("caret_color",                     "CodeEdit", EDITOR_GET("text_editor/theme/highlighting/caret_color"));
	p_theme->set_color("font_selected_color",             "CodeEdit", EDITOR_GET("text_editor/theme/highlighting/text_selected_color"));
	p_theme->set_color("selection_color",                 "CodeEdit", EDITOR_GET("text_editor/theme/highlighting/selection_color"));
	p_theme->set_color("brace_mismatch_color",            "CodeEdit", EDITOR_GET("text_editor/theme/highlighting/brace_mismatch_color"));
	p_theme->set_color("current_line_color",              "CodeEdit", EDITOR_GET("text_editor/theme/highlighting/current_line_color"));
	p_theme->set_color("line_length_guideline_color",     "CodeEdit", EDITOR_GET("text_editor/theme/highlighting/line_length_guideline_color"));
	p_theme->set_color("word_highlighted_color",          "CodeEdit", EDITOR_GET("text_editor/theme/highlighting/word_highlighted_color"));
	p_theme->set_color("bookmark_color",                  "CodeEdit", EDITOR_GET("text_editor/theme/highlighting/bookmark_color"));
	p_theme->set_color("breakpoint_color",                "CodeEdit", EDITOR_GET("text_editor/theme/highlighting/breakpoint_color"));
	p_theme->set_color("executing_line_color",            "CodeEdit", EDITOR_GET("text_editor/theme/highlighting/executing_line_color"));
	p_theme->set_color("code_folding_color",              "CodeEdit", EDITOR_GET("text_editor/theme/highlighting/code_folding_color"));
	p_theme->set_color("folded_code_region_color",        "CodeEdit", EDITOR_GET("text_editor/theme/highlighting/folded_code_region_color"));
	p_theme->set_color("search_result_color",             "CodeEdit", EDITOR_GET("text_editor/theme/highlighting/search_result_color"));
	p_theme->set_color("search_result_border_color",      "CodeEdit", EDITOR_GET("text_editor/theme/highlighting/search_result_border_color"));
	/* clang-format on */
}

void EditorThemeManager::_populate_visual_shader_styles(const Ref<EditorTheme> &p_theme, ThemeConfiguration &p_config) {
	EditorSettings *ed_settings = EditorSettings::get_singleton();
	String visual_shader_color_theme = ed_settings->get("editors/visual_editors/color_theme");
	if (visual_shader_color_theme == "Default") {
		// Connection type colors (Modern Neon Wires)
		ed_settings->set_initial_value("editors/visual_editors/connection_colors/scalar_color", Color(0.55, 0.60, 0.70), true);
		ed_settings->set_initial_value("editors/visual_editors/connection_colors/vector2_color", Color(0.557, 0.827, 0.925), true);
		ed_settings->set_initial_value("editors/visual_editors/connection_colors/vector3_color", Color(0.537, 0.706, 0.980), true);
		ed_settings->set_initial_value("editors/visual_editors/connection_colors/vector4_color", Color(0.796, 0.651, 0.969), true);
		ed_settings->set_initial_value("editors/visual_editors/connection_colors/boolean_color", Color(0.651, 0.890, 0.631), true);
		ed_settings->set_initial_value("editors/visual_editors/connection_colors/transform_color", Color(0.965, 0.765, 0.859), true);
		ed_settings->set_initial_value("editors/visual_editors/connection_colors/sampler_color", Color(0.980, 0.702, 0.529), true);

		// Node category colors (used for the node headers)
		ed_settings->set_initial_value("editors/visual_editors/category_colors/output_color", Color(0.30, 0.12, 0.18), true);
		ed_settings->set_initial_value("editors/visual_editors/category_colors/color_color", Color(0.55, 0.50, 0.15), true);
		ed_settings->set_initial_value("editors/visual_editors/category_colors/conditional_color", Color(0.20, 0.55, 0.32), true);
		ed_settings->set_initial_value("editors/visual_editors/category_colors/input_color", Color(0.50, 0.22, 0.24), true);
		ed_settings->set_initial_value("editors/visual_editors/category_colors/scalar_color", Color(0.12, 0.52, 0.65), true);
		ed_settings->set_initial_value("editors/visual_editors/category_colors/textures_color", Color(0.55, 0.35, 0.15), true);
		ed_settings->set_initial_value("editors/visual_editors/category_colors/transform_color", Color(0.52, 0.32, 0.55), true);
		ed_settings->set_initial_value("editors/visual_editors/category_colors/utility_color", Color(0.22, 0.22, 0.28), true);
		ed_settings->set_initial_value("editors/visual_editors/category_colors/vector_color", Color(0.22, 0.30, 0.55), true);
		ed_settings->set_initial_value("editors/visual_editors/category_colors/special_color", Color(0.12, 0.40, 0.32), true);
		ed_settings->set_initial_value("editors/visual_editors/category_colors/particle_color", Color(0.15, 0.40, 0.85), true);

	} else if (visual_shader_color_theme == "Legacy") {
		ed_settings->set_initial_value("editors/visual_editors/connection_colors/scalar_color", Color(0.38, 0.85, 0.96), true);
		ed_settings->set_initial_value("editors/visual_editors/connection_colors/vector2_color", Color(0.74, 0.57, 0.95), true);
		ed_settings->set_initial_value("editors/visual_editors/connection_colors/vector3_color", Color(0.84, 0.49, 0.93), true);
		ed_settings->set_initial_value("editors/visual_editors/connection_colors/vector4_color", Color(1.0, 0.125, 0.95), true);
		ed_settings->set_initial_value("editors/visual_editors/connection_colors/boolean_color", Color(0.55, 0.65, 0.94), true);
		ed_settings->set_initial_value("editors/visual_editors/connection_colors/transform_color", Color(0.96, 0.66, 0.43), true);
		ed_settings->set_initial_value("editors/visual_editors/connection_colors/sampler_color", Color(1.0, 1.0, 0.0), true);

		Ref<StyleBoxFlat> gn_panel_style = p_theme->get_stylebox(SceneStringName(panel), "GraphNode");
		Color gn_bg_color = gn_panel_style->get_bg_color();
		ed_settings->set_initial_value("editors/visual_editors/category_colors/output_color", gn_bg_color, true);
		ed_settings->set_initial_value("editors/visual_editors/category_colors/color_color", gn_bg_color, true);
		ed_settings->set_initial_value("editors/visual_editors/category_colors/conditional_color", gn_bg_color, true);
		ed_settings->set_initial_value("editors/visual_editors/category_colors/input_color", gn_bg_color, true);
		ed_settings->set_initial_value("editors/visual_editors/category_colors/scalar_color", gn_bg_color, true);
		ed_settings->set_initial_value("editors/visual_editors/category_colors/textures_color", gn_bg_color, true);
		ed_settings->set_initial_value("editors/visual_editors/category_colors/transform_color", gn_bg_color, true);
		ed_settings->set_initial_value("editors/visual_editors/category_colors/utility_color", gn_bg_color, true);
		ed_settings->set_initial_value("editors/visual_editors/category_colors/vector_color", gn_bg_color, true);
		ed_settings->set_initial_value("editors/visual_editors/category_colors/special_color", gn_bg_color, true);
		ed_settings->set_initial_value("editors/visual_editors/category_colors/particle_color", gn_bg_color, true);
	}
}

void EditorThemeManager::_reset_dirty_flag() {
	outdated_cache_dirty = true;
}

// Public interface for theme generation.

Ref<EditorTheme> EditorThemeManager::generate_theme(const Ref<EditorTheme> &p_old_theme) {
	OS::get_singleton()->benchmark_begin_measure(get_benchmark_key(), "Generate Theme");

	Ref<EditorTheme> theme = _create_base_theme(p_old_theme);

	OS::get_singleton()->benchmark_begin_measure(get_benchmark_key(), "Merge Custom Theme");

	const String custom_theme_path = EDITOR_GET("interface/theme/custom_theme");
	if (!custom_theme_path.is_empty()) {
		Ref<Theme> custom_theme = ResourceLoader::load(custom_theme_path);
		if (custom_theme.is_valid()) {
			theme->merge_with(custom_theme);
		}
	}

	OS::get_singleton()->benchmark_end_measure(get_benchmark_key(), "Merge Custom Theme");

	OS::get_singleton()->benchmark_end_measure(get_benchmark_key(), "Generate Theme");
	benchmark_run++;

	return theme;
}

bool EditorThemeManager::is_generated_theme_outdated() {
	if (outdated_cache_dirty) {
		outdated_cache = EditorSettings::get_singleton()->check_changed_settings_in_group("interface/theme") ||
				EditorSettings::get_singleton()->check_changed_settings_in_group("interface/editor/fonts") ||
				EditorSettings::get_singleton()->check_changed_settings_in_group("interface/editor/appearance/max_sticky_tree_items") ||
				EditorSettings::get_singleton()->check_changed_settings_in_group("interface/touchscreen/enable_touch_optimizations") ||
				EditorSettings::get_singleton()->check_changed_settings_in_group("editors/visual_editors") ||
				EditorSettings::get_singleton()->check_changed_settings_in_group("text_editor/theme") ||
				EditorSettings::get_singleton()->check_changed_settings_in_group("text_editor/help/help") ||
				EditorSettings::get_singleton()->check_changed_settings_in_group("docks/property_editor/subresource_hue_tint") ||
				EditorSettings::get_singleton()->check_changed_settings_in_group("filesystem/file_dialog/thumbnail_size") ||
				EditorSettings::get_singleton()->check_changed_settings_in_group("run/output/font_size");

		callable_mp_static(&EditorThemeManager::_reset_dirty_flag).call_deferred();
		outdated_cache_dirty = false;
	}

	return outdated_cache;
}

bool EditorThemeManager::is_dark_theme() {
	Color base_color = EDITOR_GET("interface/theme/base_color");
	return base_color.get_luminance() < 0.5;
}

bool EditorThemeManager::is_dark_icon_and_font() {
	int icon_font_color_setting = EDITOR_GET("interface/theme/icon_and_font_color");
	if (icon_font_color_setting == ColorMode::AUTO_COLOR) {
		return is_dark_theme();
	}

	return icon_font_color_setting == ColorMode::LIGHT_COLOR;
}

void EditorThemeManager::initialize() {
	EditorColorMap::create();
	EditorTheme::initialize();
}

void EditorThemeManager::finalize() {
	EditorColorMap::finish();
	EditorTheme::finalize();
}
