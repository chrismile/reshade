/*
 * Copyright (C) 2014 Patrick Mours. All rights reserved.
 * License: https://github.com/crosire/reshade#license
 */

#include "version.h"
#include "dll_log.hpp"
#include "dll_config.hpp"
#include "addon_manager.hpp"
#include "runtime.hpp"
#include "runtime_objects.hpp"
#include "effect_parser.hpp"
#include "effect_codegen.hpp"
#include "effect_preprocessor.hpp"
#include "input.hpp"
#include "input_freepie.hpp"
#include <set>
#include <thread>
#include <algorithm>
#include <stb_image.h>
#include <stb_image_dds.h>
#include <stb_image_write.h>
#include <stb_image_resize.h>

static const uint32_t NUM_QUERY_FRAMES = 4;

extern volatile long g_network_traffic;

bool resolve_path(std::filesystem::path &path)
{
	std::error_code ec;
	// First convert path to an absolute path
	// Ignore the working directory and instead start relative paths at the DLL location
	if (!path.is_absolute())
		path = std::filesystem::absolute(g_reshade_base_path / path, ec);
	// Finally try to canonicalize the path too
	if (auto canonical_path = std::filesystem::canonical(path, ec); !ec)
		path = std::move(canonical_path);
	return !ec; // The canonicalization step fails if the path does not exist
}
bool resolve_preset_path(std::filesystem::path &path)
{
	// First make sure the extension matches, before diving into the file system
	if (const std::filesystem::path ext = path.extension();
		ext != L".ini" && ext != L".txt")
		return false;
	// A non-existent path is valid for a new preset
	// Otherwise ensure the file has a technique list, which should make it a preset
	return !resolve_path(path) || reshade::ini_file::load_cache(path).has({}, "Techniques");
}

static bool find_file(const std::vector<std::filesystem::path> &search_paths, std::filesystem::path &path)
{
	std::error_code ec;
	// Do not have to perform a search if the path is already absolute
	if (path.is_absolute())
		return std::filesystem::exists(path, ec);
	for (std::filesystem::path search_path : search_paths)
		// Append relative file path to absolute search path
		if (search_path /= path; resolve_path(search_path))
			return path = std::move(search_path), true;
	return false;
}
static std::vector<std::filesystem::path> find_files(const std::vector<std::filesystem::path> &search_paths, std::initializer_list<std::filesystem::path> extensions)
{
	std::error_code ec;
	std::vector<std::filesystem::path> files;
	for (std::filesystem::path search_path : search_paths)
		if (resolve_path(search_path))
			for (const std::filesystem::directory_entry &entry : std::filesystem::directory_iterator(search_path, std::filesystem::directory_options::skip_permission_denied, ec))
				if (!entry.is_directory(ec) &&
					std::find(extensions.begin(), extensions.end(), entry.path().extension()) != extensions.end())
					files.emplace_back(entry); // Construct path from directory entry in-place
	return files;
}

reshade::runtime::runtime() :
	_start_time(std::chrono::high_resolution_clock::now()),
	_last_present_time(std::chrono::high_resolution_clock::now()),
	_last_frame_duration(std::chrono::milliseconds(1)),
	_effect_search_paths({ L".\\" }),
	_texture_search_paths({ L".\\" }),
	_reload_key_data(),
	_performance_mode_key_data(),
	_effects_key_data(),
	_screenshot_key_data(),
	_prev_preset_key_data(),
	_next_preset_key_data(),
	_config_path(g_reshade_base_path / L"ReShade.ini"),
	_screenshot_path(g_reshade_base_path)
{
	_needs_update = check_for_update(_latest_version);

	// Default shortcut PrtScrn
	_screenshot_key_data[0] = 0x2C;

	// Fall back to alternative configuration file name if it exists
	std::error_code ec;
	if (std::filesystem::path config_path_alt = g_reshade_base_path / g_reshade_dll_path.filename().replace_extension(L".ini");
		std::filesystem::exists(config_path_alt, ec) && !std::filesystem::exists(_config_path, ec))
		_config_path = std::move(config_path_alt);

#if RESHADE_GUI
	init_gui();
#endif
	load_config();
}
reshade::runtime::~runtime()
{
	assert(_worker_threads.empty());
	assert(!_is_initialized && _techniques.empty());

#if RESHADE_GUI
	deinit_gui();
#endif
}

bool reshade::runtime::on_init(input::window_handle window)
{
	assert(!_is_initialized);

	if (window != nullptr)
		_input = input::register_window(window);
	else
		_input = std::make_shared<input>(nullptr);

	// Reset frame count to zero so effects are loaded in 'update_and_render_effects'
	_framecount = 0;

	_is_initialized = true;
	_last_reload_time = std::chrono::high_resolution_clock::now();

	_preset_save_success = true;
	_screenshot_save_success = true;

	api::device *const device = get_device();

	// Create back buffer shader resource
	if (_backbuffer_texture.handle == 0)
	{
		if (!device->create_resource(
			api::resource_desc(_width, _height, 1, 1, api::format_to_typeless(get_backbuffer_format()), 1, api::memory_heap::gpu_only, api::resource_usage::copy_dest | api::resource_usage::shader_resource),
			nullptr,
			api::resource_usage::shader_resource,
			&_backbuffer_texture))
			return false;
		device->set_debug_name(_backbuffer_texture, "ReShade back buffer");

		if (!device->create_resource_view(
			_backbuffer_texture,
			api::resource_usage::shader_resource,
			api::resource_view_desc(api::format_to_default_typed(get_backbuffer_format()), 0, 1, 0, 1),
			&_backbuffer_texture_view[0]))
			return false;
		if (!device->create_resource_view(
			_backbuffer_texture,
			api::resource_usage::shader_resource,
			api::resource_view_desc(api::format_to_default_typed_srgb(get_backbuffer_format()), 0, 1, 0, 1),
			&_backbuffer_texture_view[1]))
			return false;
	}

	// Create effect depth-stencil resource
	if (_effect_stencil.handle == 0)
	{
		// Find a supported stencil format
		api::format possible_stencil_formats[] = {
			api::format::s8_uint,
			api::format::d16_unorm_s8_uint,
			api::format::d24_unorm_s8_uint,
			api::format::d32_float_s8_uint
		};

		for (const api::format format : possible_stencil_formats)
		{
			if (device->check_format_support(format, api::resource_usage::depth_stencil))
			{
				_effect_stencil_format = format;
				break;
			}
		}

		assert(_effect_stencil_format != api::format::unknown);

		if (!device->create_resource(
			api::resource_desc(_width, _height, 1, 1, _effect_stencil_format, 1, api::memory_heap::gpu_only, api::resource_usage::depth_stencil),
			nullptr,
			api::resource_usage::depth_stencil_write,
			&_effect_stencil))
			return false;
		device->set_debug_name(_effect_stencil, "ReShade stencil buffer");

		if (!device->create_resource_view(
			_effect_stencil,
			api::resource_usage::depth_stencil,
			api::resource_view_desc(_effect_stencil_format, 0, 1, 0, 1),
			&_effect_stencil_view))
			return false;
	}

	// Create an empty image, which is used when no depth buffer was detected (since you cannot bind nothing to a descriptor in Vulkan)
	// Use VK_FORMAT_R16_SFLOAT format, since it is mandatory according to the spec (see https://www.khronos.org/registry/vulkan/specs/1.1/html/vkspec.html#features-required-format-support)
	if (_empty_texture.handle == 0)
	{
		if (!device->create_resource(
			api::resource_desc(1, 1, 1, 1, api::format::r16_float, 1, api::memory_heap::gpu_only, api::resource_usage::shader_resource),
			nullptr,
			api::resource_usage::shader_resource,
			&_empty_texture))
			return false;
		device->set_debug_name(_empty_texture, "ReShade empty texture");

		if (!device->create_resource_view(
			_empty_texture,
			api::resource_usage::shader_resource,
			api::resource_view_desc(api::format::r16_float, 0, 1, 0, 1),
			&_empty_texture_view))
			return false;
	}

#if RESHADE_GUI
	if (!init_imgui_resources())
		return false;
#endif

#if RESHADE_ADDON
	invoke_addon_event<addon_event::init_effect_runtime>(this);
#endif

	LOG(INFO) << "Recreated runtime environment on runtime " << this << '.';

	return true;
}
void reshade::runtime::on_reset()
{
	if (_is_initialized)
		// Update initialization state immediately, so that any effect loading still in progress can abort early
		_is_initialized = false;
	else
		return; // Nothing to do if the runtime was already destroyed or not successfully initialized in the first place

	unload_effects();

	_width = _height = 0;

	api::device *const device = get_device();

	device->wait_idle();

	device->destroy_resource(_backbuffer_texture);
	_backbuffer_texture = {};
	device->destroy_resource_view(_backbuffer_texture_view[0]);
	_backbuffer_texture_view[0] = {};
	device->destroy_resource_view(_backbuffer_texture_view[1]);
	_backbuffer_texture_view[1] = {};

	device->destroy_resource(_effect_stencil);
	_effect_stencil = {};
	device->destroy_resource_view(_effect_stencil_view);
	_effect_stencil_view = {};

	device->destroy_resource(_empty_texture);
	_empty_texture = {};
	device->destroy_resource_view(_empty_texture_view);
	_empty_texture_view = {};

#if RESHADE_GUI
	device->destroy_resource(_imgui.font_atlas);
	_imgui.font_atlas = {};
	device->destroy_resource_view(_imgui.font_atlas_view);
	_imgui.font_atlas_view = {};
	_rebuild_font_atlas = true;

	for (unsigned int i = 0; i < NUM_IMGUI_BUFFERS; ++i)
	{
		device->destroy_resource(_imgui.indices[i]);
		_imgui.indices[i] = {};
		_imgui.num_indices[i] = 0;
		device->destroy_resource(_imgui.vertices[i]);
		_imgui.vertices[i] = {};
		_imgui.num_vertices[i] = 0;
	}

	device->destroy_sampler(_imgui.sampler_state);
	_imgui.sampler_state = {};
	device->destroy_pipeline(api::pipeline_type::graphics, _imgui.pipeline);
	_imgui.pipeline = {};
	device->destroy_pipeline_layout(_imgui.pipeline_layout);
	_imgui.pipeline_layout = {};
	device->destroy_descriptor_set_layout(_imgui.table_layouts[0]);
	_imgui.table_layouts[0] = {};
	device->destroy_descriptor_set_layout(_imgui.table_layouts[1]);
	_imgui.table_layouts[1] = {};
#endif

#if RESHADE_ADDON
	invoke_addon_event<addon_event::destroy_effect_runtime>(this);
#endif

	LOG(INFO) << "Destroyed runtime environment on runtime " << this << '.';
}
void reshade::runtime::on_present()
{
	_framecount++;
	const auto current_time = std::chrono::high_resolution_clock::now();
	_last_frame_duration = current_time - _last_present_time;
	_last_present_time = current_time;

#ifdef NDEBUG
	// Lock input so it cannot be modified by other threads while we are reading it here
	const auto input_lock = _input->lock();
#endif

#if RESHADE_GUI
	// Draw overlay
	draw_gui();

	if (_should_save_screenshot && _screenshot_save_gui && (_show_overlay || (_preview_texture.handle != 0 && _effects_enabled)))
		save_screenshot(L" overlay");
#endif

	// All screenshots were created at this point, so reset request
	_should_save_screenshot = false;

	// Handle keyboard shortcuts
	if (!_ignore_shortcuts)
	{
		if (_input->is_key_pressed(_effects_key_data, _force_shortcut_modifiers))
			_effects_enabled = !_effects_enabled;

		if (_input->is_key_pressed(_screenshot_key_data, _force_shortcut_modifiers))
			_should_save_screenshot = true; // Notify 'update_and_render_effects' that we want to save a screenshot next frame

		// Do not allow the next shortcuts while effects are being loaded or compiled (since they affect that state)
		if (!is_loading() && _reload_compile_queue.empty())
		{
			if (_input->is_key_pressed(_reload_key_data, _force_shortcut_modifiers))
				reload_effects();

			if (_input->is_key_pressed(_performance_mode_key_data, _force_shortcut_modifiers))
			{
				_performance_mode = !_performance_mode;
				save_config();
				reload_effects();
			}

			if (const bool reversed = _input->is_key_pressed(_prev_preset_key_data, _force_shortcut_modifiers);
				reversed || _input->is_key_pressed(_next_preset_key_data, _force_shortcut_modifiers))
			{
				// The preset shortcut key was pressed down, so start the transition
				if (switch_to_next_preset(_current_preset_path.parent_path(), reversed))
				{
					_last_preset_switching_time = current_time;
					_is_in_between_presets_transition = true;
					save_config();
				}
			}

			// Continuously update preset values while a transition is in progress
			if (_is_in_between_presets_transition)
				load_current_preset();
		}
	}

	// Reset input status
	_input->next_frame();

	// Save modified INI files
	if (!ini_file::flush_cache())
		_preset_save_success = false;

#if RESHADE_ADDON
	// Detect high network traffic
	static int cooldown = 0, traffic = 0;
	if (cooldown-- > 0)
	{
		traffic += g_network_traffic > 0;
	}
	else
	{
		addon::enable_or_disable_addons(traffic < 10);
		traffic = 0;
		cooldown = 60;
	}
#endif

	// Reset frame statistics
	g_network_traffic = 0;
}

bool reshade::runtime::load_effect(const std::filesystem::path &source_file, const reshade::ini_file &preset, size_t effect_index, bool preprocess_required)
{
	std::string attributes;
	attributes += "app=" + g_target_executable_path.stem().u8string() + ';';
	attributes += "width=" + std::to_string(_width) + ';';
	attributes += "height=" + std::to_string(_height) + ';';
	attributes += "color_bit_depth=" + std::to_string(_color_bit_depth) + ';';
	attributes += "version=" + std::to_string(VERSION_MAJOR * 10000 + VERSION_MINOR * 100 + VERSION_REVISION) + ';';
	attributes += "performance_mode=" + std::string(_performance_mode ? "1" : "0") + ';';
	attributes += "vendor=" + std::to_string(_vendor_id) + ';';
	attributes += "device=" + std::to_string(_device_id) + ';';

	std::set<std::filesystem::path> include_paths;
	if (source_file.is_absolute())
		include_paths.emplace(source_file.parent_path());
	for (std::filesystem::path include_path : _effect_search_paths)
		if (resolve_path(include_path))
			include_paths.emplace(std::move(include_path));

	for (const std::filesystem::path &include_path : include_paths)
	{
		std::error_code ec;
		attributes += include_path.u8string();
		for (const auto &entry : std::filesystem::directory_iterator(include_path, std::filesystem::directory_options::skip_permission_denied, ec))
		{
			const std::filesystem::path filename = entry.path().filename();
			if (filename == source_file.filename() || filename.extension() == L".fxh")
			{
				attributes += ',';
				attributes += filename.u8string();
				attributes += '?';
				attributes += std::to_string(entry.last_write_time(ec).time_since_epoch().count());
			}
		}
		attributes += ';';
	}

	std::vector<std::string> preprocessor_definitions = _global_preprocessor_definitions;
	preprocessor_definitions.insert(preprocessor_definitions.end(), _preset_preprocessor_definitions.begin(), _preset_preprocessor_definitions.end());
	for (const std::string &definition : preprocessor_definitions)
		attributes += definition + ';';

	const size_t source_hash = std::hash<std::string>()(attributes);

	effect &effect = _effects[effect_index];
	const std::string effect_name = source_file.filename().u8string();
	if (source_file != effect.source_file || source_hash != effect.source_hash)
	{
		effect = {};
		effect.source_file = source_file;
		effect.source_hash = source_hash;
	}

	if (_effect_load_skipping && !_load_option_disable_skipping && !_worker_threads.empty()) // Only skip during 'load_effects'
	{
		if (std::vector<std::string> techniques;
			preset.get({}, "Techniques", techniques))
		{
			effect.skipped = std::find_if(techniques.cbegin(), techniques.cend(), [&effect_name](const std::string &technique) {
				const size_t at_pos = technique.find('@') + 1;
				return at_pos == 0 || technique.find(effect_name, at_pos) == at_pos; }) == techniques.cend();

			if (effect.skipped)
			{
				if (_reload_remaining_effects != 0 && _reload_remaining_effects != std::numeric_limits<size_t>::max())
					_reload_remaining_effects--;
				return false;
			}
		}
	}

	bool source_cached = false; std::string source;
	if (!effect.preprocessed && (preprocess_required || (source_cached = load_effect_cache(source_file, source_hash, source)) == false))
	{
		reshadefx::preprocessor pp;
		pp.add_macro_definition("__RESHADE__", std::to_string(VERSION_MAJOR * 10000 + VERSION_MINOR * 100 + VERSION_REVISION));
		pp.add_macro_definition("__RESHADE_PERFORMANCE_MODE__", _performance_mode ? "1" : "0");
		pp.add_macro_definition("__VENDOR__", std::to_string(_vendor_id));
		pp.add_macro_definition("__DEVICE__", std::to_string(_device_id));
		pp.add_macro_definition("__RENDERER__", std::to_string(_renderer_id));
		pp.add_macro_definition("__APPLICATION__", std::to_string( // Truncate hash to 32-bit, since lexer currently only supports 32-bit numbers anyway
			std::hash<std::string>()(g_target_executable_path.stem().u8string()) & 0xFFFFFFFF));
		pp.add_macro_definition("BUFFER_WIDTH", std::to_string(_width));
		pp.add_macro_definition("BUFFER_HEIGHT", std::to_string(_height));
		pp.add_macro_definition("BUFFER_RCP_WIDTH", "(1.0 / BUFFER_WIDTH)");
		pp.add_macro_definition("BUFFER_RCP_HEIGHT", "(1.0 / BUFFER_HEIGHT)");
		pp.add_macro_definition("BUFFER_COLOR_BIT_DEPTH", std::to_string(_color_bit_depth));

		for (const std::string &definition : preprocessor_definitions)
		{
			if (definition.empty() || definition == "=")
				continue; // Skip invalid definitions

			const size_t equals_index = definition.find('=');
			if (equals_index != std::string::npos)
				pp.add_macro_definition(
					definition.substr(0, equals_index),
					definition.substr(equals_index + 1));
			else
				pp.add_macro_definition(definition);
		}

		for (const std::filesystem::path &include_path : include_paths)
			pp.add_include_path(include_path);

		// Add some conversion macros for compatibility with older versions of ReShade
		pp.append_string(
			"#define tex2Doffset(s, coords, offset) tex2D(s, coords, offset)\n"
			"#define tex2Dlodoffset(s, coords, offset) tex2Dlod(s, coords, offset)\n"
			"#define tex2Dgather(s, t, c) tex2Dgather##c(s, t)\n"
			"#define tex2Dgatheroffset(s, t, o, c) tex2Dgather##c(s, t, o)\n"
			"#define tex2Dgather0 tex2DgatherR\n"
			"#define tex2Dgather1 tex2DgatherG\n"
			"#define tex2Dgather2 tex2DgatherB\n"
			"#define tex2Dgather3 tex2DgatherA\n");

		// Load and preprocess the source file
		effect.preprocessed = pp.append_file(source_file);

		// Append preprocessor errors to the error list
		effect.errors      += pp.errors();

		if (effect.preprocessed)
		{
			source = std::move(pp.output());
			source_cached = save_effect_cache(source_file, source_hash, source);

			// Keep track of used preprocessor definitions (so they can be displayed in the overlay)
			effect.definitions.clear();
			for (const auto &definition : pp.used_macro_definitions())
			{
				if (definition.first.size() <= 10 || definition.first[0] == '_' || !definition.first.compare(0, 8, "RESHADE_") || !definition.first.compare(0, 7, "BUFFER_"))
					continue;

				effect.definitions.emplace_back(definition.first, trim(definition.second));
			}

			std::sort(effect.definitions.begin(), effect.definitions.end());

			// Keep track of included files
			effect.included_files = pp.included_files();
			std::sort(effect.included_files.begin(), effect.included_files.end()); // Sort file names alphabetically
		}
	}

	if (!effect.compiled && !source.empty())
	{
		unsigned shader_model;
		if (_renderer_id == 0x9000)
			shader_model = 30; // D3D9
		else if (_renderer_id < 0xa100)
			shader_model = 40; // D3D10 (including feature level 9)
		else if (_renderer_id < 0xb000)
			shader_model = 41; // D3D10.1
		else if (_renderer_id < 0xc000)
			shader_model = 50; // D3D11
		else
			shader_model = 51; // D3D12

		std::unique_ptr<reshadefx::codegen> codegen;
		if ((_renderer_id & 0xF0000) == 0)
			codegen.reset(reshadefx::create_codegen_hlsl(shader_model, !_no_debug_info, _performance_mode));
		else if (_renderer_id < 0x20000)
			codegen.reset(reshadefx::create_codegen_glsl(!_no_debug_info, _performance_mode, false, true));
		else // Vulkan uses SPIR-V input
			codegen.reset(reshadefx::create_codegen_spirv(true, !_no_debug_info, _performance_mode, false, false));

		reshadefx::parser parser;

		// Compile the pre-processed source code (try the compile even if the preprocessor step failed to get additional error information)
		effect.compiled = parser.parse(std::move(source), codegen.get());

		// Append parser errors to the error list
		effect.errors  += parser.errors();

		// Write result to effect module
		codegen->write_result(effect.module);

		if (effect.compiled)
		{
			effect.uniforms.clear();

			// Create space for all variables (aligned to 16 bytes)
			effect.uniform_data_storage.resize((effect.module.total_uniform_size + 15) & ~15);

			for (uniform variable : effect.module.uniforms)
			{
				variable.effect_index = effect_index;

				// Copy initial data into uniform storage area
				reset_uniform_value(variable);

				const std::string_view special = variable.annotation_as_string("source");
				if (special.empty()) /* Ignore if annotation is missing */;
				else if (special == "frametime")
					variable.special = special_uniform::frame_time;
				else if (special == "framecount")
					variable.special = special_uniform::frame_count;
				else if (special == "random")
					variable.special = special_uniform::random;
				else if (special == "pingpong")
					variable.special = special_uniform::ping_pong;
				else if (special == "date")
					variable.special = special_uniform::date;
				else if (special == "timer")
					variable.special = special_uniform::timer;
				else if (special == "key")
					variable.special = special_uniform::key;
				else if (special == "mousepoint")
					variable.special = special_uniform::mouse_point;
				else if (special == "mousedelta")
					variable.special = special_uniform::mouse_delta;
				else if (special == "mousebutton")
					variable.special = special_uniform::mouse_button;
				else if (special == "mousewheel")
					variable.special = special_uniform::mouse_wheel;
				else if (special == "freepie")
					variable.special = special_uniform::freepie;
				else if (special == "ui_open" ||special == "overlay_open")
					variable.special = special_uniform::overlay_open;
				else if (special == "ui_active" || special == "overlay_active")
					variable.special = special_uniform::overlay_active;
				else if (special == "ui_hovered" || special == "overlay_hovered")
					variable.special = special_uniform::overlay_hovered;

				effect.uniforms.push_back(std::move(variable));
			}

			// Fill all specialization constants with values from the current preset
			if (_performance_mode)
			{
				effect.preamble.clear();

				for (reshadefx::uniform_info &constant : effect.module.spec_constants)
				{
					effect.preamble += "#define SPEC_CONSTANT_" + constant.name + ' ';

					switch (constant.type.base)
					{
					case reshadefx::type::t_int:
						preset.get(effect_name, constant.name, constant.initializer_value.as_int);
						break;
					case reshadefx::type::t_bool:
					case reshadefx::type::t_uint:
						preset.get(effect_name, constant.name, constant.initializer_value.as_uint);
						break;
					case reshadefx::type::t_float:
						preset.get(effect_name, constant.name, constant.initializer_value.as_float);
						break;
					}

					// Check if this is a split specialization constant and move data accordingly
					if (constant.type.is_scalar() && constant.offset != 0)
						constant.initializer_value.as_uint[0] = constant.initializer_value.as_uint[constant.offset];

					for (unsigned int i = 0; i < constant.type.components(); ++i)
					{
						switch (constant.type.base)
						{
						case reshadefx::type::t_bool:
							effect.preamble += constant.initializer_value.as_uint[i] ? "true" : "false";
							break;
						case reshadefx::type::t_int:
							effect.preamble += std::to_string(constant.initializer_value.as_int[i]);
							break;
						case reshadefx::type::t_uint:
							effect.preamble += std::to_string(constant.initializer_value.as_uint[i]);
							break;
						case reshadefx::type::t_float:
							effect.preamble += std::to_string(constant.initializer_value.as_float[i]);
							break;
						}

						if (i + 1 < constant.type.components())
							effect.preamble += ", ";
					}

					effect.preamble += '\n';
				}
			}
		}
	}

	if ( effect.compiled && (effect.preprocessed || source_cached))
	{
		const std::lock_guard<std::mutex> lock(_reload_mutex);

		for (texture texture : effect.module.textures)
		{
			texture.effect_index = effect_index;

			// Try to share textures with the same name across effects
			if (const auto existing_texture = std::find_if(_textures.begin(), _textures.end(),
				[&texture](const auto &item) { return item.unique_name == texture.unique_name; });
				existing_texture != _textures.end())
			{
				// Cannot share texture if this is a normal one, but the existing one is a reference and vice versa
				if (texture.semantic != existing_texture->semantic)
				{
					effect.errors += "error: " + texture.unique_name + ": another effect (";
					effect.errors += _effects[existing_texture->effect_index].source_file.filename().u8string();
					effect.errors += ") already created a texture with the same name but different semantic\n";
					effect.compiled = false;
					break;
				}

				if (texture.semantic.empty() && !existing_texture->matches_description(texture))
				{
					effect.errors += "warning: " + texture.unique_name + ": another effect (";
					effect.errors += _effects[existing_texture->effect_index].source_file.filename().u8string();
					effect.errors += ") already created a texture with the same name but different dimensions\n";
				}
				if (texture.semantic.empty() && (existing_texture->annotation_as_string("source") != texture.annotation_as_string("source")))
				{
					effect.errors += "warning: " + texture.unique_name + ": another effect (";
					effect.errors += _effects[existing_texture->effect_index].source_file.filename().u8string();
					effect.errors += ") already created a texture with a different image file\n";
				}

				if (existing_texture->semantic == "COLOR" && _color_bit_depth != 8)
				{
					for (const auto &sampler_info : effect.module.samplers)
					{
						if (sampler_info.srgb && sampler_info.texture_name == texture.unique_name)
						{
							effect.errors += "warning: " + sampler_info.unique_name + ": texture does not support sRGB sampling (back buffer format is not RGBA8)";
						}
					}
				}

				if (std::find(existing_texture->shared.begin(), existing_texture->shared.end(), effect_index) == existing_texture->shared.end())
					existing_texture->shared.push_back(effect_index);

				// Always make shared textures render targets, since they may be used as such in a different effect
				existing_texture->render_target = true;
				existing_texture->storage_access = true;
				continue;
			}

			if (texture.annotation_as_int("pooled") && texture.semantic.empty())
			{
				// Try to find another pooled texture to share with (and do not share within the same effect)
				if (const auto existing_texture = std::find_if(_textures.begin(), _textures.end(),
					[&texture](const auto &item) { return item.annotation_as_int("pooled") && item.effect_index != texture.effect_index && item.matches_description(texture); });
					existing_texture != _textures.end())
				{
					// Overwrite referenced texture in samplers with the pooled one
					for (auto &sampler_info : effect.module.samplers)
						if (sampler_info.texture_name == texture.unique_name)
							sampler_info.texture_name  = existing_texture->unique_name;
					// Overwrite referenced texture in storages with the pooled one
					for (auto &storage_info : effect.module.storages)
						if (storage_info.texture_name == texture.unique_name)
							storage_info.texture_name  = existing_texture->unique_name;
					// Overwrite referenced texture in render targets with the pooled one
					for (auto &technique_info : effect.module.techniques)
					{
						for (auto &pass_info : technique_info.passes)
						{
							std::replace(std::begin(pass_info.render_target_names), std::end(pass_info.render_target_names),
								texture.unique_name, existing_texture->unique_name);

							for (auto &sampler_info : pass_info.samplers)
								if (sampler_info.texture_name == texture.unique_name)
									sampler_info.texture_name  = existing_texture->unique_name;
							for (auto &storage_info : pass_info.storages)
								if (storage_info.texture_name == texture.unique_name)
									storage_info.texture_name  = existing_texture->unique_name;
						}
					}

					if (std::find(existing_texture->shared.begin(), existing_texture->shared.end(), effect_index) == existing_texture->shared.end())
						existing_texture->shared.push_back(effect_index);

					existing_texture->render_target = true;
					existing_texture->storage_access = true;
					continue;
				}
			}

			if (!texture.semantic.empty() && (texture.semantic != "COLOR" && texture.semantic != "DEPTH"))
				effect.errors += "warning: " + texture.unique_name + ": unknown semantic '" + texture.semantic + "'\n";

			// This is the first effect using this texture
			texture.shared.push_back(effect_index);

			_textures.push_back(std::move(texture));
		}

		for (technique technique : effect.module.techniques)
		{
			technique.effect_index = effect_index;

			technique.hidden = technique.annotation_as_int("hidden") != 0;

			if (technique.annotation_as_int("enabled"))
				enable_technique(technique);

			_techniques.push_back(std::move(technique));
		}
	}

	if (_reload_remaining_effects != 0 && _reload_remaining_effects != std::numeric_limits<size_t>::max())
		_reload_remaining_effects--;
	else
		_reload_remaining_effects = 0; // Force effect initialization in 'update_and_render_effects'

	if ( effect.compiled && (effect.preprocessed || source_cached))
	{
		if (effect.errors.empty())
			LOG(INFO) << "Successfully loaded " << source_file << '.';
		else
			LOG(WARN) << "Successfully loaded " << source_file << " with warnings:\n" << effect.errors;
		return true;
	}
	else
	{
		_last_reload_successfull = false;

		if (effect.errors.empty())
			LOG(ERROR) << "Failed to load " << source_file << '!';
		else
			LOG(ERROR) << "Failed to load " << source_file << ":\n" << effect.errors;
		return false;
	}
}
void reshade::runtime::load_effects()
{
	// Reload preprocessor definitions from current preset before compiling
	_preset_preprocessor_definitions.clear();
	ini_file &preset = ini_file::load_cache(_current_preset_path);
	preset.get({}, "PreprocessorDefinitions", _preset_preprocessor_definitions);

	// Build a list of effect files by walking through the effect search paths
	const std::vector<std::filesystem::path> effect_files =
		find_files(_effect_search_paths, { L".fx" });

	if (effect_files.empty())
		return; // No effect files found, so nothing more to do

	// Have to be initialized at this point or else the threads spawned below will immediately exit without reducing the remaining effects count
	assert(_is_initialized);

	// Allocate space for effects which are placed in this array during the 'load_effect' call
	const size_t offset = _effects.size();
	_effects.resize(offset + effect_files.size());
	_reload_remaining_effects = effect_files.size();

	// Now that we have a list of files, load them in parallel
	// Split workload into batches instead of launching a thread for every file to avoid launch overhead and stutters due to too many threads being in flight
	const size_t num_splits = std::min<size_t>(effect_files.size(), std::max<size_t>(std::thread::hardware_concurrency(), 2u) - 1);

	// Keep track of the spawned threads, so the runtime cannot be destroyed while they are still running
	for (size_t n = 0; n < num_splits; ++n)
		// Create copy of preset instead of reference, so it stays valid even if 'ini_file::load_cache' is called while effects are still being loaded
		_worker_threads.emplace_back([this, effect_files, offset, num_splits, n, preset]() {
			// Abort loading when initialization state changes (indicating that 'on_reset' was called in the meantime)
			for (size_t i = 0; i < effect_files.size() && _is_initialized; ++i)
				if (i * num_splits / effect_files.size() == n)
					load_effect(effect_files[i], preset, offset + i);
		});
}
void reshade::runtime::load_textures()
{
	_last_texture_reload_successfull = true;

	LOG(INFO) << "Loading image files for textures ...";

	for (texture &texture : _textures)
	{
		if (texture.resource.handle == 0 || !texture.semantic.empty())
			continue; // Ignore textures that are not created yet and those that are handled in the runtime implementation

		std::filesystem::path source_path = std::filesystem::u8path(
			texture.annotation_as_string("source"));
		// Ignore textures that have no image file attached to them (e.g. plain render targets)
		if (source_path.empty())
			continue;

		// Search for image file using the provided search paths unless the path provided is already absolute
		if (!find_file(_texture_search_paths, source_path))
		{
			LOG(ERROR) << "Source " << source_path << " for texture '" << texture.unique_name << "' could not be found in any of the texture search paths!";
			_last_texture_reload_successfull = false;
			continue;
		}

		unsigned char *filedata = nullptr;
		int width = 0, height = 0, channels = 0;

		if (FILE *file; _wfopen_s(&file, source_path.c_str(), L"rb") == 0)
		{
			// Read texture data into memory in one go since that is faster than reading chunk by chunk
			std::vector<uint8_t> mem(static_cast<size_t>(std::filesystem::file_size(source_path)));
			fread(mem.data(), 1, mem.size(), file);
			fclose(file);

			if (stbi_dds_test_memory(mem.data(), static_cast<int>(mem.size())))
				filedata = stbi_dds_load_from_memory(mem.data(), static_cast<int>(mem.size()), &width, &height, &channels, STBI_rgb_alpha);
			else
				filedata = stbi_load_from_memory(mem.data(), static_cast<int>(mem.size()), &width, &height, &channels, STBI_rgb_alpha);
		}

		if (filedata == nullptr)
		{
			LOG(ERROR) << "Source " << source_path << " for texture '" << texture.unique_name << "' could not be loaded! Make sure it is of a compatible file format.";
			_last_texture_reload_successfull = false;
			continue;
		}

		// Need to potentially resize image data to the texture dimensions
		std::vector<uint8_t> resized(texture.width * texture.height * 4);
		if (texture.width != uint32_t(width) || texture.height != uint32_t(height))
		{
			LOG(INFO) << "Resizing image data for texture '" << texture.unique_name << "' from " << width << "x" << height << " to " << texture.width << "x" << texture.height << " ...";

			stbir_resize_uint8(filedata, width, height, 0, resized.data(), texture.width, texture.height, 0, 4);
		}
		else
		{
			std::memcpy(resized.data(), filedata, resized.size());
		}

		stbi_image_free(filedata);

		// Collapse data to the correct number of components per pixel based on the texture format
		uint32_t row_pitch = texture.width;
		switch (texture.format)
		{
		case reshadefx::texture_format::r8:
			for (size_t i = 4, k = 1; i < resized.size(); i += 4, k += 1)
				resized[k] = resized[i];
			break;
		case reshadefx::texture_format::rg8:
			for (size_t i = 4, k = 2; i < resized.size(); i += 4, k += 2)
				resized[k + 0] = resized[i + 0],
				resized[k + 1] = resized[i + 1];
			row_pitch *= 2;
			break;
		case reshadefx::texture_format::rgba8:
			row_pitch *= 4;
			break;
		default:
			LOG(ERROR) << "Texture upload is not supported for format " << static_cast<unsigned int>(texture.format) << " of texture '" << texture.unique_name << "'!";
			continue;
		}

		api::command_list *const cmd_list = get_command_queue()->get_immediate_command_list();
		cmd_list->barrier(texture.resource, api::resource_usage::shader_resource, api::resource_usage::copy_dest);
		get_device()->upload_texture_region({ resized.data(), row_pitch, row_pitch * texture.height }, texture.resource, 0);
		cmd_list->barrier(texture.resource, api::resource_usage::copy_dest, api::resource_usage::shader_resource);

		if (texture.levels > 1)
			cmd_list->generate_mipmaps(texture.srv[0]);

		texture.loaded = true;
	}

	_textures_loaded = true;
}

bool reshade::runtime::init_effect(size_t effect_index)
{
	api::device *const device = get_device();

	effect &effect = _effects[effect_index];

	// Compile shader modules
	api::shader_format shader_format = _renderer_id & 0x10000 ? api::shader_format::glsl : _renderer_id & 0x20000 ? api::shader_format::spirv : api::shader_format::dxbc;
	std::unordered_map<std::string, std::vector<char>> entry_points;

	for (const reshadefx::entry_point &entry_point : effect.module.entry_points)
	{
		api::shader_stage type = api::shader_stage::all;
		switch (entry_point.type)
		{
		case reshadefx::shader_type::vs:
			type = api::shader_stage::vertex;
			break;
		case reshadefx::shader_type::ps:
			type = api::shader_stage::pixel;
			break;
		case reshadefx::shader_type::cs:
			type = api::shader_stage::compute;
			if (!device->check_capability(api::device_caps::compute_shader))
			{
				effect.errors += "Compute shaders are not supported in D3D9.";
				return false;
			}
			break;
		}

		if (!compile_effect(effect, type, entry_point.name, entry_points[entry_point.name]))
		{
			LOG(ERROR) << "Failed to create shader module for effect file '" << effect.source_file << "' entry point '" << entry_point.name << "'!";
			return false;
		}
	}

	// Build specialization constants
	std::vector<uint32_t> spec_data;
	std::vector<uint32_t> spec_constants;
	for (const reshadefx::uniform_info &constant : effect.module.spec_constants)
	{
		const uint32_t id = static_cast<uint32_t>(spec_constants.size());
		spec_data.push_back(constant.initializer_value.as_uint[0]);
		spec_constants.push_back(id);
	}

	// Create query pool for time measurements
	if (!device->create_query_pool(api::query_type::timestamp, static_cast<uint32_t>(effect.module.techniques.size() * 2 * NUM_QUERY_FRAMES), &effect.query_heap))
		return false;

	uint32_t total_passes = 0;
	std::vector<api::descriptor_update> descriptor_updates;
	for (const reshadefx::technique_info &info : effect.module.techniques)
		total_passes += static_cast<uint32_t>(info.passes.size());

	// Create global constant buffer (except in D3D9, which does not have constant buffers)
	if (device->get_api() != api::device_api::d3d9 && !effect.uniform_data_storage.empty())
	{
		if (!device->create_resource(
			api::resource_desc(effect.uniform_data_storage.size(), api::memory_heap::cpu_to_gpu, api::resource_usage::constant_buffer),
			nullptr,
			api::resource_usage::cpu_access,
			&effect.cb))
		{
			LOG(ERROR) << "Failed to create constant buffer for effect file '" << effect.source_file << "'!";
			return false;
		}

		device->set_debug_name(effect.cb, "ReShade constant buffer");

		api::descriptor_range range;
		range.binding = 0;
		range.dx_shader_register = 0; // b0 (global constant buffer)
		range.type = api::descriptor_type::constant_buffer;
		range.count = 1;
		range.visibility = api::shader_stage::all;
		if (!device->create_descriptor_set_layout(1, &range, false, &effect.set_layouts[0]))
			return false;

		if (!device->create_descriptor_sets(effect.set_layouts[0], 1, &effect.cb_set))
			return false;

		api::descriptor_update &update = descriptor_updates.emplace_back();
		update.set = effect.cb_set;
		update.binding = 0;
		update.type = api::descriptor_type::constant_buffer;
		update.descriptor.resource = effect.cb;
	}

	// Initialize sampler and storage bindings
	std::vector<api::descriptor_set> texture_tables;
	std::vector<api::descriptor_set> storage_tables;
	const bool sampler_with_resource_view = device->check_capability(api::device_caps::sampler_with_resource_view);

	if (effect.module.num_sampler_bindings != 0)
	{
		api::descriptor_range range;
		range.binding = 0;
		range.dx_shader_register = 0; // s0
		range.type = sampler_with_resource_view ? api::descriptor_type::sampler_with_resource_view : api::descriptor_type::sampler;
		range.count = effect.module.num_sampler_bindings;
		range.visibility = api::shader_stage::all;
		if (!device->create_descriptor_set_layout(1, &range, false, &effect.set_layouts[1]))
			return false;

		if (sampler_with_resource_view)
		{
			texture_tables.resize(total_passes);
			if (!device->create_descriptor_sets(effect.set_layouts[1], total_passes, texture_tables.data()))
				return false;
		}
		else
		{
			if (!device->create_descriptor_sets(effect.set_layouts[1], 1, &effect.sampler_set))
				return false;

			uint16_t sampler_list = 0;
			for (const reshadefx::sampler_info &info : effect.module.samplers)
			{
				// Only initialize sampler if it has not been created before
				if (0 != (sampler_list & (1 << info.binding)))
					continue;

				sampler_list |= (1 << info.binding); // Maximum sampler slot count is 16, so a 16-bit integer is enough to hold all bindings

				api::sampler_desc desc;
				desc.filter = static_cast<api::texture_filter>(info.filter);
				desc.address_u = static_cast<api::texture_address_mode>(info.address_u);
				desc.address_v = static_cast<api::texture_address_mode>(info.address_v);
				desc.address_w = static_cast<api::texture_address_mode>(info.address_w);
				desc.mip_lod_bias = info.lod_bias;
				desc.max_anisotropy = 1;
				desc.min_lod = info.min_lod;
				desc.max_lod = info.max_lod;

				// Generate hash for sampler description
				size_t desc_hash = 2166136261;
				for (size_t i = 0; i < sizeof(desc); ++i)
					desc_hash = (desc_hash * 16777619) ^ reinterpret_cast<const uint8_t *>(&desc)[i];

				api::descriptor_update &update = descriptor_updates.emplace_back();
				update.set = effect.sampler_set;
				update.binding = info.binding;
				update.type = api::descriptor_type::sampler;

				if (const auto it = _effect_sampler_states.find(desc_hash);
					it != _effect_sampler_states.end())
				{
					update.descriptor.sampler = it->second;
				}
				else
				{
					if (!device->create_sampler(desc, &update.descriptor.sampler))
						return false;

					_effect_sampler_states.emplace(desc_hash, update.descriptor.sampler);
				}
			}
		}
	}

	if (effect.module.num_texture_bindings != 0)
	{
		assert(!sampler_with_resource_view);

		api::descriptor_range range;
		range.binding = 0;
		range.dx_shader_register = 0; // t0
		range.type = api::descriptor_type::shader_resource_view;
		range.count = effect.module.num_texture_bindings;
		range.visibility = api::shader_stage::all;
		if (!device->create_descriptor_set_layout(1, &range, false, &effect.set_layouts[2]))
			return false;

		texture_tables.resize(total_passes);
		if (!device->create_descriptor_sets(effect.set_layouts[2], total_passes, texture_tables.data()))
			return false;
	}

	if (effect.module.num_storage_bindings != 0)
	{
		api::descriptor_range range;
		range.binding = 0;
		range.dx_shader_register = 0; // u0
		range.type = api::descriptor_type::unordered_access_view;
		range.count = effect.module.num_storage_bindings;
		range.visibility = api::shader_stage::all;
		if (!device->create_descriptor_set_layout(1, &range, false, &effect.set_layouts[sampler_with_resource_view ? 2 : 3]))
			return false;

		storage_tables.resize(total_passes);
		if (!device->create_descriptor_sets(effect.set_layouts[sampler_with_resource_view ? 2 : 3], total_passes, storage_tables.data()))
			return false;
	}

	// Initialize pipeline layout
	if (!device->create_pipeline_layout(4, effect.set_layouts, 0, nullptr, &effect.layout))
	{
		LOG(ERROR) << "Failed to create pipeline layout for effect file '" << effect.source_file << "'!";
		return false;
	}

	uint32_t technique_index = 0;
	uint32_t total_pass_index = 0;
	for (technique &technique : _techniques)
	{
		if (!technique.passes_data.empty() || technique.effect_index != effect_index)
			continue;

		technique.passes_data.resize(technique.passes.size());

		// Offset index so that a query exists for each command frame and two subsequent ones are used for before/after stamps
		technique.query_base_index = technique_index++ * 2 * NUM_QUERY_FRAMES;

		for (size_t pass_index = 0; pass_index < technique.passes.size(); ++pass_index, ++total_pass_index)
		{
			reshadefx::pass_info &pass_info = technique.passes[pass_index];
			technique::pass_data &pass_data = technique.passes_data[pass_index];

			if (!pass_info.cs_entry_point.empty())
			{
				technique.has_compute_passes = true;

				api::pipeline_desc desc = { api::pipeline_type::compute };
				desc.layout = effect.layout;

				const auto &cs = entry_points.at(pass_info.cs_entry_point);
				desc.compute.shader.code = cs.data();
				desc.compute.shader.code_size = cs.size();
				desc.compute.shader.format = shader_format;
				if (shader_format == api::shader_format::spirv)
				{
					desc.compute.shader.entry_point = pass_info.cs_entry_point.c_str();
					desc.compute.shader.num_spec_constants = static_cast<uint32_t>(effect.module.spec_constants.size());
					desc.compute.shader.spec_constant_ids = spec_constants.data();
					desc.compute.shader.spec_constant_values = spec_data.data();
				}

				if (!device->create_pipeline(desc, &pass_data.pipeline))
				{
					LOG(ERROR) << "Failed to create compute pipeline for pass " << pass_index << " in technique '" << technique.name << "'!";
					return false;
				}
			}
			else
			{
				api::pipeline_desc desc = { api::pipeline_type::graphics };
				desc.layout = effect.layout;

				const auto &vs = entry_points.at(pass_info.vs_entry_point);
				desc.graphics.vertex_shader.code = vs.data();
				desc.graphics.vertex_shader.code_size = vs.size();
				desc.graphics.vertex_shader.format = shader_format;
				if (shader_format == api::shader_format::spirv)
				{
					desc.graphics.vertex_shader.entry_point = pass_info.vs_entry_point.c_str();
					desc.graphics.vertex_shader.num_spec_constants = static_cast<uint32_t>(effect.module.spec_constants.size());
					desc.graphics.vertex_shader.spec_constant_ids = spec_constants.data();
					desc.graphics.vertex_shader.spec_constant_values = spec_data.data();
				}

				const auto &ps = entry_points.at(pass_info.ps_entry_point);
				desc.graphics.pixel_shader.code = ps.data();
				desc.graphics.pixel_shader.code_size = ps.size();
				desc.graphics.pixel_shader.format = shader_format;
				if (shader_format == api::shader_format::spirv)
				{
					desc.graphics.pixel_shader.entry_point = pass_info.ps_entry_point.c_str();
					desc.graphics.pixel_shader.num_spec_constants = static_cast<uint32_t>(effect.module.spec_constants.size());
					desc.graphics.pixel_shader.spec_constant_ids = spec_constants.data();
					desc.graphics.pixel_shader.spec_constant_values = spec_data.data();
				}

				desc.graphics.num_viewports = 1;

				for (uint32_t k = 0; k < 8 && !pass_info.render_target_names[k].empty(); ++k)
				{
					texture &texture = look_up_texture_by_name(pass_info.render_target_names[k]);

					pass_data.modified_resources.push_back(texture.resource);

					if (texture.levels > 1)
						pass_data.generate_mipmap_views.push_back(texture.srv[pass_info.srgb_write_enable]);

					const api::resource_desc res_desc(
						device->get_resource_desc(texture.resource));
					const api::resource_view_desc rtv_desc(
						pass_info.srgb_write_enable ? api::format_to_default_typed_srgb(res_desc.texture.format) : api::format_to_default_typed(res_desc.texture.format), 0, 1, 0, 1);

					pass_data.render_targets[k] = texture.rtv[pass_info.srgb_write_enable];

					desc.graphics.num_render_targets = pass_data.num_render_targets = k + 1;
					desc.graphics.render_target_format[k] = rtv_desc.format;
				}

				if (pass_info.render_target_names[0].empty())
				{
					desc.graphics.num_render_targets = 1;
					desc.graphics.render_target_format[0] =
						pass_info.srgb_write_enable ? api::format_to_default_typed_srgb(get_backbuffer_format()) : api::format_to_default_typed(get_backbuffer_format());

					pass_info.viewport_width = _width;
					pass_info.viewport_height = _height;
				}

				// Only need to attach stencil if stencil is actually used in this pass
				if (pass_info.stencil_enable &&
					pass_info.viewport_width == _width &&
					pass_info.viewport_height == _height)
				{
					desc.graphics.depth_stencil_format = _effect_stencil_format;
				}
				else
				{
					desc.graphics.depth_stencil_format = api::format::unknown;
				}

				desc.graphics.sample_mask = std::numeric_limits<uint32_t>::max();
				desc.graphics.sample_count = 1;

				switch (pass_info.topology)
				{
				case reshadefx::primitive_topology::point_list:
					desc.graphics.topology = api::primitive_topology::point_list;
					break;
				case reshadefx::primitive_topology::line_list:
					desc.graphics.topology = api::primitive_topology::line_list;
					break;
				case reshadefx::primitive_topology::line_strip:
					desc.graphics.topology = api::primitive_topology::line_strip;
					break;
				case reshadefx::primitive_topology::triangle_list:
					desc.graphics.topology = api::primitive_topology::triangle_list;
					break;
				case reshadefx::primitive_topology::triangle_strip:
					desc.graphics.topology = api::primitive_topology::triangle_strip;
					break;
				}

				const auto convert_blend_op = [](reshadefx::pass_blend_op value) {
					switch (value)
					{
					default:
					case reshadefx::pass_blend_op::add: return api::blend_op::add;
					case reshadefx::pass_blend_op::subtract: return api::blend_op::subtract;
					case reshadefx::pass_blend_op::rev_subtract: return api::blend_op::rev_subtract;
					case reshadefx::pass_blend_op::min: return api::blend_op::min;
					case reshadefx::pass_blend_op::max: return api::blend_op::max;
					}
				};
				const auto convert_blend_func = [](reshadefx::pass_blend_func value) {
					switch (value) {
					case reshadefx::pass_blend_func::zero: return api::blend_factor::zero;
					default:
					case reshadefx::pass_blend_func::one: return api::blend_factor::one;
					case reshadefx::pass_blend_func::src_color: return api::blend_factor::src_color;
					case reshadefx::pass_blend_func::src_alpha: return api::blend_factor::src_alpha;
					case reshadefx::pass_blend_func::inv_src_color: return api::blend_factor::inv_src_color;
					case reshadefx::pass_blend_func::inv_src_alpha: return api::blend_factor::inv_src_alpha;
					case reshadefx::pass_blend_func::dst_color: return api::blend_factor::dst_color;
					case reshadefx::pass_blend_func::dst_alpha: return api::blend_factor::dst_alpha;
					case reshadefx::pass_blend_func::inv_dst_color: return api::blend_factor::inv_dst_color;
					case reshadefx::pass_blend_func::inv_dst_alpha: return api::blend_factor::inv_dst_alpha;
					}
				};

				auto &blend_state = desc.graphics.blend_state;
				for (uint32_t i = 0; i < 8; ++i)
				{
					blend_state.blend_enable[i] = pass_info.blend_enable;
					blend_state.src_color_blend_factor[i] = convert_blend_func(pass_info.src_blend);
					blend_state.dst_color_blend_factor[i] = convert_blend_func(pass_info.dest_blend);
					blend_state.color_blend_op[i] = convert_blend_op(pass_info.blend_op);
					blend_state.src_alpha_blend_factor[i] = convert_blend_func(pass_info.src_blend_alpha);
					blend_state.dst_alpha_blend_factor[i] = convert_blend_func(pass_info.dest_blend_alpha);
					blend_state.alpha_blend_op[i] = convert_blend_op(pass_info.blend_op_alpha);
					blend_state.render_target_write_mask[i] = pass_info.color_write_mask;
				}

				auto &rasterizer_state = desc.graphics.rasterizer_state;
				rasterizer_state.fill_mode = api::fill_mode::solid;
				rasterizer_state.cull_mode = api::cull_mode::none;
				rasterizer_state.depth_clip = true;

				const auto convert_stencil_op = [](reshadefx::pass_stencil_op value) {
					switch (value) {
					case reshadefx::pass_stencil_op::zero: return api::stencil_op::zero;
					default:
					case reshadefx::pass_stencil_op::keep: return api::stencil_op::keep;
					case reshadefx::pass_stencil_op::invert: return api::stencil_op::invert;
					case reshadefx::pass_stencil_op::replace: return api::stencil_op::replace;
					case reshadefx::pass_stencil_op::incr: return api::stencil_op::incr;
					case reshadefx::pass_stencil_op::incr_sat: return api::stencil_op::incr_sat;
					case reshadefx::pass_stencil_op::decr: return api::stencil_op::decr;
					case reshadefx::pass_stencil_op::decr_sat: return api::stencil_op::decr_sat;
					}
				};
				const auto convert_stencil_func = [](reshadefx::pass_stencil_func value) {
					switch (value)
					{
					case reshadefx::pass_stencil_func::never: return api::compare_op::never;
					case reshadefx::pass_stencil_func::equal: return api::compare_op::equal;
					case reshadefx::pass_stencil_func::not_equal: return api::compare_op::not_equal;
					case reshadefx::pass_stencil_func::less: return api::compare_op::less;
					case reshadefx::pass_stencil_func::less_equal: return api::compare_op::less_equal;
					case reshadefx::pass_stencil_func::greater: return api::compare_op::greater;
					case reshadefx::pass_stencil_func::greater_equal: return api::compare_op::greater_equal;
					default:
					case reshadefx::pass_stencil_func::always: return api::compare_op::always;
					}
				};

				auto &depth_stencil_state = desc.graphics.depth_stencil_state;
				depth_stencil_state.depth_test = false;
				depth_stencil_state.depth_write_mask = false;
				depth_stencil_state.depth_func = api::compare_op::always;
				depth_stencil_state.stencil_test = pass_info.stencil_enable;
				depth_stencil_state.stencil_read_mask = pass_info.stencil_read_mask;
				depth_stencil_state.stencil_write_mask = pass_info.stencil_write_mask;
				depth_stencil_state.back_stencil_fail_op = convert_stencil_op(pass_info.stencil_op_fail);
				depth_stencil_state.back_stencil_depth_fail_op = convert_stencil_op(pass_info.stencil_op_depth_fail);
				depth_stencil_state.back_stencil_pass_op = convert_stencil_op(pass_info.stencil_op_pass);
				depth_stencil_state.back_stencil_func = convert_stencil_func(pass_info.stencil_comparison_func);
				depth_stencil_state.front_stencil_fail_op = depth_stencil_state.back_stencil_fail_op;
				depth_stencil_state.front_stencil_depth_fail_op = depth_stencil_state.back_stencil_depth_fail_op;
				depth_stencil_state.front_stencil_pass_op = depth_stencil_state.back_stencil_pass_op;
				depth_stencil_state.front_stencil_func = depth_stencil_state.back_stencil_func;

				if (!device->create_pipeline(desc, &pass_data.pipeline))
				{
					LOG(ERROR) << "Failed to create graphics pipeline for pass " << pass_index << " in technique '" << technique.name << "'!";
					return false;
				}
			}

			if (effect.module.num_sampler_bindings != 0)
			{
				pass_data.texture_set = texture_tables[total_pass_index];

				for (const reshadefx::sampler_info &info : pass_info.samplers)
				{
					api::descriptor_update &update = descriptor_updates.emplace_back();
					update.set = pass_data.texture_set;

					if (sampler_with_resource_view)
					{
						update.binding = info.binding;
						update.type = api::descriptor_type::sampler_with_resource_view;

						api::sampler_desc desc;
						desc.filter = static_cast<api::texture_filter>(info.filter);
						desc.address_u = static_cast<api::texture_address_mode>(info.address_u);
						desc.address_v = static_cast<api::texture_address_mode>(info.address_v);
						desc.address_w = static_cast<api::texture_address_mode>(info.address_w);
						desc.mip_lod_bias = info.lod_bias;
						desc.max_anisotropy = 1;
						desc.compare_op = api::compare_op::never;
						desc.min_lod = info.min_lod;
						desc.max_lod = info.max_lod;

						// Generate hash for sampler description
						size_t desc_hash = 2166136261;
						for (size_t i = 0; i < sizeof(desc); ++i)
							desc_hash = (desc_hash * 16777619) ^ reinterpret_cast<const uint8_t *>(&desc)[i];

						if (const auto it = _effect_sampler_states.find(desc_hash);
							it != _effect_sampler_states.end())
						{
							update.descriptor.sampler = it->second;
						}
						else
						{
							if (!device->create_sampler(desc, &update.descriptor.sampler))
								return false;

							_effect_sampler_states.emplace(desc_hash, update.descriptor.sampler);
						}
					}
					else
					{
						update.binding = info.texture_binding;
						update.type = api::descriptor_type::shader_resource_view;
					}

					const texture &texture = look_up_texture_by_name(info.texture_name);

					if (texture.semantic == "COLOR")
					{
						update.descriptor.view = _backbuffer_texture_view[info.srgb];
					}
					else if (!texture.semantic.empty())
					{
						if (const auto it = _texture_semantic_bindings.find(texture.semantic);
							it != _texture_semantic_bindings.end())
							update.descriptor.view = it->second;
						else
							update.descriptor.view = _empty_texture_view;

						// Keep track of the texture descriptor to simplify updating it
						effect.texture_semantic_to_binding.push_back({ texture.semantic, update.set, update.binding, update.descriptor.sampler });
					}
					else
					{
						update.descriptor.view = texture.srv[info.srgb];
					}

					assert(update.descriptor.view.handle != 0);
				}
			}

			if (effect.module.num_storage_bindings != 0)
			{
				pass_data.storage_set = storage_tables[total_pass_index];

				for (const reshadefx::storage_info &info : pass_info.storages)
				{
					api::descriptor_update &update = descriptor_updates.emplace_back();
					update.set = pass_data.storage_set;
					update.binding = info.binding;
					update.type = api::descriptor_type::unordered_access_view;

					const texture &texture = look_up_texture_by_name(info.texture_name);

					assert(texture.semantic.empty());
					{
						update.descriptor.view = texture.uav;

						pass_data.modified_resources.push_back(texture.resource);

						if (texture.levels > 1)
							pass_data.generate_mipmap_views.push_back(texture.srv[0]);
					}

					assert(update.descriptor.view.handle != 0);
				}
			}
		}
	}

	if (!descriptor_updates.empty())
		device->update_descriptor_sets(static_cast<uint32_t>(descriptor_updates.size()), descriptor_updates.data());

	return true;
}
bool reshade::runtime::init_texture(texture &tex)
{
	api::device *const device = get_device();

	// Do not create resource if it is a special reference, those are set in 'render_technique' and 'update_texture_bindings'
	if (!tex.semantic.empty())
		return true;

	api::format format = api::format::unknown;
	api::format view_format = api::format::unknown;
	api::format view_format_srgb = api::format::unknown;

	switch (tex.format)
	{
	case reshadefx::texture_format::r8:
		format = api::format::r8_unorm;
		break;
	case reshadefx::texture_format::r16f:
		format = api::format::r16_float;
		break;
	case reshadefx::texture_format::r32f:
		format = api::format::r32_float;
		break;
	case reshadefx::texture_format::rg8:
		format = api::format::r8g8_unorm;
		break;
	case reshadefx::texture_format::rg16:
		format = api::format::r16g16_unorm;
		break;
	case reshadefx::texture_format::rg16f:
		format = api::format::r16g16_float;
		break;
	case reshadefx::texture_format::rg32f:
		format = api::format::r32g32_float;
		break;
	case reshadefx::texture_format::rgba8:
		format = api::format::r8g8b8a8_typeless;
		view_format = api::format::r8g8b8a8_unorm;
		view_format_srgb = api::format::r8g8b8a8_unorm_srgb;
		break;
	case reshadefx::texture_format::rgba16:
		format = api::format::r16g16b16a16_unorm;
		break;
	case reshadefx::texture_format::rgba16f:
		format = api::format::r16g16b16a16_float;
		break;
	case reshadefx::texture_format::rgba32f:
		format = api::format::r32g32b32a32_float;
		break;
	case reshadefx::texture_format::rgb10a2:
		format = api::format::r10g10b10a2_unorm;
		break;
	}

	if (view_format == api::format::unknown)
		view_format_srgb = view_format = format;

	api::resource_usage usage = api::resource_usage::shader_resource;
	if (tex.semantic.empty())
		usage |= api::resource_usage::copy_dest; // For texture data upload
	if (tex.render_target)
		usage |= api::resource_usage::render_target;
	if (tex.storage_access && _renderer_id >= 0xb000)
		usage |= api::resource_usage::unordered_access;

	api::resource_flags flags = api::resource_flags::none;
	if (tex.levels > 1)
		flags |= api::resource_flags::generate_mipmaps;

	// Clear texture to zero since by default its contents are undefined
	std::vector<uint8_t> zero_data(tex.width * tex.height * 16);
	std::vector<api::subresource_data> initial_data(tex.levels);
	for (uint32_t level = 0, width = tex.width; level < tex.levels; ++level, width /= 2)
	{
		initial_data[level].data = zero_data.data();
		initial_data[level].row_pitch = width * 16;
	}

	if (!device->create_resource(api::resource_desc(tex.width, tex.height, 1, tex.levels, format, 1, api::memory_heap::gpu_only, usage, flags), initial_data.data(), api::resource_usage::shader_resource, &tex.resource))
	{
		LOG(ERROR) << "Failed to create texture '" << tex.unique_name << "'!";
		LOG(DEBUG) << "> Details: Width = " << tex.width << ", Height = " << tex.height << ", Levels = " << tex.levels << ", Format = " << static_cast<uint32_t>(format) << ", Usage = " << std::hex << static_cast<uint32_t>(usage) << std::dec;
		return false;
	}

	device->set_debug_name(tex.resource, tex.unique_name.c_str());

	// Always create shader resource views
	{
		if (!device->create_resource_view(tex.resource, api::resource_usage::shader_resource, api::resource_view_desc(view_format, 0, tex.levels, 0, 1), &tex.srv[0]))
		{
			LOG(ERROR) << "Failed to create shader resource view for texture '" << tex.unique_name << "'!";
			LOG(DEBUG) << "> Details: Format = " << static_cast<uint32_t>(view_format) << ", Levels = " << tex.levels;
			return false;
		}
		if (view_format_srgb == view_format || tex.storage_access) // sRGB formats do not support storage usage
		{
			tex.srv[1] = tex.srv[0];
		}
		else if (!device->create_resource_view(tex.resource, api::resource_usage::shader_resource, api::resource_view_desc(view_format_srgb, 0, tex.levels, 0, 1), &tex.srv[1]))
		{
			LOG(ERROR) << "Failed to create shader resource view for texture '" << tex.unique_name << "'!";
			LOG(DEBUG) << "> Details: Format = " << static_cast<uint32_t>(view_format_srgb) << ", Levels = " << tex.levels;
			return false;
		}
	}

	// Create render target views (with a single level)
	if (tex.render_target)
	{
		if (!device->create_resource_view(tex.resource, api::resource_usage::render_target, api::resource_view_desc(view_format, 0, 1, 0, 1), &tex.rtv[0]))
		{
			LOG(ERROR) << "Failed to create render target view for texture '" << tex.unique_name << "'!";
			LOG(DEBUG) << "> Details: Format = " << static_cast<uint32_t>(view_format);
			return false;
		}
		if (view_format_srgb == view_format)
		{
			tex.rtv[1] = tex.rtv[0];
		}
		else if (!device->create_resource_view(tex.resource, api::resource_usage::render_target, api::resource_view_desc(view_format_srgb, 0, 1, 0, 1), &tex.rtv[1]))
		{
			LOG(ERROR) << "Failed to create render target view for texture '" << tex.unique_name << "'!";
			LOG(DEBUG) << "> Details: Format = " << static_cast<uint32_t>(view_format_srgb);
			return false;
		}
	}

	if (tex.storage_access && _renderer_id >= 0xb000)
	{
		if (!device->create_resource_view(tex.resource, api::resource_usage::unordered_access, api::resource_view_desc(view_format, 0, 1, 0, 1), &tex.uav))
		{
			LOG(ERROR) << "Failed to create unordered access view for texture '" << tex.unique_name << "'!";
			LOG(DEBUG) << "> Details: Format = " << static_cast<uint32_t>(view_format);
			return false;
		}
	}

	return true;
}

void reshade::runtime::update_texture_bindings(const char *semantic, api::resource_view srv)
{
	if (srv.handle != 0)
		_texture_semantic_bindings[semantic] = srv;
	else
		_texture_semantic_bindings.erase(semantic);

	api::device *const device = get_device();

	// Make sure all previous frames have finished before freeing the image view and updating descriptors (since they may be in use otherwise)
	device->wait_idle();

	// Update texture bindings
	std::vector<api::descriptor_update> updates;

	for (effect &effect_data : _effects)
	{
		for (const auto &binding : effect_data.texture_semantic_to_binding)
		{
			if (binding.semantic != semantic)
				continue;

			api::descriptor_update &update = updates.emplace_back();
			update.set = binding.set;
			update.binding = binding.index;
			update.type = binding.sampler.handle != 0 ? api::descriptor_type::sampler_with_resource_view : api::descriptor_type::shader_resource_view;
			update.descriptor.sampler = binding.sampler;
			update.descriptor.view = srv;
		}
	}

	device->update_descriptor_sets(static_cast<uint32_t>(updates.size()), updates.data());
}

void reshade::runtime::unload_effect(size_t effect_index)
{
	assert(effect_index < _effects.size());

	api::device *const device = get_device();

	// Make sure no effect resources are currently in use
	device->wait_idle();

	for (technique &tech : _techniques)
	{
		if (tech.effect_index != effect_index)
			continue;

		for (size_t pass_index = 0; pass_index < tech.passes_data.size(); ++pass_index)
		{
			device->destroy_pipeline(tech.passes[pass_index].cs_entry_point.empty() ? api::pipeline_type::graphics : api::pipeline_type::compute, tech.passes_data[pass_index].pipeline);

			device->destroy_descriptor_sets(_effects[effect_index].set_layouts[2], 1, &tech.passes_data[pass_index].texture_set);
			device->destroy_descriptor_sets(_effects[effect_index].set_layouts[3], 1, &tech.passes_data[pass_index].storage_set);
		}

		tech.passes_data.clear();
	}

	{	effect &effect = _effects[effect_index];

		device->destroy_resource(effect.cb);
		effect.cb = {};

		device->destroy_pipeline_layout(effect.layout);
		effect.layout = {};

		device->destroy_descriptor_sets(effect.set_layouts[0], 1, &effect.cb_set);
		effect.cb_set = {};

		device->destroy_descriptor_sets(effect.set_layouts[1], 1, &effect.sampler_set);
		effect.sampler_set = {};

		for (uint32_t i = 0; i < 4; ++i)
		{
			device->destroy_descriptor_set_layout(effect.set_layouts[i]);
			effect.set_layouts[i] = {};
		}

		device->destroy_query_pool(effect.query_heap);
		effect.query_heap = {};
	}

#if RESHADE_GUI
	_preview_texture.handle = 0;
#endif

	// Lock here to be safe in case another effect is still loading
	const std::lock_guard<std::mutex> lock(_reload_mutex);

	// Destroy textures belonging to this effect
	_textures.erase(std::remove_if(_textures.begin(), _textures.end(),
		[this, effect_index](texture &tex) {
			tex.shared.erase(std::remove(tex.shared.begin(), tex.shared.end(), effect_index), tex.shared.end());
			if (tex.shared.empty()) {
				destroy_texture(tex);
				return true;
			}
			return false;
		}), _textures.end());
	// Clean up techniques belonging to this effect
	_techniques.erase(std::remove_if(_techniques.begin(), _techniques.end(),
		[effect_index](const technique &tech) {
			return tech.effect_index == effect_index;
		}), _techniques.end());

	_effects[effect_index].rendering = 0;
	// Do not clear effect here, since it is common to be re-used immediately
}
void reshade::runtime::unload_effects()
{
	api::device *const device = get_device();

	// Make sure no effect resources are currently in use
	device->wait_idle();

	for (technique &tech : _techniques)
	{
		for (size_t pass_index = 0; pass_index < tech.passes_data.size(); ++pass_index)
		{
			device->destroy_pipeline(tech.passes[pass_index].cs_entry_point.empty() ? api::pipeline_type::graphics : api::pipeline_type::compute, tech.passes_data[pass_index].pipeline);

			device->destroy_descriptor_sets(_effects[tech.effect_index].set_layouts[2], 1, &tech.passes_data[pass_index].texture_set);
			device->destroy_descriptor_sets(_effects[tech.effect_index].set_layouts[3], 1, &tech.passes_data[pass_index].storage_set);
		}

		tech.passes_data.clear();
	}

	for (const effect &effect : _effects)
	{
		device->destroy_resource(effect.cb);

		device->destroy_descriptor_sets(effect.set_layouts[0], 1, &effect.cb_set);
		device->destroy_descriptor_sets(effect.set_layouts[1], 1, &effect.sampler_set);

		device->destroy_pipeline_layout(effect.layout);
		for (uint32_t i = 0; i < 4; ++i)
			device->destroy_descriptor_set_layout(effect.set_layouts[i]);

		device->destroy_query_pool(effect.query_heap);
	}

	for (auto &[hash, sampler] : _effect_sampler_states)
		device->destroy_sampler(sampler);
	_effect_sampler_states.clear();

#if RESHADE_GUI
	_preview_texture.handle = 0;
	_effect_filter[0] = '\0'; // And reset filter too, since the list of techniques might have changed
#endif

	// Make sure no threads are still accessing effect data
	for (std::thread &thread : _worker_threads)
		if (thread.joinable())
			thread.join();
	_worker_threads.clear();

	// Destroy all textures
	for (texture &tex : _textures)
		destroy_texture(tex);
	_textures.clear();
	_textures_loaded = false;
	// Clean up all techniques
	_techniques.clear();

	// Reset the effect list after all resources have been destroyed
	_effects.clear();
}
void reshade::runtime::destroy_texture(texture &texture)
{
	api::device *const device = get_device();

	device->destroy_resource(texture.resource);
	texture.resource = {};

	device->destroy_resource_view(texture.srv[0]);
	if (texture.srv[1] != texture.srv[0])
		device->destroy_resource_view(texture.srv[1]);
	texture.srv[0] = {};
	texture.srv[1] = {};

	device->destroy_resource_view(texture.rtv[0]);
	if (texture.rtv[1] != texture.rtv[0])
		device->destroy_resource_view(texture.rtv[1]);
	texture.rtv[0] = {};
	texture.rtv[1] = {};

	device->destroy_resource_view(texture.uav);
	texture.uav = {};
}

bool reshade::runtime::reload_effect(size_t effect_index, bool preprocess_required)
{
#if RESHADE_GUI
	_show_splash = false; // Hide splash bar when reloading a single effect file
#endif

	const std::filesystem::path source_file = _effects[effect_index].source_file;
	unload_effect(effect_index);
	return load_effect(source_file, ini_file::load_cache(_current_preset_path), effect_index, preprocess_required);
}
void reshade::runtime::reload_effects()
{
	// Clear out any previous effects
	unload_effects();

#if RESHADE_GUI
	_show_splash = true; // Always show splash bar when reloading everything
	_reload_count++;
#endif
	_last_reload_successfull = true;

	load_effects();
}

bool reshade::runtime::load_effect_cache(const std::filesystem::path &source_file, const size_t hash, std::string &source) const
{
	if (_no_effect_cache)
		return false;

	std::filesystem::path path = g_reshade_base_path / _intermediate_cache_path;
	path /= std::filesystem::u8path("reshade-" + source_file.stem().u8string() + '-' + std::to_string(_renderer_id) + '-' + std::to_string(hash) + ".i");

	const HANDLE file = CreateFileW(path.c_str(), FILE_GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
	if (file == INVALID_HANDLE_VALUE)
		return false;
	DWORD size = GetFileSize(file, nullptr);
	source.resize(size);
	const BOOL result = ReadFile(file, source.data(), size, &size, nullptr);
	CloseHandle(file);
	return result != FALSE;
}
bool reshade::runtime::load_effect_cache(const std::filesystem::path &source_file, const std::string &entry_point, const size_t hash, std::vector<char> &cso, std::string &dasm) const
{
	if (_no_effect_cache)
		return false;

	std::filesystem::path path = g_reshade_base_path / _intermediate_cache_path;
	path /= std::filesystem::u8path("reshade-" + source_file.stem().u8string() + '-' + entry_point + '-' + std::to_string(_renderer_id) + '-' + std::to_string(hash) + ".cso");

	{	const HANDLE file = CreateFileW(path.c_str(), FILE_GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
		if (file == INVALID_HANDLE_VALUE)
			return false;
		DWORD size = GetFileSize(file, nullptr);
		cso.resize(size);
		const BOOL result = ReadFile(file, cso.data(), size, &size, nullptr);
		CloseHandle(file);
		if (result == FALSE)
			return false;
	}

	path.replace_extension(L".asm");

	{	const HANDLE file = CreateFileW(path.c_str(), FILE_GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
		if (file == INVALID_HANDLE_VALUE)
			return false;
		DWORD size = GetFileSize(file, nullptr);
		dasm.resize(size);
		const BOOL result = ReadFile(file, dasm.data(), size, &size, nullptr);
		CloseHandle(file);
		if (result == FALSE)
			return false;
	}

	return true;
}
bool reshade::runtime::save_effect_cache(const std::filesystem::path &source_file, const size_t hash, const std::string &source) const
{
	if (_no_effect_cache)
		return false;

	std::filesystem::path path = g_reshade_base_path / _intermediate_cache_path;
	path /= std::filesystem::u8path("reshade-" + source_file.stem().u8string() + '-' + std::to_string(_renderer_id) + '-' + std::to_string(hash) + ".i");

	const HANDLE file = CreateFileW(path.c_str(), FILE_GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_ARCHIVE | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
	if (file == INVALID_HANDLE_VALUE)
		return false;
	DWORD size = static_cast<DWORD>(source.size());
	const BOOL result = WriteFile(file, source.data(), size, &size, nullptr);
	CloseHandle(file);
	return result != FALSE;
}
bool reshade::runtime::save_effect_cache(const std::filesystem::path &source_file, const std::string &entry_point, const size_t hash, const std::vector<char> &cso, const std::string &dasm) const
{
	if (_no_effect_cache)
		return false;

	std::filesystem::path path = g_reshade_base_path / _intermediate_cache_path;
	path /= std::filesystem::u8path("reshade-" + source_file.stem().u8string() + '-' + entry_point + '-' + std::to_string(_renderer_id) + '-' + std::to_string(hash) + ".cso");

	{	const HANDLE file = CreateFileW(path.c_str(), FILE_GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
		if (file == INVALID_HANDLE_VALUE)
			return false;
		DWORD size = static_cast<DWORD>(cso.size());
		const BOOL result = WriteFile(file, cso.data(), size, &size, nullptr);
		CloseHandle(file);
		if (result == FALSE)
			return false;
	}

	path.replace_extension(L".asm");

	{	const HANDLE file = CreateFileW(path.c_str(), FILE_GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_ARCHIVE | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
		if (file == INVALID_HANDLE_VALUE)
			return false;
		DWORD size = static_cast<DWORD>(dasm.size());
		const BOOL result = WriteFile(file, dasm.c_str(), size, &size, NULL);
		CloseHandle(file);
		if (result == FALSE)
			return false;
	}

	return true;
}

void reshade::runtime::clear_effect_cache()
{
	// Find all cached effect files and delete them
	std::error_code ec;
	for (const std::filesystem::directory_entry &entry : std::filesystem::directory_iterator(g_reshade_base_path / _intermediate_cache_path, std::filesystem::directory_options::skip_permission_denied, ec))
	{
		if (entry.is_directory(ec))
			continue;

		const std::filesystem::path filename = entry.path().filename();
		const std::filesystem::path extension = entry.path().extension();
		if (filename.native().compare(0, 8, L"reshade-") != 0 || (extension != L".i" && extension != L".cso" && extension != L".asm"))
			continue;

		DeleteFileW(entry.path().c_str());
	}
}

void reshade::runtime::update_and_render_effects()
{
	// Delay first load to the first render call to avoid loading while the application is still initializing
	if (_framecount == 0 && !_no_reload_on_init)
		reload_effects();

	if (_reload_remaining_effects == 0)
	{
		// Clear the thread list now that they all have finished
		for (std::thread &thread : _worker_threads)
			if (thread.joinable())
				thread.join(); // Threads have exited, but still need to join them prior to destruction
		_worker_threads.clear();

		// Finished loading effects, so apply preset to figure out which ones need compiling
		load_current_preset();

		_last_reload_time = std::chrono::high_resolution_clock::now();
		_reload_remaining_effects = std::numeric_limits<size_t>::max();

		// Reset all effect loading options
		_load_option_disable_skipping = false;

#if RESHADE_GUI
		// Update all editors after a reload
		for (editor_instance &instance : _editors)
		{
			if (const auto it = std::find_if(_effects.begin(), _effects.end(),
				[this, &instance](const auto &effect) { return effect.source_file == instance.file_path; }); it != _effects.end())
			{
				instance.effect_index = std::distance(_effects.begin(), it);

				if (instance.entry_point_name.empty() || instance.entry_point_name == "Generated code")
					open_code_editor(instance);
				else
					instance.editor.clear_text();
			}
		}
#endif
	}
	else if (_reload_remaining_effects != std::numeric_limits<size_t>::max())
	{
		return; // Cannot render while effects are still being loaded
	}
	else if (!_reload_compile_queue.empty())
	{
		// Pop an effect from the queue
		const size_t effect_index = _reload_compile_queue.back();
		_reload_compile_queue.pop_back();
		effect &effect = _effects[effect_index];

		// Create textures now, since they are referenced when building samplers in the 'init_effect' call below
		for (texture &tex : _textures)
		{
			if (tex.resource.handle != 0 || (
				// Always create shared textures, since they may be in use by this effect already
				tex.effect_index != effect_index && tex.shared.size() <= 1))
				continue;

			if (!init_texture(tex))
			{
				effect.errors += "Failed to create texture " + tex.unique_name;
				effect.compiled = false;
				break;
			}
		}

		// Compile the effect with the back-end implementation (unless texture creation failed)
		if (effect.compiled)
			effect.compiled = init_effect(effect_index);

		// De-duplicate error lines (D3DCompiler sometimes repeats the same error multiple times)
		for (size_t line_offset = 0, next_line_offset;
			(next_line_offset = effect.errors.find('\n', line_offset)) != std::string::npos; line_offset = next_line_offset + 1)
		{
			const std::string_view cur_line(effect.errors.c_str() + line_offset, next_line_offset - line_offset);

			if (const size_t end_offset = effect.errors.find('\n', next_line_offset + 1);
				end_offset != std::string::npos)
			{
				const std::string_view next_line(effect.errors.c_str() + next_line_offset + 1, end_offset - next_line_offset - 1);
				if (cur_line == next_line)
				{
					effect.errors.erase(next_line_offset, end_offset - next_line_offset);
					next_line_offset = line_offset - 1;
				}
			}

			// Also remove D3DCompiler warnings about 'groupshared' specifier used in VS/PS modules
			if (cur_line.find("X3579") != std::string_view::npos)
			{
				effect.errors.erase(line_offset, next_line_offset + 1 - line_offset);
				next_line_offset = line_offset - 1;
			}
		}

		if (!effect.compiled) // Something went wrong, do clean up
		{
			if (effect.errors.empty())
				LOG(ERROR) << "Failed initializing " << effect.source_file << '!';
			else
				LOG(ERROR) << "Failed initializing " << effect.source_file << ":\n" << effect.errors;

			// Destroy all textures belonging to this effect
			for (texture &tex : _textures)
				if (tex.effect_index == effect_index && tex.shared.size() <= 1)
					destroy_texture(tex);
			// Disable all techniques belonging to this effect
			for (technique &tech : _techniques)
				if (tech.effect_index == effect_index)
					disable_technique(tech);

			_last_reload_successfull = false;
		}

		// An effect has changed, need to reload textures
		_textures_loaded = false;

#if RESHADE_GUI
		if (effect.compiled)
		{
			// Update assembly in all editors after a reload
			for (editor_instance &instance : _editors)
			{
				if (instance.entry_point_name.empty() || instance.file_path != effect.source_file)
					continue;
				assert(instance.effect_index == effect_index);

				if (const auto assembly_it = effect.assembly.find(instance.entry_point_name);
					assembly_it != effect.assembly.end())
					open_code_editor(instance);
			}
		}
#endif
	}
	else if (!_textures_loaded)
	{
		// Now that all effects were compiled, load all textures
		load_textures();
	}

#ifdef NDEBUG
	// Lock input so it cannot be modified by other threads while we are reading it here
	// TODO: This does not catch input happening between now and 'on_present'
	const auto input_lock = _input->lock();
#endif

	if (_should_save_screenshot && (_screenshot_save_before || !_effects_enabled))
		save_screenshot(_effects_enabled ? L" original" : std::wstring(), !_effects_enabled);

	// Nothing to do here if effects are disabled globally
	if (!_effects_enabled)
		return;

	// Update special uniform variables
	for (effect &effect : _effects)
	{
		if (!effect.rendering)
			continue;

		for (uniform &variable : effect.uniforms)
		{
			if (!_ignore_shortcuts && _input->is_key_pressed(variable.toggle_key_data, _force_shortcut_modifiers))
			{
				assert(variable.supports_toggle_key());

				// Change to next value if the associated shortcut key was pressed
				switch (variable.type.base)
				{
					case reshadefx::type::t_bool:
					{
						bool data;
						get_uniform_value(variable, &data, 1);
						set_uniform_value(variable, !data);
						break;
					}
					case reshadefx::type::t_int:
					case reshadefx::type::t_uint:
					{
						int data[4];
						get_uniform_value(variable, data, 4);
						const std::string_view ui_items = variable.annotation_as_string("ui_items");
						int num_items = 0;
						for (size_t offset = 0, next; (next = ui_items.find('\0', offset)) != std::string::npos; offset = next + 1)
							num_items++;
						data[0] = (data[0] + 1 >= num_items) ? 0 : data[0] + 1;
						set_uniform_value(variable, data, 4);
						break;
					}
				}
				save_current_preset();
			}

			switch (variable.special)
			{
				case special_uniform::frame_time:
				{
					set_uniform_value(variable, _last_frame_duration.count() * 1e-6f);
					break;
				}
				case special_uniform::frame_count:
				{
					if (variable.type.is_boolean())
						set_uniform_value(variable, (_framecount % 2) == 0);
					else
						set_uniform_value(variable, static_cast<unsigned int>(_framecount % UINT_MAX));
					break;
				}
				case special_uniform::random:
				{
					const int min = variable.annotation_as_int("min", 0, 0);
					const int max = variable.annotation_as_int("max", 0, RAND_MAX);
					set_uniform_value(variable, min + (std::rand() % (std::abs(max - min) + 1)));
					break;
				}
				case special_uniform::ping_pong:
				{
					const float min = variable.annotation_as_float("min", 0, 0.0f);
					const float max = variable.annotation_as_float("max", 0, 1.0f);
					const float step_min = variable.annotation_as_float("step", 0);
					const float step_max = variable.annotation_as_float("step", 1);
					float increment = step_max == 0 ? step_min : (step_min + std::fmodf(static_cast<float>(std::rand()), step_max - step_min + 1));
					const float smoothing = variable.annotation_as_float("smoothing");

					float value[2] = { 0, 0 };
					get_uniform_value(variable, value, 2);
					if (value[1] >= 0)
					{
						increment = std::max(increment - std::max(0.0f, smoothing - (max - value[0])), 0.05f);
						increment *= _last_frame_duration.count() * 1e-9f;

						if ((value[0] += increment) >= max)
							value[0] = max, value[1] = -1;
					}
					else
					{
						increment = std::max(increment - std::max(0.0f, smoothing - (value[0] - min)), 0.05f);
						increment *= _last_frame_duration.count() * 1e-9f;

						if ((value[0] -= increment) <= min)
							value[0] = min, value[1] = +1;
					}
					set_uniform_value(variable, value, 2);
					break;
				}
				case special_uniform::date:
				{
					const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
					tm tm; localtime_s(&tm, &t);

					const int value[4] = {
						tm.tm_year + 1900,
						tm.tm_mon + 1,
						tm.tm_mday,
						tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec
					};
					set_uniform_value(variable, value, 4);
					break;
				}
				case special_uniform::timer:
				{
					const unsigned long long timer_ms = std::chrono::duration_cast<std::chrono::milliseconds>(_last_present_time - _start_time).count();
					set_uniform_value(variable, static_cast<unsigned int>(timer_ms));
					break;
				}
				case special_uniform::key:
				{
					if (const int keycode = variable.annotation_as_int("keycode");
						keycode > 7 && keycode < 256)
					{
						if (const std::string_view mode = variable.annotation_as_string("mode");
							mode == "toggle" || variable.annotation_as_int("toggle"))
						{
							bool current_value = false;
							get_uniform_value(variable, &current_value, 1);
							if (_input->is_key_pressed(keycode))
								set_uniform_value(variable, !current_value);
						}
						else if (mode == "press")
							set_uniform_value(variable, _input->is_key_pressed(keycode));
						else
							set_uniform_value(variable, _input->is_key_down(keycode));
					}
					break;
				}
				case special_uniform::mouse_point:
				{
					set_uniform_value(variable, _input->mouse_position_x(), _input->mouse_position_y());
					break;
				}
				case special_uniform::mouse_delta:
				{
					set_uniform_value(variable, _input->mouse_movement_delta_x(), _input->mouse_movement_delta_y());
					break;
				}
				case special_uniform::mouse_button:
				{
					if (const int keycode = variable.annotation_as_int("keycode");
						keycode >= 0 && keycode < 5)
					{
						if (const std::string_view mode = variable.annotation_as_string("mode");
							mode == "toggle" || variable.annotation_as_int("toggle"))
						{
							bool current_value = false;
							get_uniform_value(variable, &current_value, 1);
							if (_input->is_mouse_button_pressed(keycode))
								set_uniform_value(variable, !current_value);
						}
						else if (mode == "press")
							set_uniform_value(variable, _input->is_mouse_button_pressed(keycode));
						else
							set_uniform_value(variable, _input->is_mouse_button_down(keycode));
					}
					break;
				}
				case special_uniform::mouse_wheel:
				{
					const float min = variable.annotation_as_float("min");
					const float max = variable.annotation_as_float("max");
					float step = variable.annotation_as_float("step");
					if (step == 0.0f)
						step  = 1.0f;

					float value[2] = { 0, 0 };
					get_uniform_value(variable, value, 2);
					value[1] = _input->mouse_wheel_delta();
					value[0] = value[0] + value[1] * step;
					if (min != max)
					{
						value[0] = std::max(value[0], min);
						value[0] = std::min(value[0], max);
					}
					set_uniform_value(variable, value, 2);
					break;
				}
				case special_uniform::freepie:
				{
					if (freepie_io_data data;
						freepie_io_read(variable.annotation_as_int("index"), &data))
						set_uniform_value(variable, &data.yaw, 3 * 2);
					break;
				}
#if RESHADE_GUI
				case special_uniform::overlay_open:
				{
					set_uniform_value(variable, _show_overlay);
					break;
				}
				case special_uniform::overlay_active:
				case special_uniform::overlay_hovered:
				{
					// These are set in 'draw_variable_editor' when overlay is open
					if (!_show_overlay)
						set_uniform_value(variable, 0);
					break;
				}
#endif
			}
		}
	}

	auto cmd_list = get_command_queue()->get_immediate_command_list();
	cmd_list->barrier(get_backbuffer_resource(), api::resource_usage::present, api::resource_usage::render_target);

	// Render all enabled techniques
	for (technique &technique : _techniques)
	{
		if (!_ignore_shortcuts && _input->is_key_pressed(technique.toggle_key_data, _force_shortcut_modifiers))
		{
			if (!technique.enabled)
				enable_technique(technique);
			else
				disable_technique(technique);
		}

		if (technique.passes_data.empty() || !technique.enabled)
			continue; // Ignore techniques that are not fully loaded or currently disabled

		const auto time_technique_started = std::chrono::high_resolution_clock::now();
		render_technique(technique);
		const auto time_technique_finished = std::chrono::high_resolution_clock::now();

		technique.average_cpu_duration.append(std::chrono::duration_cast<std::chrono::nanoseconds>(time_technique_finished - time_technique_started).count());

		if (technique.time_left > 0)
		{
			technique.time_left -= std::chrono::duration_cast<std::chrono::milliseconds>(_last_frame_duration).count();
			if (technique.time_left <= 0)
				disable_technique(technique);
		}
	}

	cmd_list->barrier(get_backbuffer_resource(), api::resource_usage::render_target, api::resource_usage::present);

	if (_should_save_screenshot)
		save_screenshot(std::wstring(), true);
}

void reshade::runtime::render_technique(technique &technique)
{
	const effect &effect = _effects[technique.effect_index];

	api::device *const device = get_device();
	api::command_list *const cmd_list = get_command_queue()->get_immediate_command_list();

#if RESHADE_ADDON
	invoke_addon_event<addon_event::reshade_before_effects>(this, cmd_list);
#endif

	if (_gather_gpu_statistics)
	{
		// Evaluate queries from oldest frame in queue
		if (uint64_t timestamps[2];
			device->get_query_results(effect.query_heap, technique.query_base_index + ((_framecount + 1) % NUM_QUERY_FRAMES) * 2, 2, timestamps, sizeof(uint64_t)))
			technique.average_gpu_duration.append(timestamps[1] - timestamps[0]);

		cmd_list->finish_query(effect.query_heap, api::query_type::timestamp, technique.query_base_index + (_framecount % NUM_QUERY_FRAMES) * 2);
	}

#ifndef NDEBUG
	const float debug_event_col[4] = { 1.0f, 0.8f, 0.8f, 1.0f };
	cmd_list->begin_debug_marker(technique.name.c_str(), debug_event_col);
#endif

	// Setup shader constants
	if (effect.cb.handle != 0)
	{
		if (void *mapped_ptr;
			device->map_resource(effect.cb, 0, api::map_access::write_discard, &mapped_ptr))
		{
			std::memcpy(mapped_ptr, effect.uniform_data_storage.data(), effect.uniform_data_storage.size());
			device->unmap_resource(effect.cb, 0);
		}

		cmd_list->bind_descriptor_sets(api::pipeline_type::graphics, effect.layout, 0, 1, &effect.cb_set);
		if (technique.has_compute_passes)
			cmd_list->bind_descriptor_sets(api::pipeline_type::compute, effect.layout, 0, 1, &effect.cb_set);
	}
	else if (device->get_api() == api::device_api::d3d9)
	{
		cmd_list->push_constants(api::shader_stage::all, effect.layout, 0, 0, static_cast<uint32_t>(effect.uniform_data_storage.size() / sizeof(uint32_t)), reinterpret_cast<const uint32_t *>(effect.uniform_data_storage.data()));
	}

	const bool sampler_with_resource_view = device->check_capability(api::device_caps::sampler_with_resource_view);

	// Setup samplers
	if (effect.sampler_set.handle != 0)
	{
		assert(!sampler_with_resource_view);

		cmd_list->bind_descriptor_sets(api::pipeline_type::graphics, effect.layout, 1, 1, &effect.sampler_set);
		if (technique.has_compute_passes)
			cmd_list->bind_descriptor_sets(api::pipeline_type::compute, effect.layout, 1, 1, &effect.sampler_set);
	}

	bool is_effect_stencil_cleared = false;
	bool needs_implicit_backbuffer_copy = true; // First pass always needs the back buffer updated

	for (size_t pass_index = 0; pass_index < technique.passes.size(); ++pass_index)
	{
		if (needs_implicit_backbuffer_copy)
		{
			// Save back buffer of previous pass
			const api::resource resources[2] = { get_backbuffer_resource(), _backbuffer_texture };
			const api::resource_usage state_old[2] = { api::resource_usage::render_target, api::resource_usage::shader_resource };
			const api::resource_usage state_new[2] = { api::resource_usage::copy_source, api::resource_usage::copy_dest };

			cmd_list->barrier(2, resources, state_old, state_new);
			cmd_list->copy_resource(get_backbuffer_resource(), _backbuffer_texture);
			cmd_list->barrier(2, resources, state_new, state_old);
		}

		const reshadefx::pass_info &pass_info = technique.passes[pass_index];
		const technique::pass_data &pass_data = technique.passes_data[pass_index];

#ifndef NDEBUG
		cmd_list->begin_debug_marker((pass_info.name.empty() ? "Pass " + std::to_string(pass_index) : pass_info.name).c_str(), debug_event_col);
#endif

		if (!pass_info.cs_entry_point.empty())
		{
			// Compute shaders do not write to the back buffer, so no update necessary
			needs_implicit_backbuffer_copy = false;

			cmd_list->bind_pipeline(api::pipeline_type::compute, pass_data.pipeline);

			std::vector<api::resource_usage> state_old(pass_data.modified_resources.size(), api::resource_usage::shader_resource);
			std::vector<api::resource_usage> state_new(pass_data.modified_resources.size(), api::resource_usage::unordered_access);
			cmd_list->barrier(static_cast<uint32_t>(pass_data.modified_resources.size()), pass_data.modified_resources.data(), state_old.data(), state_new.data());

			if (pass_data.texture_set.handle != 0)
				cmd_list->bind_descriptor_sets(api::pipeline_type::compute, effect.layout, sampler_with_resource_view ? 1 : 2, 1, &pass_data.texture_set);
			if (pass_data.storage_set.handle != 0)
				cmd_list->bind_descriptor_sets(api::pipeline_type::compute, effect.layout, sampler_with_resource_view ? 2 : 3, 1, &pass_data.storage_set);

			cmd_list->dispatch(pass_info.viewport_width, pass_info.viewport_height, pass_info.viewport_dispatch_z);

			cmd_list->barrier(static_cast<uint32_t>(pass_data.modified_resources.size()), pass_data.modified_resources.data(), state_new.data(), state_old.data());
		}
		else
		{
			cmd_list->bind_pipeline(api::pipeline_type::graphics, pass_data.pipeline);

			// Transition resource state for render targets
			std::vector<api::resource_usage> state_old(pass_data.modified_resources.size(), api::resource_usage::shader_resource);
			std::vector<api::resource_usage> state_new(pass_data.modified_resources.size(), api::resource_usage::render_target);
			cmd_list->barrier(static_cast<uint32_t>(pass_data.modified_resources.size()), pass_data.modified_resources.data(), state_old.data(), state_new.data());

			// Setup render targets
			if (pass_info.stencil_enable && !is_effect_stencil_cleared)
			{
				is_effect_stencil_cleared = true;

				cmd_list->clear_depth_stencil_view(_effect_stencil_view, 0x2, 1.0f, 0);
			}

			const float clear_color[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

			if (pass_data.num_render_targets == 0)
			{
				needs_implicit_backbuffer_copy = true;

				const api::resource_view rtv = get_backbuffer(pass_info.srgb_write_enable);

				if (pass_info.clear_render_targets)
					cmd_list->clear_render_target_view(rtv, clear_color);

				cmd_list->begin_render_pass(
					1, &rtv,
					pass_info.stencil_enable ? _effect_stencil_view : api::resource_view { 0 });
			}
			else
			{
				needs_implicit_backbuffer_copy = false;

				if (pass_info.clear_render_targets)
					cmd_list->clear_render_target_views(pass_data.num_render_targets, pass_data.render_targets, clear_color);

				cmd_list->begin_render_pass(
					pass_data.num_render_targets, pass_data.render_targets,
					pass_info.stencil_enable && pass_info.viewport_width == _width && pass_info.viewport_height == _height ? _effect_stencil_view : api::resource_view { 0 });
			}

			// Setup shader resources after binding render targets, to ensure any OM bindings by the application are unset at this point
			// Otherwise a slot referencing a resource still bound to the OM would be filled with NULL, which can happen with the depth buffer (https://docs.microsoft.com/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-pssetshaderresources)
			if (pass_data.texture_set.handle != 0)
				cmd_list->bind_descriptor_sets(api::pipeline_type::graphics, effect.layout, sampler_with_resource_view ? 1 : 2, 1, &pass_data.texture_set);

			const float viewport[6] = {
				0.0f, 0.0f,
				static_cast<float>(pass_info.viewport_width),
				static_cast<float>(pass_info.viewport_height),
				0.0f, 1.0f
			};
			cmd_list->bind_viewports(0, 1, viewport);
			const int32_t scissor_rect[4] = {
				0, 0,
				static_cast<int32_t>(pass_info.viewport_width),
				static_cast<int32_t>(pass_info.viewport_height)
			};
			cmd_list->bind_scissor_rects(0, 1, scissor_rect);

			if (device->get_api() == api::device_api::d3d9)
			{
				// Set __TEXEL_SIZE__ constant (see effect_codegen_hlsl.cpp)
				const float texel_size[4] = {
					-1.0f / pass_info.viewport_width,
					 1.0f / pass_info.viewport_height
				};
				cmd_list->push_constants(api::shader_stage::vertex, effect.layout, 0, 255 * 4, 4, reinterpret_cast<const uint32_t *>(texel_size));
			}

			// Draw primitives
			cmd_list->draw(pass_info.num_vertices, 1, 0, 0);

			cmd_list->finish_render_pass();

			// Transition resource state back to shader access
			cmd_list->barrier(static_cast<uint32_t>(pass_data.modified_resources.size()), pass_data.modified_resources.data(), state_new.data(), state_old.data());
		}

		// Generate mipmaps for modified resources
		for (const auto modified_texture : pass_data.generate_mipmap_views)
			cmd_list->generate_mipmaps(modified_texture);

#ifndef NDEBUG
		cmd_list->finish_debug_marker();
#endif
	}

#ifndef NDEBUG
	cmd_list->finish_debug_marker();
#endif

	if (_gather_gpu_statistics)
		cmd_list->finish_query(effect.query_heap, api::query_type::timestamp, technique.query_base_index + (_framecount % NUM_QUERY_FRAMES) * 2 + 1);

#if RESHADE_ADDON
	invoke_addon_event<addon_event::reshade_after_effects>(this, cmd_list);
#endif
}

void reshade::runtime::enable_technique(technique &technique)
{
	assert(technique.effect_index < _effects.size());

	if (!_effects[technique.effect_index].compiled)
		return; // Cannot enable techniques that failed to compile

	const bool status_changed = !technique.enabled;
	technique.enabled = true;
	technique.time_left = technique.annotation_as_int("timeout");

	// Queue effect file for compilation if it was not fully loaded yet
	if (technique.passes_data.empty() && // Avoid adding the same effect multiple times to the queue if it contains multiple techniques that were enabled simultaneously
		std::find(_reload_compile_queue.begin(), _reload_compile_queue.end(), technique.effect_index) == _reload_compile_queue.end())
		_reload_compile_queue.push_back(technique.effect_index);

	if (status_changed) // Increase rendering reference count
		_effects[technique.effect_index].rendering++;
}
void reshade::runtime::disable_technique(technique &technique)
{
	assert(technique.effect_index < _effects.size());

	const bool status_changed =  technique.enabled;
	technique.enabled = false;
	technique.time_left = 0;
	technique.average_cpu_duration.clear();
	technique.average_gpu_duration.clear();

	if (status_changed) // Decrease rendering reference count
		_effects[technique.effect_index].rendering--;
}

void reshade::runtime::subscribe_to_load_config(std::function<void(const ini_file &)> function)
{
	_load_config_callables.push_back(function);

	function(ini_file::load_cache(_config_path));
}
void reshade::runtime::subscribe_to_save_config(std::function<void(ini_file &)> function)
{
	_save_config_callables.push_back(function);

	function(ini_file::load_cache(_config_path));
}

void reshade::runtime::load_config()
{
	const ini_file &config = ini_file::load_cache(_config_path);

	config.get("INPUT", "ForceShortcutModifiers", _force_shortcut_modifiers);
	config.get("INPUT", "KeyEffects", _effects_key_data);
	config.get("INPUT", "KeyNextPreset", _next_preset_key_data);
	config.get("INPUT", "KeyPerformanceMode", _performance_mode_key_data);
	config.get("INPUT", "KeyPreviousPreset", _prev_preset_key_data);
	config.get("INPUT", "KeyReload", _reload_key_data);
	config.get("INPUT", "KeyScreenshot", _screenshot_key_data);

	config.get("GENERAL", "NoDebugInfo", _no_debug_info);
	config.get("GENERAL", "NoEffectCache", _no_effect_cache);
	config.get("GENERAL", "NoReloadOnInit", _no_reload_on_init);

	config.get("GENERAL", "EffectSearchPaths", _effect_search_paths);
	config.get("GENERAL", "PerformanceMode", _performance_mode);
	config.get("GENERAL", "PreprocessorDefinitions", _global_preprocessor_definitions);
	config.get("GENERAL", "SkipLoadingDisabledEffects", _effect_load_skipping);
	config.get("GENERAL", "TextureSearchPaths", _texture_search_paths);
	config.get("GENERAL", "IntermediateCachePath", _intermediate_cache_path);

	config.get("GENERAL", "PresetPath", _current_preset_path);
	config.get("GENERAL", "PresetTransitionDelay", _preset_transition_delay);

	config.get("GENERAL", "GatherGPUStatistics", _gather_gpu_statistics);

	// Fall back to temp directory if cache path does not exist
	if (_intermediate_cache_path.empty() || !resolve_path(_intermediate_cache_path))
	{
		WCHAR temp_path[MAX_PATH] = L"";
		GetTempPathW(MAX_PATH, temp_path);
		_intermediate_cache_path = temp_path;
	}

	// Use default if the preset file does not exist yet
	if (!resolve_preset_path(_current_preset_path))
		_current_preset_path = g_reshade_base_path / L"ReShadePreset.ini";

	config.get("SCREENSHOT", "ClearAlpha", _screenshot_clear_alpha);
	config.get("SCREENSHOT", "FileFormat", _screenshot_format);
	config.get("SCREENSHOT", "FileNamingFormat", _screenshot_naming);
	config.get("SCREENSHOT", "JPEGQuality", _screenshot_jpeg_quality);
	config.get("SCREENSHOT", "SaveBeforeShot", _screenshot_save_before);
	config.get("SCREENSHOT", "SaveOverlayShot", _screenshot_save_gui);
	config.get("SCREENSHOT", "SavePath", _screenshot_path);
	config.get("SCREENSHOT", "SavePresetFile", _screenshot_include_preset);

	for (const auto &callback : _load_config_callables)
		callback(config);
}
void reshade::runtime::save_config() const
{
	ini_file &config = ini_file::load_cache(_config_path);

	config.set("INPUT", "ForceShortcutModifiers", _force_shortcut_modifiers);
	config.set("INPUT", "KeyEffects", _effects_key_data);
	config.set("INPUT", "KeyNextPreset", _next_preset_key_data);
	config.set("INPUT", "KeyPerformanceMode", _performance_mode_key_data);
	config.set("INPUT", "KeyPreviousPreset", _prev_preset_key_data);
	config.set("INPUT", "KeyReload", _reload_key_data);
	config.set("INPUT", "KeyScreenshot", _screenshot_key_data);

	config.set("GENERAL", "NoDebugInfo", _no_debug_info);
	config.set("GENERAL", "NoEffectCache", _no_effect_cache);
	config.set("GENERAL", "NoReloadOnInit", _no_reload_on_init);

	config.set("GENERAL", "EffectSearchPaths", _effect_search_paths);
	config.set("GENERAL", "PerformanceMode", _performance_mode);
	config.set("GENERAL", "PreprocessorDefinitions", _global_preprocessor_definitions);
	config.set("GENERAL", "SkipLoadingDisabledEffects", _effect_load_skipping);
	config.set("GENERAL", "TextureSearchPaths", _texture_search_paths);
	config.set("GENERAL", "IntermediateCachePath", _intermediate_cache_path);

	// Use ReShade DLL directory as base for relative preset paths (see 'resolve_preset_path')
	std::filesystem::path relative_preset_path = _current_preset_path.lexically_proximate(g_reshade_base_path);
	if (relative_preset_path.wstring().rfind(L"..", 0) != std::wstring::npos)
		relative_preset_path = _current_preset_path; // Do not use relative path if preset is in a parent directory
	if (relative_preset_path.is_relative()) // Prefix preset path with dot character to better indicate it being a relative path
		relative_preset_path = L"." / relative_preset_path;
	config.set("GENERAL", "PresetPath", relative_preset_path);
	config.set("GENERAL", "PresetTransitionDelay", _preset_transition_delay);

	config.set("GENERAL", "GatherGPUStatistics", _gather_gpu_statistics);

	config.set("SCREENSHOT", "ClearAlpha", _screenshot_clear_alpha);
	config.set("SCREENSHOT", "FileFormat", _screenshot_format);
	config.set("SCREENSHOT", "FileNamingFormat", _screenshot_naming);
	config.set("SCREENSHOT", "JPEGQuality", _screenshot_jpeg_quality);
	config.set("SCREENSHOT", "SaveBeforeShot", _screenshot_save_before);
	config.set("SCREENSHOT", "SaveOverlayShot", _screenshot_save_gui);
	config.set("SCREENSHOT", "SavePath", _screenshot_path);
	config.set("SCREENSHOT", "SavePresetFile", _screenshot_include_preset);

	for (const auto &callback : _save_config_callables)
		callback(config);
}

void reshade::runtime::load_current_preset()
{
	_preset_save_success = true;

	ini_file config = ini_file::load_cache(_config_path); // Copy config, because reference becomes invalid in the next line
	const ini_file &preset = ini_file::load_cache(_current_preset_path);

	std::vector<std::string> technique_list;
	preset.get({}, "Techniques", technique_list);
	std::vector<std::string> sorted_technique_list;
	preset.get({}, "TechniqueSorting", sorted_technique_list);
	std::vector<std::string> preset_preprocessor_definitions;
	preset.get({}, "PreprocessorDefinitions", preset_preprocessor_definitions);

	// Recompile effects if preprocessor definitions have changed or running in performance mode (in which case all preset values are compile-time constants)
	if (_reload_remaining_effects != 0) // ... unless this is the 'load_current_preset' call in 'update_and_render_effects'
	{
		if (_performance_mode || preset_preprocessor_definitions != _preset_preprocessor_definitions)
		{
			_preset_preprocessor_definitions = std::move(preset_preprocessor_definitions);
			reload_effects();
			return; // Preset values are loaded in 'update_and_render_effects' during effect loading
		}

		if (std::find_if(technique_list.begin(), technique_list.end(), [this](const std::string &technique) {
				if (const size_t at_pos = technique.find('@'); at_pos == std::string::npos)
					return true;
				else if (const auto it = std::find_if(_effects.begin(), _effects.end(),
					[effect_name = static_cast<std::string_view>(technique).substr(at_pos + 1)](const effect &effect) { return effect_name == effect.source_file.filename().u8string(); }); it == _effects.end())
					return true;
				else
					return it->skipped; }) != technique_list.end())
		{
			reload_effects();
			return;
		}
	}

	if (sorted_technique_list.empty())
		config.get("GENERAL", "TechniqueSorting", sorted_technique_list);
	if (sorted_technique_list.empty())
		sorted_technique_list = technique_list;

	// Reorder techniques
	std::sort(_techniques.begin(), _techniques.end(), [this, &sorted_technique_list](const technique &lhs, const technique &rhs) {
		const std::string lhs_unique = lhs.name + '@' + _effects[lhs.effect_index].source_file.filename().u8string();
		const std::string rhs_unique = rhs.name + '@' + _effects[rhs.effect_index].source_file.filename().u8string();
		auto lhs_it = std::find(sorted_technique_list.begin(), sorted_technique_list.end(), lhs_unique);
		auto rhs_it = std::find(sorted_technique_list.begin(), sorted_technique_list.end(), rhs_unique);
		if (lhs_it == sorted_technique_list.end())
			lhs_it = std::find(sorted_technique_list.begin(), sorted_technique_list.end(), lhs.name);
		if (rhs_it == sorted_technique_list.end())
			rhs_it = std::find(sorted_technique_list.begin(), sorted_technique_list.end(), rhs.name);
		return lhs_it < rhs_it; });

	// Compute times since the transition has started and how much is left till it should end
	auto transition_time = std::chrono::duration_cast<std::chrono::microseconds>(_last_present_time - _last_preset_switching_time).count();
	auto transition_ms_left = _preset_transition_delay - transition_time / 1000;
	auto transition_ms_left_from_last_frame = transition_ms_left + std::chrono::duration_cast<std::chrono::microseconds>(_last_frame_duration).count() / 1000;

	if (_is_in_between_presets_transition && transition_ms_left <= 0)
		_is_in_between_presets_transition = false;

	for (effect &effect : _effects)
	{
		for (uniform &variable : effect.uniforms)
		{
			if (variable.special != special_uniform::none)
				continue;
			const std::string section = effect.source_file.filename().u8string();

			if (variable.supports_toggle_key())
			{
				if (!preset.get(section, "Key" + variable.name, variable.toggle_key_data))
					std::memset(variable.toggle_key_data, 0, sizeof(variable.toggle_key_data));
			}

			// Reset values to defaults before loading from a new preset
			if (!_is_in_between_presets_transition)
				reset_uniform_value(variable);

			reshadefx::constant values, values_old;
			switch (variable.type.base)
			{
			case reshadefx::type::t_int:
				get_uniform_value(variable, values.as_int, variable.type.components());
				preset.get(section, variable.name, values.as_int);
				set_uniform_value(variable, values.as_int, variable.type.components());
				break;
			case reshadefx::type::t_bool:
			case reshadefx::type::t_uint:
				get_uniform_value(variable, values.as_uint, variable.type.components());
				preset.get(section, variable.name, values.as_uint);
				set_uniform_value(variable, values.as_uint, variable.type.components());
				break;
			case reshadefx::type::t_float:
				get_uniform_value(variable, values.as_float, variable.type.components());
				values_old = values;
				preset.get(section, variable.name, values.as_float);
				if (_is_in_between_presets_transition)
				{
					// Perform smooth transition on floating point values
					for (unsigned int i = 0; i < variable.type.components(); i++)
					{
						const auto transition_ratio = (values.as_float[i] - values_old.as_float[i]) / transition_ms_left_from_last_frame;
						values.as_float[i] = values.as_float[i] - transition_ratio * transition_ms_left;
					}
				}
				set_uniform_value(variable, values.as_float, variable.type.components());
				break;
			}
		}
	}

	for (technique &technique : _techniques)
	{
		const std::string unique_name =
			technique.name + '@' + _effects[technique.effect_index].source_file.filename().u8string();

		// Ignore preset if "enabled" annotation is set
		if (technique.annotation_as_int("enabled") ||
			std::find(technique_list.begin(), technique_list.end(), unique_name) != technique_list.end() ||
			std::find(technique_list.begin(), technique_list.end(), technique.name) != technique_list.end())
			enable_technique(technique);
		else
			disable_technique(technique);

		if (!preset.get({}, "Key" + unique_name, technique.toggle_key_data) &&
			!preset.get({}, "Key" + technique.name, technique.toggle_key_data))
		{
			technique.toggle_key_data[0] = technique.annotation_as_int("toggle");
			technique.toggle_key_data[1] = technique.annotation_as_int("togglectrl");
			technique.toggle_key_data[2] = technique.annotation_as_int("toggleshift");
			technique.toggle_key_data[3] = technique.annotation_as_int("togglealt");
		}
	}

	// Reverse compile queue so that effects are enabled in the order they are defined in the preset (since the queue is worked from back to front)
	std::reverse(_reload_compile_queue.begin(), _reload_compile_queue.end());
}
void reshade::runtime::save_current_preset() const
{
	ini_file &preset = ini_file::load_cache(_current_preset_path);

	// Build list of active techniques and effects
	std::vector<std::string> technique_list, sorted_technique_list;
	std::unordered_set<size_t> effect_list;
	effect_list.reserve(_techniques.size());
	technique_list.reserve(_techniques.size());
	sorted_technique_list.reserve(_techniques.size());

	for (const technique &technique : _techniques)
	{
		const std::string unique_name =
			technique.name + '@' + _effects[technique.effect_index].source_file.filename().u8string();

		if (technique.enabled)
			technique_list.push_back(unique_name);
		if (technique.enabled || technique.toggle_key_data[0] != 0)
			effect_list.insert(technique.effect_index);

		// Keep track of the order of all techniques and not just the enabled ones
		sorted_technique_list.push_back(unique_name);

		if (technique.toggle_key_data[0] != 0)
			preset.set({}, "Key" + unique_name, technique.toggle_key_data);
		else if (technique.annotation_as_int("toggle") != 0)
			preset.set({}, "Key" + unique_name, 0); // Overwrite default toggle key to none
		else
			preset.remove_key({}, "Key" + unique_name);
	}

	preset.set({}, "Techniques", std::move(technique_list));
	preset.set({}, "TechniqueSorting", std::move(sorted_technique_list));
	preset.set({}, "PreprocessorDefinitions", _preset_preprocessor_definitions);

	// TODO: Do we want to save spec constants here too? The preset will be rather empty in performance mode otherwise.
	for (size_t effect_index = 0; effect_index < _effects.size(); ++effect_index)
	{
		if (effect_list.find(effect_index) == effect_list.end())
			continue;

		const effect &effect = _effects[effect_index];

		for (const uniform &variable : effect.uniforms)
		{
			if (variable.special != special_uniform::none)
				continue;

			const std::string section = effect.source_file.filename().u8string();
			const unsigned int components = variable.type.components();
			reshadefx::constant values;

			if (variable.supports_toggle_key())
			{
				// save the shortcut key into the preset files
				if (variable.toggle_key_data[0] != 0)
					preset.set(section, "Key" + variable.name, variable.toggle_key_data);
				else
					preset.remove_key(section, "Key" + variable.name);
			}

			switch (variable.type.base)
			{
			case reshadefx::type::t_int:
				get_uniform_value(variable, values.as_int, components);
				preset.set(section, variable.name, values.as_int, components);
				break;
			case reshadefx::type::t_bool:
			case reshadefx::type::t_uint:
				get_uniform_value(variable, values.as_uint, components);
				preset.set(section, variable.name, values.as_uint, components);
				break;
			case reshadefx::type::t_float:
				get_uniform_value(variable, values.as_float, components);
				preset.set(section, variable.name, values.as_float, components);
				break;
			}
		}
	}
}

bool reshade::runtime::switch_to_next_preset(std::filesystem::path filter_path, bool reversed)
{
	std::error_code ec; // This is here to ignore file system errors below

	std::filesystem::path filter_text;
	if (resolve_path(filter_path); !std::filesystem::is_directory(filter_path, ec))
		if (filter_text = filter_path.filename(); !filter_text.empty())
			filter_path = filter_path.parent_path();

	size_t current_preset_index = std::numeric_limits<size_t>::max();
	std::vector<std::filesystem::path> preset_paths;

	for (std::filesystem::path preset_path : std::filesystem::directory_iterator(filter_path, std::filesystem::directory_options::skip_permission_denied, ec))
	{
		// Skip anything that is not a valid preset file
		if (!resolve_preset_path(preset_path))
			continue;

		// Keep track of the index of the current preset in the list of found preset files that is being build
		if (std::filesystem::equivalent(preset_path, _current_preset_path, ec)) {
			current_preset_index = preset_paths.size();
			preset_paths.push_back(std::move(preset_path));
			continue;
		}

		const std::wstring preset_name = preset_path.stem();
		// Only add those files that are matching the filter text
		if (filter_text.empty() || std::search(preset_name.begin(), preset_name.end(), filter_text.native().begin(), filter_text.native().end(),
			[](wchar_t c1, wchar_t c2) { return towlower(c1) == towlower(c2); }) != preset_name.end())
			preset_paths.push_back(std::move(preset_path));
	}

	if (preset_paths.begin() == preset_paths.end())
		return false; // No valid preset files were found, so nothing more to do

	if (current_preset_index == std::numeric_limits<size_t>::max())
	{
		// Current preset was not in the container path, so just use the first or last file
		if (reversed)
			_current_preset_path = preset_paths.back();
		else
			_current_preset_path = preset_paths.front();
	}
	else
	{
		// Current preset was found in the container path, so use the file before or after it
		if (auto it = std::next(preset_paths.begin(), current_preset_index); reversed)
			_current_preset_path = it == preset_paths.begin() ? preset_paths.back() : *--it;
		else
			_current_preset_path = it == std::prev(preset_paths.end()) ? preset_paths.front() : *++it;
	}

	return true;
}

void reshade::runtime::save_screenshot(const std::wstring &postfix, const bool should_save_preset)
{
	char timestamp[21];
	const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
	tm tm; localtime_s(&tm, &t);
	sprintf_s(timestamp, " %.4d-%.2d-%.2d %.2d-%.2d-%.2d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);

	std::wstring filename = g_target_executable_path.stem().concat(timestamp);
	if (_screenshot_naming == 1)
		filename += L' ' + _current_preset_path.stem().wstring();

	filename += postfix;
	filename += _screenshot_format == 0 ? L".bmp" : _screenshot_format == 1 ? L".png" : L".jpg";

	std::filesystem::path screenshot_path = g_reshade_base_path / _screenshot_path / filename;

	LOG(INFO) << "Saving screenshot to " << screenshot_path << " ...";

	_screenshot_save_success = false; // Default to a save failure unless it is reported to succeed below

	if (std::vector<uint8_t> data(_width * _height * 4); capture_screenshot(data.data()))
	{
		// Clear alpha channel
		// The alpha channel doesn't need to be cleared if we're saving a JPEG, stbi ignores it
		if (_screenshot_clear_alpha && _screenshot_format != 2)
			for (uint32_t h = 0; h < _height; ++h)
				for (uint32_t w = 0; w < _width; ++w)
					data[(h * _width + w) * 4 + 3] = 0xFF;

		if (FILE *file; _wfopen_s(&file, screenshot_path.c_str(), L"wb") == 0)
		{
			const auto write_callback = [](void *context, void *data, int size) {
				fwrite(data, 1, size, static_cast<FILE *>(context));
			};

			switch (_screenshot_format)
			{
			case 0:
				_screenshot_save_success = stbi_write_bmp_to_func(write_callback, file, _width, _height, 4, data.data()) != 0;
				break;
			case 1:
				_screenshot_save_success = stbi_write_png_to_func(write_callback, file, _width, _height, 4, data.data(), 0) != 0;
				break;
			case 2:
				_screenshot_save_success = stbi_write_jpg_to_func(write_callback, file, _width, _height, 4, data.data(), _screenshot_jpeg_quality) != 0;
				break;
			}

			fclose(file);
		}
	}

	_last_screenshot_file = screenshot_path;
	_last_screenshot_time = std::chrono::high_resolution_clock::now();

	if (!_screenshot_save_success)
	{
		LOG(ERROR) << "Failed to write screenshot to " << screenshot_path << '!';
	}
	else if (_screenshot_include_preset && should_save_preset && ini_file::flush_cache(_current_preset_path))
	{
		// Preset was flushed to disk, so can just copy it over to the new location
		std::error_code ec; std::filesystem::copy_file(_current_preset_path, screenshot_path.replace_extension(L".ini"), std::filesystem::copy_options::overwrite_existing, ec);
	}
}

static inline bool force_floating_point_value(const reshadefx::type &type, uint32_t renderer_id)
{
	if (renderer_id == 0x9000)
		return true; // All uniform variables are floating-point in D3D9
	if (type.is_matrix() && (renderer_id & 0x10000))
		return true; // All matrices are floating-point in GLSL
	return false;
}

void reshade::runtime::get_uniform_value(const uniform &variable, uint8_t *data, size_t size, size_t base_index) const
{
	size = std::min(size, static_cast<size_t>(variable.size));
	assert(data != nullptr && (size % 4) == 0);

	auto &data_storage = _effects[variable.effect_index].uniform_data_storage;
	assert(variable.offset + size <= data_storage.size());

	const size_t array_length = (variable.type.is_array() ? variable.type.array_length : 1);
	if (assert(base_index < array_length); base_index >= array_length)
		return;

	if (variable.type.is_matrix())
	{
		for (size_t a = base_index, i = 0; a < array_length; ++a)
			// Each row of a matrix is 16-byte aligned, so needs special handling
			for (size_t row = 0; row < variable.type.rows; ++row)
				for (size_t col = 0; i < (size / 4) && col < variable.type.cols; ++col, ++i)
					std::memcpy(
						data + ((a - base_index) * variable.type.components() + (row * variable.type.cols + col)) * 4,
						data_storage.data() + variable.offset + (a * (variable.type.rows * 4) + (row * 4 + col)) * 4, 4);
	}
	else if (array_length > 1)
	{
		for (size_t a = base_index, i = 0; a < array_length; ++a)
			// Each element in the array is 16-byte aligned, so needs special handling
			for (size_t row = 0; i < (size / 4) && row < variable.type.rows; ++row, ++i)
				std::memcpy(
					data + ((a - base_index) * variable.type.components() + row) * 4,
					data_storage.data() + variable.offset + (a * 4 + row) * 4, 4);
	}
	else
	{
		std::memcpy(data, data_storage.data() + variable.offset, size);
	}
}
void reshade::runtime::get_uniform_value(const uniform &variable, bool *values, size_t count, size_t array_index) const
{
	count = std::min(count, static_cast<size_t>(variable.size / 4));
	assert(values != nullptr);

	const auto data = static_cast<uint8_t *>(alloca(variable.size));
	get_uniform_value(variable, data, variable.size, array_index);

	for (size_t i = 0; i < count; i++)
		values[i] = reinterpret_cast<const uint32_t *>(data)[i] != 0;
}
void reshade::runtime::get_uniform_value(const uniform &variable, int32_t *values, size_t count, size_t array_index) const
{
	if (variable.type.is_integral() && !force_floating_point_value(variable.type, _renderer_id))
	{
		get_uniform_value(variable, reinterpret_cast<uint8_t *>(values), count * sizeof(int32_t), array_index);
		return;
	}

	count = std::min(count, static_cast<size_t>(variable.size / 4));
	assert(values != nullptr);

	const auto data = static_cast<uint8_t *>(alloca(variable.size));
	get_uniform_value(variable, data, variable.size, array_index);

	for (size_t i = 0; i < count; i++)
		values[i] = static_cast<int32_t>(reinterpret_cast<const float *>(data)[i]);
}
void reshade::runtime::get_uniform_value(const uniform &variable, uint32_t *values, size_t count, size_t array_index) const
{
	get_uniform_value(variable, reinterpret_cast<int32_t *>(values), count, array_index);
}
void reshade::runtime::get_uniform_value(const uniform &variable, float *values, size_t count, size_t array_index) const
{
	if (variable.type.is_floating_point() || force_floating_point_value(variable.type, _renderer_id))
	{
		get_uniform_value(variable, reinterpret_cast<uint8_t *>(values), count * sizeof(float), array_index);
		return;
	}

	count = std::min(count, static_cast<size_t>(variable.size / 4));
	assert(values != nullptr);

	const auto data = static_cast<uint8_t *>(alloca(variable.size));
	get_uniform_value(variable, data, variable.size, array_index);

	for (size_t i = 0; i < count; ++i)
		if (variable.type.is_signed())
			values[i] = static_cast<float>(reinterpret_cast<const int32_t *>(data)[i]);
		else
			values[i] = static_cast<float>(reinterpret_cast<const uint32_t *>(data)[i]);
}
void reshade::runtime::set_uniform_value(uniform &variable, const uint8_t *data, size_t size, size_t base_index)
{
	size = std::min(size, static_cast<size_t>(variable.size));
	assert(data != nullptr && (size % 4) == 0);

	auto &data_storage = _effects[variable.effect_index].uniform_data_storage;
	assert(variable.offset + size <= data_storage.size());

	const size_t array_length = (variable.type.is_array() ? variable.type.array_length : 1);
	if (assert(base_index < array_length); base_index >= array_length)
		return;

	if (variable.type.is_matrix())
	{
		for (size_t a = base_index, i = 0; a < array_length; ++a)
			// Each row of a matrix is 16-byte aligned, so needs special handling
			for (size_t row = 0; row < variable.type.rows; ++row)
				for (size_t col = 0; i < (size / 4) && col < variable.type.cols; ++col, ++i)
					std::memcpy(
						data_storage.data() + variable.offset + (a * variable.type.rows * 4 + (row * 4 + col)) * 4,
						data + ((a - base_index) * variable.type.components() + (row * variable.type.cols + col)) * 4, 4);
	}
	else if (array_length > 1)
	{
		for (size_t a = base_index, i = 0; a < array_length; ++a)
			// Each element in the array is 16-byte aligned, so needs special handling
			for (size_t row = 0; i < (size / 4) && row < variable.type.rows; ++row, ++i)
				std::memcpy(
					data_storage.data() + variable.offset + (a * 4 + row) * 4,
					data + ((a - base_index) * variable.type.components() + row) * 4, 4);
	}
	else
	{
		std::memcpy(data_storage.data() + variable.offset, data, size);
	}
}
void reshade::runtime::set_uniform_value(uniform &variable, const bool *values, size_t count, size_t array_index)
{
	if (variable.type.is_floating_point() || force_floating_point_value(variable.type, _renderer_id))
	{
		const auto data = static_cast<float *>(alloca(count * sizeof(float)));
		for (size_t i = 0; i < count; ++i)
			data[i] = values[i] ? 1.0f : 0.0f;

		set_uniform_value(variable, reinterpret_cast<const uint8_t *>(data), count * sizeof(float), array_index);
	}
	else
	{
		const auto data = static_cast<uint32_t *>(alloca(count * sizeof(uint32_t)));
		for (size_t i = 0; i < count; ++i)
			data[i] = values[i] ? 1 : 0;

		set_uniform_value(variable, reinterpret_cast<const uint8_t *>(data), count * sizeof(uint32_t), array_index);
	}
}
void reshade::runtime::set_uniform_value(uniform &variable, const int32_t *values, size_t count, size_t array_index)
{
	if (variable.type.is_floating_point() || force_floating_point_value(variable.type, _renderer_id))
	{
		const auto data = static_cast<float *>(alloca(count * sizeof(float)));
		for (size_t i = 0; i < count; ++i)
			data[i] = static_cast<float>(values[i]);

		set_uniform_value(variable, reinterpret_cast<const uint8_t *>(data), count * sizeof(float), array_index);
	}
	else
	{
		set_uniform_value(variable, reinterpret_cast<const uint8_t *>(values), count * sizeof(int32_t), array_index);
	}
}
void reshade::runtime::set_uniform_value(uniform &variable, const uint32_t *values, size_t count, size_t array_index)
{
	if (variable.type.is_floating_point() || force_floating_point_value(variable.type, _renderer_id))
	{
		const auto data = static_cast<float *>(alloca(count * sizeof(float)));
		for (size_t i = 0; i < count; ++i)
			data[i] = static_cast<float>(values[i]);

		set_uniform_value(variable, reinterpret_cast<const uint8_t *>(data), count * sizeof(float), array_index);
	}
	else
	{
		set_uniform_value(variable, reinterpret_cast<const uint8_t *>(values), count * sizeof(uint32_t), array_index);
	}
}
void reshade::runtime::set_uniform_value(uniform &variable, const float *values, size_t count, size_t array_index)
{
	if (variable.type.is_floating_point() || force_floating_point_value(variable.type, _renderer_id))
	{
		set_uniform_value(variable, reinterpret_cast<const uint8_t *>(values), count * sizeof(float), array_index);
	}
	else
	{
		const auto data = static_cast<int32_t *>(alloca(count * sizeof(int32_t)));
		for (size_t i = 0; i < count; ++i)
			data[i] = static_cast<int32_t>(values[i]);

		set_uniform_value(variable, reinterpret_cast<const uint8_t *>(data), count * sizeof(int32_t), array_index);
	}
}

void reshade::runtime::reset_uniform_value(uniform &variable)
{
	if (!variable.has_initializer_value)
	{
		std::memset(_effects[variable.effect_index].uniform_data_storage.data() + variable.offset, 0, variable.size);
		return;
	}

	// Need to use typed setters, to ensure values are properly forced to floating point in D3D9
	for (size_t i = 0, array_length = (variable.type.is_array() ? variable.type.array_length : 1);
		i < array_length; ++i)
	{
		const reshadefx::constant &value = variable.type.is_array() ? variable.initializer_value.array_data[i] : variable.initializer_value;

		switch (variable.type.base)
		{
		case reshadefx::type::t_int:
			set_uniform_value(variable, value.as_int, variable.type.components(), i);
			break;
		case reshadefx::type::t_bool:
		case reshadefx::type::t_uint:
			set_uniform_value(variable, value.as_uint, variable.type.components(), i);
			break;
		case reshadefx::type::t_float:
			set_uniform_value(variable, value.as_float, variable.type.components(), i);
			break;
		}
	}
}

void reshade::runtime::update_uniform_variables(const char *source, const bool *values, size_t count, size_t array_index)
{
	if (is_loading())
		return;

	for (effect &effect : _effects)
		for (uniform &variable : effect.uniforms)
			if (variable.annotation_as_string("source") == source)
				set_uniform_value(variable, values, count, array_index);
}
void reshade::runtime::update_uniform_variables(const char *source, const float *values, size_t count, size_t array_index)
{
	if (is_loading())
		return;

	for (effect &effect : _effects)
		for (uniform &variable : effect.uniforms)
			if (variable.annotation_as_string("source") == source)
				set_uniform_value(variable, values, count, array_index);
}
void reshade::runtime::update_uniform_variables(const char *source, const int32_t *values, size_t count, size_t array_index)
{
	if (is_loading())
		return;

	for (effect &effect : _effects)
		for (uniform &variable : effect.uniforms)
			if (variable.annotation_as_string("source") == source)
				set_uniform_value(variable, values, count, array_index);
}
void reshade::runtime::update_uniform_variables(const char *source, const uint32_t *values, size_t count, size_t array_index)
{
	if (is_loading())
		return;

	for (effect &effect : _effects)
		for (uniform &variable : effect.uniforms)
			if (variable.annotation_as_string("source") == source)
				set_uniform_value(variable, values, count, array_index);
}

reshade::texture &reshade::runtime::look_up_texture_by_name(const std::string &unique_name)
{
	const auto it = std::find_if(_textures.begin(), _textures.end(),
		[&unique_name](const auto &item) { return item.unique_name == unique_name && (item.resource.handle != 0 || !item.semantic.empty()); });
	assert(it != _textures.end());
	return *it;
}
