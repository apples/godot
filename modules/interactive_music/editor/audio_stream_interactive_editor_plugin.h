/**************************************************************************/
/*  audio_stream_interactive_editor_plugin.h                              */
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

#pragma once

#include "../audio_stream_interactive.h"

#include "editor/gui/editor_spin_slider.h"
#include "editor/inspector/editor_inspector.h"
#include "editor/plugins/editor_plugin.h"
#include "scene/gui/base_button.h"
#include "scene/gui/box_container.h"
#include "scene/gui/dialogs.h"

class AudioStreamPlayer;
class Button;
class CheckBox;
class HSplitContainer;
class VSplitContainer;
class Tree;
class TreeItem;

class AudioStreamInteractiveTransitionChart : public Control {
	GDCLASS(AudioStreamInteractiveTransitionChart, Control);

private:
	Ref<AudioStreamInteractive> stream;
	int from_clip;
	int to_clip;

	double cue_t = 0.0;
	double playhead_t = -1.0;

	struct {
		Ref<Font> font;
		int font_size;
		Color font_color;
		Color background_color;
		Color from_curve_color;
		Color to_curve_color;
		Color filler_bar_color;
		Color playhead_color;
	} theme_cache;

	void _on_stream_changed();

	float _get_from_volume(const AudioStreamInteractive::TransitionMixResult &mix, double p_t) const;
	float _get_to_volume(const AudioStreamInteractive::TransitionMixResult &mix, double p_t) const;

protected:
	void _notification(int p_what);

public:
	void set_transition(Ref<AudioStreamInteractive> p_stream, int p_from_clip, int p_to_clip);
	void clear_transition();

	void set_preview_state(double p_cue_t, double p_playhead_t);

	AudioStreamInteractiveTransitionChart();
};

class AudioStreamInteractiveTransitionEaseSlider : public VBoxContainer {
	GDCLASS(AudioStreamInteractiveTransitionEaseSlider, VBoxContainer);

	static constexpr double EASE_EXP_MIN = 1.0 / 128.0;
	static constexpr double EASE_EXP_MAX = 127.0 / 128.0;
	static constexpr double EASE_EXP_STEP = 1.0 / 128.0;

	enum {
		BTN_EASE_LINEAR = 0,
		BTN_EASE_IN = 1,
		BTN_EASE_OUT = 2,
		BTN_EASE_ESS = 3,
		BTN_EASE_OUT_IN = 4,
	};

	Ref<ButtonGroup> btn_group = nullptr;
	Button *btn_linear = nullptr;
	Button *btn_ease_in = nullptr;
	Button *btn_ease_out = nullptr;
	Button *btn_ease_ess = nullptr;
	Button *btn_ease_out_in = nullptr;
	EditorSpinSlider *exp_slider = nullptr;

	bool is_read_only = false;

	int _ease_to_btn(double p_ease) const;
	Button *_get_btn(int p_btn) const;
	double _normalize_ease(double p_ease, int p_btn) const;
	double _denormalize_ease(double p_ease_norm, int p_btn) const;

	void _btn_pressed(BaseButton *btn);
	void _slider_changed();

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	float get_value() const;
	void set_value(float p_value);

	void set_read_only(bool p_enable);

	AudioStreamInteractiveTransitionEaseSlider();
};

class AudioStreamInteractiveTransitionEditor : public AcceptDialog {
	GDCLASS(AudioStreamInteractiveTransitionEditor, AcceptDialog);

	AudioStreamInteractive *audio_stream_interactive = nullptr;

	HSplitContainer *split = nullptr;
	Tree *tree = nullptr;

	Vector<TreeItem *> rows;

	CheckBox *transition_enabled = nullptr;
	OptionButton *transition_from = nullptr;
	OptionButton *transition_to = nullptr;
	OptionButton *fade_mode = nullptr;
	OptionButton *transition_timing = nullptr;
	SpinBox *fade_beats = nullptr;
	SpinBox *fade_offset_beats = nullptr;
	AudioStreamInteractiveTransitionEaseSlider *fade_ease_exp = nullptr;
	SpinBox *to_fade_beats = nullptr;
	SpinBox *to_fade_offset_beats = nullptr;
	AudioStreamInteractiveTransitionEaseSlider *to_fade_ease_exp = nullptr;
	OptionButton *filler_clip = nullptr;
	SpinBox *filler_clip_offset_beats = nullptr;
	CheckBox *hold_previous = nullptr;

	AudioStreamPlayer *audio_stream_player = nullptr;
	SpinBox *preview_start_position = nullptr;
	SpinBox *preview_cue_delay = nullptr;
	Button *preview_play = nullptr;
	Button *preview_stop = nullptr;
	AudioStreamInteractiveTransitionChart *chart = nullptr;

	bool preview_cue_pending = false;

	bool updating_selection = false;
	int order_counter = 0;
	HashMap<Vector2i, int> selection_order;

	Vector<Vector2i> selected;
	Vector2i editing_transition;
	bool updating = false;

	void _cell_selected(TreeItem *p_item, int p_column, bool p_selected);
	void _update_transitions();

	void _update_selection();
	void _edited();
	void _update_chart();

	void _update_preview_controls();
	void _preview_start();
	void _preview_stop();

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	void edit(Object *p_obj);

	AudioStreamInteractiveTransitionEditor();
};

//

class EditorInspectorPluginAudioStreamInteractive : public EditorInspectorPlugin {
	GDCLASS(EditorInspectorPluginAudioStreamInteractive, EditorInspectorPlugin);

	AudioStreamInteractiveTransitionEditor *audio_stream_interactive_transition_editor = nullptr;

	void _edit(Object *p_object);

public:
	virtual bool can_handle(Object *p_object) override;
	virtual void parse_end(Object *p_object) override;

	EditorInspectorPluginAudioStreamInteractive();
};

class AudioStreamInteractiveEditorPlugin : public EditorPlugin {
	GDCLASS(AudioStreamInteractiveEditorPlugin, EditorPlugin);

public:
	virtual String get_plugin_name() const override { return "AudioStreamInteractive"; }

	AudioStreamInteractiveEditorPlugin();
};
