/**************************************************************************/
/*  audio_stream_interactive_editor_plugin.cpp                            */
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

#include "audio_stream_interactive_editor_plugin.h"

#include "../audio_stream_interactive.h"

#include "core/error/error_macros.h"
#include "core/math/math_defs.h"
#include "core/math/math_funcs.h"
#include "core/math/vector2.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/object/message_queue.h"
#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/gui/editor_spin_slider.h"
#include "editor/themes/editor_scale.h"
#include "scene/audio/audio_stream_player.h"
#include "scene/gui/base_button.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/check_box.h"
#include "scene/gui/control.h"
#include "scene/gui/label.h"
#include "scene/gui/option_button.h"
#include "scene/gui/scroll_container.h"
#include "scene/gui/spin_box.h"
#include "scene/gui/split_container.h"
#include "scene/gui/tree.h"
#include "scene/main/node.h"
#include "scene/resources/style_box_flat.h"
#include "servers/audio/audio_server.h"

void AudioStreamInteractiveTransitionChart::set_transition(Ref<AudioStreamInteractive> p_stream, int p_from_clip, int p_to_clip) {
	if (stream == p_stream && from_clip == p_from_clip && to_clip == p_to_clip) {
		queue_redraw();
		return;
	}

	if (stream.is_valid()) {
		stream->disconnect("changed", callable_mp(this, &AudioStreamInteractiveTransitionChart::_on_stream_changed));
	}

	stream = p_stream;
	from_clip = p_from_clip;
	to_clip = p_to_clip;
	cue_t = 0.0;
	playhead_t = -1.0;

	if (stream.is_valid()) {
		stream->connect("changed", callable_mp(this, &AudioStreamInteractiveTransitionChart::_on_stream_changed));
	}

	queue_redraw();
}

void AudioStreamInteractiveTransitionChart::clear_transition() {
	if (stream.is_valid()) {
		stream->disconnect("changed", callable_mp(this, &AudioStreamInteractiveTransitionChart::_on_stream_changed));
	}

	stream = nullptr;
	cue_t = 0.0;
	playhead_t = -1.0;
	queue_redraw();
}

void AudioStreamInteractiveTransitionChart::set_preview_state(double p_cue_t, double p_playhead_t) {
	cue_t = p_cue_t;
	playhead_t = p_playhead_t;
	queue_redraw();
}

void AudioStreamInteractiveTransitionChart::_on_stream_changed() {
	queue_redraw();
}

float AudioStreamInteractiveTransitionChart::_get_from_volume(const AudioStreamInteractive::TransitionMixResult &mix, double p_t) const {
	if (mix.from_fade_speed == 0.0) {
		return p_t < mix.from_fade_start_t ? 1.0 : 0.0;
	}

	double fade_t = 1.0 - (p_t - mix.from_fade_start_t) * -mix.from_fade_speed;

	if (mix.from_fade_ease_exp != 0.0) {
		return Math::ease(fade_t, mix.from_fade_ease_exp);
	}

	return CLAMP(fade_t, 0.0, 1.0);
}

float AudioStreamInteractiveTransitionChart::_get_to_volume(const AudioStreamInteractive::TransitionMixResult &mix, double p_t) const {
	if (mix.to_fade_speed == 0.0) {
		return p_t < mix.to_start_t ? 0.0 : 1.0;
	}

	double fade_t = (p_t - mix.to_start_t) * mix.to_fade_speed;

	if (mix.to_fade_ease_exp != 0.0) {
		return Math::ease(fade_t, mix.to_fade_ease_exp);
	}

	return CLAMP(fade_t, 0.0, 1.0);
}

void AudioStreamInteractiveTransitionChart::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_THEME_CHANGED: {
			theme_cache.font = get_theme_font(SceneStringName(font), SNAME("Label"));
			theme_cache.font_size = get_theme_font_size(SceneStringName(font_size), SNAME("Label"));
			theme_cache.font_color = get_theme_color(SceneStringName(font_color), SNAME("Label"));
			theme_cache.background_color = get_theme_color(SNAME("background"), EditorStringName(Editor));
			theme_cache.from_curve_color = get_theme_color(SNAME("property_color_x"), EditorStringName(Editor));
			theme_cache.to_curve_color = get_theme_color(SNAME("property_color_z"), EditorStringName(Editor));
			theme_cache.filler_bar_color = get_theme_color(SNAME("property_color_y"), EditorStringName(Editor));
			theme_cache.playhead_color = get_theme_color(SNAME("accent_color"), EditorStringName(Editor));
			queue_redraw();
		} break;

		case NOTIFICATION_DRAW: {
			float font_height = theme_cache.font->get_height(theme_cache.font_size);

			// Background.

			Size2 size = get_size();
			draw_rect(Rect2(Point2(), size), theme_cache.background_color);

			// No transition.

			if (!stream.is_valid()) {
				draw_string(theme_cache.font, Point2(0, (size.y / 2.0 - font_height / 2.0)), TTR("No transition to preview."), HORIZONTAL_ALIGNMENT_CENTER, size.x, theme_cache.font_size, Color(theme_cache.font_color, 0.5));
				return;
			}

			AudioStreamInteractive::TransitionMixResult mix = stream->mix_transition(from_clip, to_clip, cue_t, false);

			// Invalid transition.

			if (mix.from_beat_sec == 0.0) {
				draw_string(theme_cache.font, Point2(0, (size.y / 2.0 - font_height / 2.0)), TTR("Invalid transition."), HORIZONTAL_ALIGNMENT_CENTER, size.x, theme_cache.font_size, Color(theme_cache.font_color, 0.5));
				return;
			}

			bool has_filler = mix.filler_end_t != mix.filler_start_t;

			float margin = 4.0 * EDSCALE;
			float filler_height = 8.0 * EDSCALE;
			float line_width = Math::round(1.0 * EDSCALE);

			Rect2 plot = Rect2(margin, margin, size.x - margin * 2, size.y - 2 * margin);
			float curves_top = plot.position.y + (has_filler ? filler_height + margin : 0.0);
			float curves_bottom = plot.get_end().y;
			float curves_height = curves_bottom - curves_top;

			// Add a margin (trans_length / 4.0) before and after the transition just to make it easier to read.
			double trans_length = MAX(MAX(mix.from_end_t, mix.to_fade_end_t), mix.filler_end_t) - mix.start_t;
			double range_start_t = cue_t - trans_length / 4.0;
			double range_end_t = cue_t + mix.start_t + trans_length + trans_length / 4.0;
			double px_per_sec = plot.size.x / (range_end_t - range_start_t);

			auto time_to_px = [&](double p_t) -> float {
				return plot.position.x + (p_t - range_start_t) * px_per_sec;
			};

			// Beat lines, aligned to the actual beats of the "from" clip (the cue is generally not on a beat).

			Color beat_color = Color(theme_cache.font_color, 0.2);
			Color bar_color = Color(theme_cache.font_color, 0.5);
			Color start_cue_color = theme_cache.font_color;

			int bar_beats = 0;
			if (from_clip != AudioStreamInteractive::CLIP_ANY) {
				Ref<AudioStream> from_stream = stream->get_clip_stream(from_clip);
				if (from_stream.is_valid() && from_stream->get_bpm() > 0) {
					bar_beats = from_stream->get_bar_beats();
				}
			}

			int first_beat = Math::ceil(range_start_t / mix.from_beat_sec);
			int last_beat = Math::ceil(range_end_t / mix.from_beat_sec);
			// If we're going to be drawing more beat lines than there are pixels, just don't.
			if (last_beat - first_beat >= plot.size.x) {
				last_beat = first_beat;
			}

			for (int b = first_beat; b < last_beat; ++b) {
				bool is_bar = bar_beats > 0 && b % bar_beats == 0;
				float x = Math::round(time_to_px(b * mix.from_beat_sec));
				draw_line(Point2(x, plot.position.y), Point2(x, curves_bottom), is_bar ? bar_color : beat_color, line_width);
			}

			double start_t_px = Math::round(time_to_px(cue_t + mix.start_t));
			draw_line(Point2(start_t_px, plot.position.y), Point2(start_t_px, curves_bottom), start_cue_color, line_width);

			// Filler bar.

			if (has_filler) {
				float x0 = CLAMP(time_to_px(cue_t + mix.filler_start_t), plot.position.x, plot.position.x + plot.size.width);
				float x1 = CLAMP(time_to_px(cue_t + mix.filler_end_t), plot.position.x, plot.position.x + plot.size.width);
				float x2 = CLAMP(time_to_px(cue_t + mix.filler_tail_end_t), plot.position.x, plot.position.x + plot.size.width);

				if (x2 > x1) {
					Rect2 r = Rect2(x1, plot.position.y, x2 - x1, filler_height);
					draw_rect(r, Color(theme_cache.filler_bar_color, 0.25));
					draw_rect(r, Color(theme_cache.filler_bar_color, 0.5), false, line_width);
				}

				if (x1 > x0) {
					Rect2 r = Rect2(x0, plot.position.y, x1 - x0, filler_height);
					draw_rect(r, Color(theme_cache.filler_bar_color, 0.5));
					draw_rect(r, theme_cache.filler_bar_color, false, line_width);
				}
			}

			// Volume curves.

			int columns = int(plot.size.x);

			PackedVector2Array from_polyline;
			PackedVector2Array to_polyline;
			from_polyline.reserve(columns);
			to_polyline.reserve(columns);

			Color from_area_color = Color(theme_cache.from_curve_color, 0.5);
			Color to_area_color = Color(theme_cache.to_curve_color, 0.5);

			for (int i = 0; i < columns; ++i) {
				double t = Math::lerp(range_start_t, range_end_t, (double(i) + 0.5) / double(columns));

				float x = plot.position.x + i;
				float from_y = Math::round(curves_bottom - _get_from_volume(mix, t - cue_t) * curves_height);
				float to_y = Math::round(curves_bottom - _get_to_volume(mix, t - cue_t) * curves_height);

				from_polyline.append(Vector2(x, from_y));
				if (from_y < curves_bottom) {
					draw_line(Vector2(x, from_y), Vector2(x, curves_bottom), from_area_color);
				}

				to_polyline.append(Vector2(x, to_y));
				if (to_y < curves_bottom) {
					draw_line(Vector2(x, to_y), Vector2(x, curves_bottom), to_area_color);
				}
			}

			draw_polyline(from_polyline, theme_cache.from_curve_color, line_width, true);
			draw_polyline(to_polyline, theme_cache.to_curve_color, line_width, true);

			// Playhead.

			if (playhead_t >= 0.0) {
				float x = Math::round(CLAMP(time_to_px(playhead_t), plot.position.x, plot.get_end().x));
				draw_line(Point2(x, plot.position.y), Point2(x, curves_bottom), theme_cache.playhead_color, 2.0 * EDSCALE);
			}
		} break;
	}
}

AudioStreamInteractiveTransitionChart::AudioStreamInteractiveTransitionChart() {
	set_custom_minimum_size(Size2(0, 100) * EDSCALE);
	set_h_size_flags(SIZE_EXPAND_FILL);
	set_clip_contents(true);
}

////////////////////////

int AudioStreamInteractiveTransitionEaseSlider::_ease_to_btn(double p_ease) const {
	if (p_ease == 0.0) {
		return BTN_EASE_LINEAR;
	} else if (p_ease > 0.0 && p_ease < 1.0) {
		return BTN_EASE_IN;
	} else if (p_ease >= 1.0) {
		return BTN_EASE_OUT;
	} else if (p_ease <= -1.0) {
		return BTN_EASE_ESS;
	} else {
		return BTN_EASE_OUT_IN;
	}
}

Button *AudioStreamInteractiveTransitionEaseSlider::_get_btn(int p_btn) const {
	switch (p_btn) {
		case BTN_EASE_LINEAR:
			return btn_linear;
		case BTN_EASE_IN:
			return btn_ease_in;
		case BTN_EASE_OUT:
			return btn_ease_out;
		case BTN_EASE_ESS:
			return btn_ease_ess;
		case BTN_EASE_OUT_IN:
			return btn_ease_out_in;
		default:
			ERR_FAIL_V_MSG(nullptr, "Bug: Invalid p_btn.");
	}
}

double AudioStreamInteractiveTransitionEaseSlider::_normalize_ease(double p_ease, int p_btn) const {
	switch (p_btn) {
		case BTN_EASE_LINEAR:
			return 0.0;
		case BTN_EASE_IN:
			return CLAMP(1.0 - p_ease, EASE_EXP_MIN, EASE_EXP_MAX);
		case BTN_EASE_OUT:
			return CLAMP(Math::atan(p_ease - 1.0) * 2.0 / Math::PI, EASE_EXP_MIN, EASE_EXP_MAX);
		case BTN_EASE_ESS:
			return CLAMP(Math::atan(-p_ease - 1.0) * 2.0 / Math::PI, EASE_EXP_MIN, EASE_EXP_MAX);
		case BTN_EASE_OUT_IN:
			return CLAMP(1.0 + p_ease, EASE_EXP_MIN, EASE_EXP_MAX);
		default:
			ERR_FAIL_V_MSG(0.0, "Bug: Invalid p_btn.");
	}
}

double AudioStreamInteractiveTransitionEaseSlider::_denormalize_ease(double p_ease_norm, int p_btn) const {
	switch (p_btn) {
		case BTN_EASE_LINEAR:
			return 0.0;
		case BTN_EASE_IN:
			return 1.0 - p_ease_norm;
		case BTN_EASE_OUT:
			return 1.0 + Math::tan(p_ease_norm * Math::PI / 2.0);
		case BTN_EASE_ESS:
			return -1.0 - Math::tan(p_ease_norm * Math::PI / 2.0);
		case BTN_EASE_OUT_IN:
			return -1.0 + p_ease_norm;
		default:
			ERR_FAIL_V_MSG(0.0, "Bug: Invalid p_btn.");
	}
}

void AudioStreamInteractiveTransitionEaseSlider::_btn_pressed(BaseButton *btn) {
	if (btn == btn_linear) {
		exp_slider->set_min(0.0);
		exp_slider->set_max(0.0);
		exp_slider->set_read_only(true);
	} else {
		exp_slider->set_min(EASE_EXP_MIN);
		exp_slider->set_max(EASE_EXP_MAX);
		exp_slider->set_read_only(false);
	}
	emit_signal(SNAME("value_changed"));
}

void AudioStreamInteractiveTransitionEaseSlider::_slider_changed() {
	emit_signal(SNAME("value_changed"));
}

float AudioStreamInteractiveTransitionEaseSlider::get_value() const {
	ERR_FAIL_NULL_V(btn_group->get_pressed_button(), 0.0);
	return _denormalize_ease(exp_slider->get_value(), btn_group->get_pressed_button()->get_index());
}

void AudioStreamInteractiveTransitionEaseSlider::set_value(float p_value) {
	int btn = _ease_to_btn(p_value);
	_get_btn(btn)->set_pressed(true);
	float val = _normalize_ease(p_value, btn);
	exp_slider->set_value(val);
}

void AudioStreamInteractiveTransitionEaseSlider::set_read_only(bool p_enable) {
	btn_linear->set_disabled(p_enable);
	btn_ease_in->set_disabled(p_enable);
	btn_ease_out->set_disabled(p_enable);
	btn_ease_ess->set_disabled(p_enable);
	btn_ease_out_in->set_disabled(p_enable);
	exp_slider->set_read_only(p_enable);
}

void AudioStreamInteractiveTransitionEaseSlider::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY:
		case NOTIFICATION_THEME_CHANGED: {
			btn_linear->set_button_icon(get_editor_theme_icon(SNAME("AudioStreamInteractiveEaseLinear")));
			btn_ease_in->set_button_icon(get_editor_theme_icon(SNAME("AudioStreamInteractiveEaseIn")));
			btn_ease_out->set_button_icon(get_editor_theme_icon(SNAME("AudioStreamInteractiveEaseOut")));
			btn_ease_ess->set_button_icon(get_editor_theme_icon(SNAME("AudioStreamInteractiveEaseEss")));
			btn_ease_out_in->set_button_icon(get_editor_theme_icon(SNAME("AudioStreamInteractiveEaseOutIn")));
		} break;
	}
}

void AudioStreamInteractiveTransitionEaseSlider::_bind_methods() {
	ADD_SIGNAL(MethodInfo("value_changed"));
}

AudioStreamInteractiveTransitionEaseSlider::AudioStreamInteractiveTransitionEaseSlider() {
	HBoxContainer *btn_hbox = memnew(HBoxContainer);
	add_child(btn_hbox);

	btn_group = memnew(ButtonGroup);
	btn_group->connect(SNAME("pressed"), callable_mp(this, &AudioStreamInteractiveTransitionEaseSlider::_btn_pressed));

	btn_linear = memnew(Button);
	btn_hbox->add_child(btn_linear);
	btn_linear->set_accessibility_name("Linear");
	btn_linear->set_toggle_mode(true);
	btn_linear->set_pressed(true);
	btn_linear->set_button_group(btn_group);

	btn_ease_in = memnew(Button);
	btn_hbox->add_child(btn_ease_in);
	btn_ease_in->set_accessibility_name("Ease In");
	btn_ease_in->set_toggle_mode(true);
	btn_ease_in->set_button_group(btn_group);

	btn_ease_out = memnew(Button);
	btn_hbox->add_child(btn_ease_out);
	btn_ease_out->set_accessibility_name("Ease Out");
	btn_ease_out->set_toggle_mode(true);
	btn_ease_out->set_button_group(btn_group);

	btn_ease_ess = memnew(Button);
	btn_hbox->add_child(btn_ease_ess);
	btn_ease_ess->set_accessibility_name("S-Curve");
	btn_ease_ess->set_toggle_mode(true);
	btn_ease_ess->set_button_group(btn_group);

	btn_ease_out_in = memnew(Button);
	btn_hbox->add_child(btn_ease_out_in);
	btn_ease_out_in->set_accessibility_name("Ease Out-In");
	btn_ease_out_in->set_toggle_mode(true);
	btn_ease_out_in->set_button_group(btn_group);

	exp_slider = memnew(EditorSpinSlider);
	add_child(exp_slider);
	exp_slider->set_min(0.0);
	exp_slider->set_max(0.0);
	exp_slider->set_step(EASE_EXP_STEP);
	exp_slider->connect(SceneStringName(value_changed), callable_mp(this, &AudioStreamInteractiveTransitionEaseSlider::_slider_changed).unbind(1));
	exp_slider->set_accessibility_name(TTRC("Fade Ease Exp:"));
}

////////////////////////

void AudioStreamInteractiveTransitionEditor::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY:
		case NOTIFICATION_THEME_CHANGED: {
			fade_mode->clear();
			fade_mode->add_icon_item(get_editor_theme_icon(SNAME("FadeDisabled")), TTR("Disabled"), AudioStreamInteractive::FADE_DISABLED);
			fade_mode->add_icon_item(get_editor_theme_icon(SNAME("FadeIn")), TTR("Fade-In"), AudioStreamInteractive::FADE_IN);
			fade_mode->add_icon_item(get_editor_theme_icon(SNAME("FadeOut")), TTR("Fade-Out"), AudioStreamInteractive::FADE_OUT);
			fade_mode->add_icon_item(get_editor_theme_icon(SNAME("FadeCross")), TTR("Cross-Fade"), AudioStreamInteractive::FADE_CROSS);
			fade_mode->add_icon_item(get_editor_theme_icon(SNAME("AutoPlay")), TTR("Automatic"), AudioStreamInteractive::FADE_AUTOMATIC);

			preview_play->set_button_icon(get_editor_theme_icon(SNAME("Play")));
			preview_stop->set_button_icon(get_editor_theme_icon(SNAME("Stop")));
		} break;

		case NOTIFICATION_VISIBILITY_CHANGED: {
			if (!is_visible()) {
				_preview_stop();
			}
		} break;

		case NOTIFICATION_PROCESS: {
			Ref<AudioStreamPlaybackInteractive> playback = audio_stream_player->get_stream_playback();
			if (playback.is_null()) {
				_preview_stop();
				return;
			}

			double start_t = preview_start_position->get_value();
			double preview_cue_t = start_t + preview_cue_delay->get_value();

			double from_t = start_t + playback->get_playback_position();

			if (preview_cue_pending && from_t >= preview_cue_t) {
				playback->switch_to_clip(editing_transition.y);
				preview_cue_pending = false;
			}

			double playhead_t = from_t + AudioServer::get_singleton()->get_time_since_last_mix() - AudioServer::get_singleton()->get_output_latency();
			chart->set_preview_state(preview_cue_t, playhead_t);
		} break;
	}
}

void AudioStreamInteractiveTransitionEditor::_bind_methods() {
	ClassDB::bind_method("_update_transitions", &AudioStreamInteractiveTransitionEditor::_update_transitions);
}

void AudioStreamInteractiveTransitionEditor::_edited() {
	if (updating) {
		return;
	}

	bool enabled = transition_enabled->is_pressed();
	AudioStreamInteractive::TransitionFromTime from = AudioStreamInteractive::TransitionFromTime(transition_from->get_selected());
	AudioStreamInteractive::TransitionToTime to = AudioStreamInteractive::TransitionToTime(transition_to->get_selected());
	AudioStreamInteractive::FadeMode fade = AudioStreamInteractive::FadeMode(fade_mode->get_selected());
	AudioStreamInteractive::TransitionToCueTiming timing = AudioStreamInteractive::TransitionToCueTiming(transition_timing->get_selected());
	float beats = fade_beats->get_value();
	float offset_beats = fade_offset_beats->get_value();
	float ease_exp = fade_ease_exp->get_value();
	float to_beats = to_fade_beats->get_value();
	float to_offset_beats = to_fade_offset_beats->get_value();
	float to_ease_exp = to_fade_ease_exp->get_value();
	bool use_filler = filler_clip->get_selected() > 0;
	float filler_delay_beats = filler_clip_offset_beats->get_value();
	int filler = use_filler ? filler_clip->get_selected() - 1 : 0;
	bool hold = hold_previous->is_pressed();

	EditorUndoRedoManager::get_singleton()->create_action(TTR("Edit Transitions"));
	for (int i = 0; i < selected.size(); i++) {
		if (!enabled) {
			if (audio_stream_interactive->has_transition(selected[i].x, selected[i].y)) {
				EditorUndoRedoManager::get_singleton()->add_do_method(audio_stream_interactive, "erase_transition", selected[i].x, selected[i].y);
			}
		} else {
			EditorUndoRedoManager::get_singleton()->add_do_method(audio_stream_interactive, "add_transition", selected[i].x, selected[i].y, from, to, fade, beats, use_filler, filler, hold, timing, offset_beats, ease_exp, to_beats, to_offset_beats, to_ease_exp, filler_delay_beats);
		}
	}
	EditorUndoRedoManager::get_singleton()->add_undo_property(audio_stream_interactive, "_transitions", audio_stream_interactive->get("_transitions"));
	EditorUndoRedoManager::get_singleton()->add_do_method(this, "_update_transitions");
	EditorUndoRedoManager::get_singleton()->add_undo_method(this, "_update_transitions");
	EditorUndoRedoManager::get_singleton()->commit_action();

	_update_chart();
}

void AudioStreamInteractiveTransitionEditor::_update_chart() {
	if (!audio_stream_interactive || selected.is_empty() || !transition_enabled->is_pressed()) {
		chart->clear_transition();
	} else {
		chart->set_transition(audio_stream_interactive, editing_transition.x, editing_transition.y);
	}

	_update_preview_controls();

	double preview_cue_t = preview_start_position->get_value() + preview_cue_delay->get_value();
	chart->set_preview_state(preview_cue_t, -1.0);
}

void AudioStreamInteractiveTransitionEditor::_update_preview_controls() {
	bool can_preview = audio_stream_interactive && !selected.is_empty() && editing_transition.x != AudioStreamInteractive::CLIP_ANY && editing_transition.y != AudioStreamInteractive::CLIP_ANY;
	bool playing = audio_stream_player->is_playing();

	preview_start_position->set_editable(can_preview && !playing);
	preview_cue_delay->set_editable(can_preview && !playing);
	preview_play->set_disabled(!can_preview || playing);
	preview_stop->set_disabled(!can_preview || !playing);

	if (can_preview) {
		double from_clip_length = audio_stream_interactive->get_clip_stream(editing_transition.x)->get_length();
		preview_start_position->set_max(from_clip_length);
		preview_cue_delay->set_max(MAX(from_clip_length - preview_start_position->get_value(), 0.0));
	}
}

void AudioStreamInteractiveTransitionEditor::_preview_start() {
	_preview_stop();

	ERR_FAIL_COND(editing_transition.x == AudioStreamInteractive::CLIP_ANY || editing_transition.y == AudioStreamInteractive::CLIP_ANY);
	ERR_FAIL_NULL(audio_stream_player);
	ERR_FAIL_NULL(audio_stream_interactive);
	ERR_FAIL_NULL(audio_stream_interactive->get_clip_stream(editing_transition.x));
	ERR_FAIL_NULL(audio_stream_interactive->get_clip_stream(editing_transition.y));

	audio_stream_player->set_stream(audio_stream_interactive);

	double start_t = preview_start_position->get_value();
	double cue_t = preview_cue_delay->get_value() + start_t;

	// The server starts the playback at the initial clip; hold the lock so it can be
	// redirected to the "from" clip before any of that gets mixed.
	AudioServer::get_singleton()->lock();
	audio_stream_player->play();
	Ref<AudioStreamPlaybackInteractive> playback = audio_stream_player->get_stream_playback();
	if (playback.is_valid()) {
		playback->start_clip(editing_transition.x, start_t);
	}
	AudioServer::get_singleton()->unlock();

	if (playback.is_null()) {
		audio_stream_player->stop();
		return;
	}

	preview_cue_pending = true;

	chart->set_preview_state(cue_t, start_t);

	set_process(true);
	_update_preview_controls();
}

void AudioStreamInteractiveTransitionEditor::_preview_stop() {
	ERR_FAIL_NULL(audio_stream_player);
	audio_stream_player->stop();
	audio_stream_player->set_stream(nullptr);
	set_process(false);
	_update_preview_controls();
	double preview_cue_t = preview_start_position->get_value() + preview_start_position->get_value();
	chart->set_preview_state(preview_cue_t, -1.0);
}

void AudioStreamInteractiveTransitionEditor::_update_selection() {
	updating_selection = false;
	int clip_count = audio_stream_interactive->get_clip_count();
	selected.clear();
	Vector2i editing;
	int editing_order = -1;
	for (int i = 0; i <= clip_count; i++) {
		for (int j = 0; j <= clip_count; j++) {
			if (rows[i]->is_selected(j)) {
				Vector2i meta = rows[i]->get_metadata(j);
				if (selection_order.has(meta)) {
					int order = selection_order[meta];
					if (order > editing_order) {
						editing = meta;
					}
				}
				selected.push_back(meta);
			}
		}
	}

	transition_enabled->set_disabled(selected.is_empty());
	transition_from->set_disabled(selected.is_empty());
	transition_to->set_disabled(selected.is_empty());
	fade_mode->set_disabled(selected.is_empty());
	transition_timing->set_disabled(selected.is_empty());
	fade_beats->set_editable(!selected.is_empty());
	fade_offset_beats->set_editable(!selected.is_empty());
	fade_ease_exp->set_read_only(selected.is_empty());
	to_fade_beats->set_editable(!selected.is_empty());
	to_fade_offset_beats->set_editable(!selected.is_empty());
	to_fade_ease_exp->set_read_only(selected.is_empty());
	filler_clip->set_disabled(selected.is_empty());
	filler_clip_offset_beats->set_editable(!selected.is_empty());
	hold_previous->set_disabled(selected.is_empty());

	_preview_stop();

	if (selected.is_empty()) {
		_update_chart();
		return;
	}
	editing_transition = editing;

	updating = true;
	if (!audio_stream_interactive->has_transition(editing.x, editing.y)) {
		transition_enabled->set_pressed(false);
		transition_from->select(0);
		transition_to->select(0);
		fade_mode->select(AudioStreamInteractive::FADE_AUTOMATIC);
		transition_timing->select(AudioStreamInteractive::TRANSITION_TO_CUE_TIMING_DEFAULT);
		fade_beats->set_value(1.0);
		fade_offset_beats->set_value(0.0);
		fade_ease_exp->set_value(0.0);
		to_fade_beats->set_value(0.0);
		to_fade_offset_beats->set_value(0.0);
		to_fade_ease_exp->set_value(0.0);
		filler_clip->select(0);
		filler_clip_offset_beats->set_value(0.0);
		hold_previous->set_pressed(false);
	} else {
		transition_enabled->set_pressed(true);
		transition_from->select(audio_stream_interactive->get_transition_from_time(editing.x, editing.y));
		transition_to->select(audio_stream_interactive->get_transition_to_time(editing.x, editing.y));
		fade_mode->select(audio_stream_interactive->get_transition_fade_mode(editing.x, editing.y));
		transition_timing->select(audio_stream_interactive->get_transition_to_cue_timing(editing.x, editing.y));
		fade_beats->set_value(audio_stream_interactive->get_transition_fade_beats(editing.x, editing.y));
		fade_offset_beats->set_value(audio_stream_interactive->get_transition_fade_offset_beats(editing.x, editing.y));
		fade_ease_exp->set_read_only(false);
		fade_ease_exp->set_value(audio_stream_interactive->get_transition_fade_ease_exp(editing.x, editing.y));
		to_fade_beats->set_value(audio_stream_interactive->get_transition_to_fade_beats(editing.x, editing.y));
		to_fade_offset_beats->set_value(audio_stream_interactive->get_transition_to_fade_offset_beats(editing.x, editing.y));
		to_fade_ease_exp->set_read_only(false);
		to_fade_ease_exp->set_value(audio_stream_interactive->get_transition_to_fade_ease_exp(editing.x, editing.y));
		if (audio_stream_interactive->is_transition_using_filler_clip(editing.x, editing.y)) {
			filler_clip->select(audio_stream_interactive->get_transition_filler_clip(editing.x, editing.y) + 1);
			filler_clip_offset_beats->set_value(audio_stream_interactive->get_transition_filler_clip_offset_beats(editing.x, editing.y));
		} else {
			filler_clip->select(0);
			filler_clip_offset_beats->set_value(0.0);
		}
		hold_previous->set_pressed(audio_stream_interactive->is_transition_holding_previous(editing.x, editing.y));
	}
	updating = false;
	_update_chart();
}

void AudioStreamInteractiveTransitionEditor::_cell_selected(TreeItem *p_item, int p_column, bool p_selected) {
	int to = p_item->get_meta("to");
	int from = p_column == audio_stream_interactive->get_clip_count() ? AudioStreamInteractive::CLIP_ANY : p_column;
	if (p_selected) {
		selection_order[Vector2i(from, to)] = order_counter++;
	}

	if (!updating_selection) {
		MessageQueue::get_singleton()->push_callable(callable_mp(this, &AudioStreamInteractiveTransitionEditor::_update_selection));
		updating_selection = true;
	}
}

void AudioStreamInteractiveTransitionEditor::_update_transitions() {
	if (!is_visible()) {
		return;
	}
	int clip_count = audio_stream_interactive->get_clip_count();
	Color font_color = tree->get_theme_color(SceneStringName(font_color), "Tree");
	Color font_color_default = font_color;
	font_color_default.a *= 0.5;
	Ref<Texture> fade_icons[5] = {
		get_editor_theme_icon(SNAME("FadeDisabled")),
		get_editor_theme_icon(SNAME("FadeIn")),
		get_editor_theme_icon(SNAME("FadeOut")),
		get_editor_theme_icon(SNAME("FadeCross")),
		get_editor_theme_icon(SNAME("AutoPlay"))
	};
	for (int i = 0; i <= clip_count; i++) {
		for (int j = 0; j <= clip_count; j++) {
			int from = i == clip_count ? AudioStreamInteractive::CLIP_ANY : i;
			int to = j == clip_count ? AudioStreamInteractive::CLIP_ANY : j;

			bool exists = audio_stream_interactive->has_transition(from, to);
			String tooltip;
			Ref<Texture> icon;
			if (!exists) {
				if (audio_stream_interactive->has_transition(AudioStreamInteractive::CLIP_ANY, to)) {
					from = AudioStreamInteractive::CLIP_ANY;
					tooltip = vformat(TTR(U"Using any clip → %s."), audio_stream_interactive->get_clip_name(to));
				} else if (audio_stream_interactive->has_transition(from, AudioStreamInteractive::CLIP_ANY)) {
					to = AudioStreamInteractive::CLIP_ANY;
					tooltip = vformat(TTR(U"Using %s → Any clip."), audio_stream_interactive->get_clip_name(from));
				} else if (audio_stream_interactive->has_transition(AudioStreamInteractive::CLIP_ANY, AudioStreamInteractive::CLIP_ANY)) {
					from = to = AudioStreamInteractive::CLIP_ANY;
					tooltip = TTR(U"Using all clips → Any clip.");
				} else {
					tooltip = TTR("No transition available.");
				}
			}

			String from_time;
			String to_time;
			if (audio_stream_interactive->has_transition(from, to)) {
				icon = fade_icons[audio_stream_interactive->get_transition_fade_mode(from, to)];
				switch (audio_stream_interactive->get_transition_from_time(from, to)) {
					case AudioStreamInteractive::TRANSITION_FROM_TIME_IMMEDIATE: {
						from_time = TTR("Immediate");
					} break;
					case AudioStreamInteractive::TRANSITION_FROM_TIME_NEXT_BEAT: {
						from_time = TTR("Next Beat");
					} break;
					case AudioStreamInteractive::TRANSITION_FROM_TIME_NEXT_BAR: {
						from_time = TTR("Next Bar");
					} break;
					case AudioStreamInteractive::TRANSITION_FROM_TIME_END: {
						from_time = TTR("Clip End");
					} break;
					default: {
					}
				}

				switch (audio_stream_interactive->get_transition_to_time(from, to)) {
					case AudioStreamInteractive::TRANSITION_TO_TIME_SAME_POSITION: {
						to_time = TTR("Same", "Transition Time Position");
					} break;
					case AudioStreamInteractive::TRANSITION_TO_TIME_START: {
						to_time = TTR("Start", "Transition Time Position");
					} break;
					case AudioStreamInteractive::TRANSITION_TO_TIME_PREVIOUS_POSITION: {
						to_time = TTR("Prev", "Transition Time Position");
					} break;
					default: {
					}
				}
			}

			rows[j]->set_icon(i, icon);
			rows[j]->set_text(i, to_time.is_empty() ? from_time : vformat(U"%s ⮕ %s", from_time, to_time));
			rows[j]->set_tooltip_text(i, tooltip);
			if (exists) {
				rows[j]->set_custom_color(i, font_color);
				rows[j]->set_icon_modulate(i, Color(1, 1, 1, 1));
			} else {
				rows[j]->set_custom_color(i, font_color_default);
				rows[j]->set_icon_modulate(i, Color(1, 1, 1, 0.5));
			}
		}
	}
}

void AudioStreamInteractiveTransitionEditor::edit(Object *p_obj) {
	audio_stream_interactive = Object::cast_to<AudioStreamInteractive>(p_obj);
	if (!audio_stream_interactive) {
		return;
	}

	Ref<Font> header_font = get_theme_font("bold", EditorStringName(EditorFonts));
	int header_font_size = get_theme_font_size("bold_size", EditorStringName(EditorFonts));

	tree->clear();
	rows.clear();
	selection_order.clear();
	selected.clear();

	int clip_count = audio_stream_interactive->get_clip_count();
	tree->set_columns(clip_count + 2);
	TreeItem *root = tree->create_item();
	TreeItem *header = tree->create_item(root); // Header
	int header_index = clip_count + 1;
	header->set_text(header_index, TTR("From / To"));
	header->set_selectable(header_index, false);

	filler_clip->clear();
	filler_clip->add_item(TTR("Disabled"), -1);

	const Ref<StyleBoxFlat> &sb_title = tree->get_theme_stylebox(SNAME("title_button_normal"), SNAME("Tree"));
	const Color header_color = sb_title.is_valid() ? sb_title->get_bg_color() : Color(0, 0, 0, 0);

	int max_w = 0;

	updating = true;
	for (int i = 0; i <= clip_count; i++) {
		int cell_index = i;
		int clip_i = i == clip_count ? AudioStreamInteractive::CLIP_ANY : i;
		header->set_selectable(cell_index, false);
		header->set_custom_font(cell_index, header_font);
		header->set_custom_font_size(cell_index, header_font_size);
		header->set_custom_bg_color(cell_index, header_color);

		String name;
		if (i == clip_count) {
			name = TTR("Any Clip");
		} else {
			name = audio_stream_interactive->get_clip_name(i);
		}

		int min_w = header_font->get_string_size(name + "XX").width;
		tree->set_column_custom_minimum_width(cell_index, min_w);
		max_w = MAX(max_w, min_w);

		header->set_text(cell_index, name);

		TreeItem *row = tree->create_item(root);
		row->set_text(header_index, name);
		row->set_selectable(header_index, false);
		row->set_custom_font(header_index, header_font);
		row->set_custom_font_size(header_index, header_font_size);
		row->set_custom_bg_color(header_index, header_color);
		row->set_meta("to", clip_i);
		for (int j = 0; j <= clip_count; j++) {
			int clip_j = j == clip_count ? AudioStreamInteractive::CLIP_ANY : j;
			row->set_metadata(j, Vector2i(clip_j, clip_i));
		}
		rows.push_back(row);

		if (i < clip_count) {
			filler_clip->add_item(name, i);
		}
	}

	tree->set_column_custom_minimum_width(header_index, max_w);
	selection_order.clear();
	_update_selection();
	popup_centered_clamped(Size2(900, 450) * EDSCALE);
	updating = false;
	_update_transitions();
}

AudioStreamInteractiveTransitionEditor::AudioStreamInteractiveTransitionEditor() {
	set_title(TTR("AudioStreamInteractive Transition Editor"));
	VSplitContainer *vsplit = memnew(VSplitContainer);
	add_child(vsplit);
	split = memnew(HSplitContainer);
	split->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	vsplit->add_child(split);

	tree = memnew(Tree);
	tree->set_auto_translate_mode(AUTO_TRANSLATE_MODE_DISABLED);
	tree->set_hide_root(true);
	tree->set_hide_folding(true);
	tree->add_theme_constant_override("draw_guides", 1);
	tree->set_select_mode(Tree::SELECT_MULTI);
	tree->set_custom_minimum_size(Size2(400, 0) * EDSCALE);
	tree->set_theme_type_variation("TreeSecondary");
	split->add_child(tree);

	tree->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	tree->connect("multi_selected", callable_mp(this, &AudioStreamInteractiveTransitionEditor::_cell_selected));

	ScrollContainer *edit_scroll = memnew(ScrollContainer);
	edit_scroll->set_custom_minimum_size(Size2(0.0, 100.0) * EDSCALE);
	edit_scroll->set_horizontal_scroll_mode(ScrollContainer::SCROLL_MODE_DISABLED);
	split->add_child(edit_scroll);

	VBoxContainer *edit_vb = memnew(VBoxContainer);
	edit_vb->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	edit_scroll->add_child(edit_vb);

	transition_enabled = memnew(CheckBox);
	transition_enabled->set_text(TTR("Enabled"));
	transition_enabled->set_accessibility_name(TTRC("Use Transition:"));
	edit_vb->add_margin_child(TTR("Use Transition:"), transition_enabled);
	transition_enabled->connect(SceneStringName(pressed), callable_mp(this, &AudioStreamInteractiveTransitionEditor::_edited));

	transition_from = memnew(OptionButton);
	edit_vb->add_margin_child(TTR("Transition From:"), transition_from);
	transition_from->add_item(TTR("Immediate"), AudioStreamInteractive::TRANSITION_FROM_TIME_IMMEDIATE);
	transition_from->add_item(TTR("Next Beat"), AudioStreamInteractive::TRANSITION_FROM_TIME_NEXT_BEAT);
	transition_from->add_item(TTR("Next Bar"), AudioStreamInteractive::TRANSITION_FROM_TIME_NEXT_BAR);
	transition_from->add_item(TTR("Clip End"), AudioStreamInteractive::TRANSITION_FROM_TIME_END);
	transition_from->set_accessibility_name(TTRC("Transition From:"));

	transition_from->connect(SceneStringName(item_selected), callable_mp(this, &AudioStreamInteractiveTransitionEditor::_edited).unbind(1));

	transition_to = memnew(OptionButton);
	edit_vb->add_margin_child(TTR("Transition To:"), transition_to);
	transition_to->add_item(TTR("Same Position"), AudioStreamInteractive::TRANSITION_TO_TIME_SAME_POSITION);
	transition_to->add_item(TTR("Clip Start"), AudioStreamInteractive::TRANSITION_TO_TIME_START);
	transition_to->add_item(TTR("Prev Position"), AudioStreamInteractive::TRANSITION_TO_TIME_PREVIOUS_POSITION);
	transition_to->set_accessibility_name(TTRC("Transition To:"));
	transition_to->connect(SceneStringName(item_selected), callable_mp(this, &AudioStreamInteractiveTransitionEditor::_edited).unbind(1));

	fade_mode = memnew(OptionButton);
	edit_vb->add_margin_child(TTR("Fade Mode:"), fade_mode);
	fade_mode->connect(SceneStringName(item_selected), callable_mp(this, &AudioStreamInteractiveTransitionEditor::_edited).unbind(1));
	fade_mode->set_accessibility_name(TTRC("Fade Mode:"));

	transition_timing = memnew(OptionButton);
	edit_vb->add_margin_child(TTR("Transition Timing:"), transition_timing);
	transition_timing->connect(SceneStringName(item_selected), callable_mp(this, &AudioStreamInteractiveTransitionEditor::_edited).unbind(1));
	transition_timing->set_accessibility_name(TTRC("Transition Timing:"));
	transition_timing->add_item("Default");
	transition_timing->add_item("After Filler");
	transition_timing->add_item("After Fade Out");
	transition_timing->add_item("Immediate");

	fade_beats = memnew(SpinBox);
	edit_vb->add_margin_child(TTR("Fade Beats:"), fade_beats);
	fade_beats->set_max(16);
	fade_beats->set_step(0.1);
	fade_beats->connect(SceneStringName(value_changed), callable_mp(this, &AudioStreamInteractiveTransitionEditor::_edited).unbind(1));
	fade_beats->set_accessibility_name(TTRC("Fade Beats:"));

	fade_offset_beats = memnew(SpinBox);
	edit_vb->add_margin_child(TTR("Fade Offset Beats:"), fade_offset_beats);
	fade_offset_beats->set_min(-16);
	fade_offset_beats->set_max(16);
	fade_offset_beats->set_step(0.1);
	fade_offset_beats->connect(SceneStringName(value_changed), callable_mp(this, &AudioStreamInteractiveTransitionEditor::_edited).unbind(1));
	fade_offset_beats->set_accessibility_name(TTRC("Fade Offset Beats:"));

	fade_ease_exp = memnew(AudioStreamInteractiveTransitionEaseSlider);
	edit_vb->add_margin_child(TTR("Fade Ease Exp:"), fade_ease_exp);
	fade_ease_exp->connect(SNAME("value_changed"), callable_mp(this, &AudioStreamInteractiveTransitionEditor::_edited));

	to_fade_beats = memnew(SpinBox);
	edit_vb->add_margin_child(TTR("To Fade Beats:"), to_fade_beats);
	to_fade_beats->set_max(16);
	to_fade_beats->set_step(0.1);
	to_fade_beats->connect(SceneStringName(value_changed), callable_mp(this, &AudioStreamInteractiveTransitionEditor::_edited).unbind(1));
	to_fade_beats->set_accessibility_name(TTRC("To Fade Beats:"));

	to_fade_offset_beats = memnew(SpinBox);
	edit_vb->add_margin_child(TTR("To Fade Offset Beats:"), to_fade_offset_beats);
	to_fade_offset_beats->set_min(-16);
	to_fade_offset_beats->set_max(16);
	to_fade_offset_beats->set_step(0.1);
	to_fade_offset_beats->connect(SceneStringName(value_changed), callable_mp(this, &AudioStreamInteractiveTransitionEditor::_edited).unbind(1));
	to_fade_offset_beats->set_accessibility_name(TTRC("To Fade Offset Beats:"));

	to_fade_ease_exp = memnew(AudioStreamInteractiveTransitionEaseSlider);
	edit_vb->add_margin_child(TTR("To Fade Ease Exp:"), to_fade_ease_exp);
	to_fade_ease_exp->connect(SNAME("value_changed"), callable_mp(this, &AudioStreamInteractiveTransitionEditor::_edited));

	filler_clip = memnew(OptionButton);
	edit_vb->add_margin_child(TTR("Filler Clip:"), filler_clip);
	filler_clip->set_auto_translate_mode(AUTO_TRANSLATE_MODE_DISABLED);
	filler_clip->connect(SceneStringName(item_selected), callable_mp(this, &AudioStreamInteractiveTransitionEditor::_edited).unbind(1));
	filler_clip->set_accessibility_name(TTRC("Filler Clip:"));

	filler_clip_offset_beats = memnew(SpinBox);
	edit_vb->add_margin_child(TTR("Filler Clip Offset Beats:"), filler_clip_offset_beats);
	filler_clip_offset_beats->set_min(-16);
	filler_clip_offset_beats->set_max(16);
	filler_clip_offset_beats->set_step(0.1);
	filler_clip_offset_beats->connect(SceneStringName(value_changed), callable_mp(this, &AudioStreamInteractiveTransitionEditor::_edited).unbind(1));
	filler_clip_offset_beats->set_accessibility_name(TTRC("Filler Clip Offset Beats:"));

	hold_previous = memnew(CheckBox);
	hold_previous->set_text(TTR("Enabled"));
	hold_previous->set_accessibility_name(TTRC("Hold Previous:"));
	hold_previous->connect(SceneStringName(pressed), callable_mp(this, &AudioStreamInteractiveTransitionEditor::_edited));
	edit_vb->add_margin_child(TTR("Hold Previous:"), hold_previous);

	VBoxContainer *preview_vb = memnew(VBoxContainer);
	vsplit->add_child(preview_vb);

	HBoxContainer *preview_hb = memnew(HBoxContainer);
	preview_vb->add_child(preview_hb);

	audio_stream_player = memnew(AudioStreamPlayer);
	add_child(audio_stream_player);

	preview_hb->add_child(memnew(Label(TTR("Preview Start Position:"))));
	preview_start_position = memnew(SpinBox);
	preview_start_position->set_max(3600);
	preview_start_position->set_step(0.01);
	preview_start_position->set_suffix("s");
	preview_start_position->set_tooltip_text(TTR("Position in the source clip to start the preview from."));
	preview_start_position->set_accessibility_name(TTRC("Preview Start Position:"));
	preview_start_position->connect(SceneStringName(value_changed), callable_mp(this, &AudioStreamInteractiveTransitionEditor::_update_chart).unbind(1));
	preview_hb->add_child(preview_start_position);

	preview_hb->add_child(memnew(Label(TTR("Cue Delay:"))));
	preview_cue_delay = memnew(SpinBox);
	preview_cue_delay->set_max(3600);
	preview_cue_delay->set_step(0.01);
	preview_cue_delay->set_suffix("s");
	preview_cue_delay->set_tooltip_text(TTR("Time after the start position at which the transition is requested."));
	preview_cue_delay->set_accessibility_name(TTRC("Cue Delay:"));
	preview_cue_delay->connect(SceneStringName(value_changed), callable_mp(this, &AudioStreamInteractiveTransitionEditor::_update_chart).unbind(1));
	preview_hb->add_child(preview_cue_delay);

	preview_play = memnew(Button);
	preview_play->set_text(TTR("Play"));
	preview_play->set_tooltip_text(TTR("Play the source clip from the start position and request the transition after the cue delay."));
	preview_play->connect(SceneStringName(pressed), callable_mp(this, &AudioStreamInteractiveTransitionEditor::_preview_start));
	preview_hb->add_child(preview_play);

	preview_stop = memnew(Button);
	preview_stop->set_text(TTR("Stop"));
	preview_stop->connect(SceneStringName(pressed), callable_mp(this, &AudioStreamInteractiveTransitionEditor::_preview_stop));
	preview_hb->add_child(preview_stop);

	chart = memnew(AudioStreamInteractiveTransitionChart);
	chart->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	preview_vb->add_child(chart);

	set_exclusive(true);
}

////////////////////////

bool EditorInspectorPluginAudioStreamInteractive::can_handle(Object *p_object) {
	return Object::cast_to<AudioStreamInteractive>(p_object);
}

void EditorInspectorPluginAudioStreamInteractive::_edit(Object *p_object) {
	audio_stream_interactive_transition_editor->edit(p_object);
}

void EditorInspectorPluginAudioStreamInteractive::parse_end(Object *p_object) {
	if (Object::cast_to<AudioStreamInteractive>(p_object)) {
		Button *button = memnew(EditorInspectorActionButton(TTRC("Edit Transitions"), SNAME("Blend")));
		button->connect(SceneStringName(pressed), callable_mp(this, &EditorInspectorPluginAudioStreamInteractive::_edit).bind(p_object));
		add_custom_control(button);
	}
}

EditorInspectorPluginAudioStreamInteractive::EditorInspectorPluginAudioStreamInteractive() {
	audio_stream_interactive_transition_editor = memnew(AudioStreamInteractiveTransitionEditor);
	EditorNode::get_singleton()->get_gui_base()->add_child(audio_stream_interactive_transition_editor);
}

AudioStreamInteractiveEditorPlugin::AudioStreamInteractiveEditorPlugin() {
	Ref<EditorInspectorPluginAudioStreamInteractive> inspector_plugin;
	inspector_plugin.instantiate();
	add_inspector_plugin(inspector_plugin);
}
