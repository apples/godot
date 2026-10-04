/**************************************************************************/
/*  audio_stream_interactive.cpp                                          */
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

#include "audio_stream_interactive.h"

#include "core/config/engine.h"
#include "core/error/error_macros.h"
#include "core/math/math_funcs.h"
#include "core/object/class_db.h"
#include "core/string/print_string.h"
#include "core/variant/variant.h"
#include "servers/audio/audio_driver.h"
#include "servers/audio/audio_server.h"

AudioStreamInteractive::TransitionMixResult AudioStreamInteractive::compute_mix(const Transition &transition, float current_pos, const Ref<AudioStream> &from_stream, const Ref<AudioStream> &to_stream, const Ref<AudioStream> &filler_stream, bool p_is_auto_advance) {
	TransitionMixResult result;

	FadeMode fade_mode = transition.fade_mode;

	if (fade_mode == AudioStreamInteractive::FADE_AUTOMATIC) {
		// Adjust automatic mode based on context.
		if (transition.to_time == AudioStreamInteractive::TRANSITION_TO_TIME_START) {
			fade_mode = AudioStreamInteractive::FADE_OUT;
		} else {
			fade_mode = AudioStreamInteractive::FADE_CROSS;
		}
	}

	TransitionFromTime from_time = p_is_auto_advance ? TRANSITION_FROM_TIME_END : transition.from_time;

	// Prepare the fadeout

	double transition_start_t = 0;
	double fade_speed = 0;
	bool src_no_loop = false;

	// Check if source stream has BPM, if so, transition syncs to BPM.
	double beat_sec = from_stream.is_valid() && from_stream->get_bpm() ? 60.0 / from_stream->get_bpm() : 1.0;
	double to_beat_sec = to_stream.is_valid() && to_stream->get_bpm() ? 60.0 / to_stream->get_bpm() : 1.0;

	result.from_beat_sec = beat_sec;
	result.to_beat_sec = to_beat_sec;

	if (from_stream.is_valid() && from_stream->get_bpm()) {
		switch (from_time) {
			case AudioStreamInteractive::TRANSITION_FROM_TIME_IMMEDIATE: {
				transition_start_t = 0;
			} break;
			case AudioStreamInteractive::TRANSITION_FROM_TIME_NEXT_BEAT: {
				double remainder = Math::fmod(double(current_pos), beat_sec);
				transition_start_t = beat_sec - remainder;
			} break;
			case AudioStreamInteractive::TRANSITION_FROM_TIME_NEXT_BAR: {
				if (from_stream->get_bar_beats() > 0) {
					double bar_sec = beat_sec * from_stream->get_bar_beats();
					double remainder = Math::fmod(double(current_pos), bar_sec);
					transition_start_t = bar_sec - remainder;
				} else {
					// Stream does not have a number of beats per bar - avoid NaN, and play immediately.
					transition_start_t = 0;
				}
			} break;
			case AudioStreamInteractive::TRANSITION_FROM_TIME_END: {
				double end = from_stream->get_beat_count() > 0 ? from_stream->get_beat_count() * beat_sec : from_stream->get_length();
				if (end == 0.0) {
					// Stream does not have a length.
					transition_start_t = 0;
				} else {
					transition_start_t = end - current_pos;
				}

				if (!from_stream->has_loop()) {
					src_no_loop = true;
				}

			} break;
			default: {
			}
		}
	} else {
		// Source has no BPM, so just simple transition.
		if (from_time == AudioStreamInteractive::TRANSITION_FROM_TIME_END && from_stream.is_valid() && from_stream->get_length() > 0) {
			double end = from_stream->get_length();
			transition_start_t = end - current_pos;
			if (!from_stream->has_loop()) {
				src_no_loop = true;
			}
		} else {
			transition_start_t = 0;
		}
	}

	result.start_t = transition_start_t;

	// Fade speed also aligned to BPM.
	fade_speed = 1.0 / (transition.fade_beats * beat_sec);

	if (fade_mode == AudioStreamInteractive::FADE_DISABLED || fade_mode == AudioStreamInteractive::FADE_IN) {
		if (src_no_loop) {
			// If there is no fade in the source stream, then let it continue until it ends.
			result.from_fade_start_t = 0;
			result.from_fade_speed = 0;
			result.from_fade_ease_exp = 0.0;
		} else {
			// Otherwise force a very quick fade to avoid clicks
			result.from_fade_start_t = MAX(transition_start_t + transition.fade_offset_beats * beat_sec, 0.0);
			result.from_fade_speed = 1.0 / -0.001;
			result.from_fade_ease_exp = 0.0;
		}
	} else {
		// Regular fade.
		result.from_fade_start_t = MAX(transition_start_t + transition.fade_offset_beats * beat_sec, 0.0);
		result.from_fade_speed = -fade_speed;
		result.from_fade_ease_exp = transition.fade_ease_exp;
	}

	result.from_end_t = result.from_fade_speed != 0.0 ? result.from_fade_start_t + 1.0 / -result.from_fade_speed : result.from_fade_start_t;

	double filler_length = 0.0;
	TransitionToCueTiming to_cue_timing = transition.to_cue_timing;

	if (filler_stream.is_valid()) {
		result.filler_start_t = MAX(transition_start_t + transition.filler_clip_offset_beats * beat_sec, 0.0);

		if (filler_stream->get_bpm() > 0 && filler_stream->get_beat_count() > 0) {
			double filler_beat_sec = 60.0 / filler_stream->get_bpm();
			filler_length = filler_beat_sec * filler_stream->get_beat_count();
		} else {
			filler_length = filler_stream->get_length();
		}

		result.filler_end_t = result.filler_start_t + filler_length;
		result.filler_tail_end_t = result.filler_start_t + filler_stream->get_length();

		if (to_cue_timing == AudioStreamInteractive::TRANSITION_TO_CUE_TIMING_DEFAULT) {
			to_cue_timing = AudioStreamInteractive::TRANSITION_TO_CUE_TIMING_AFTER_FILLER;
		}
	} else {
		if (to_cue_timing == AudioStreamInteractive::TRANSITION_TO_CUE_TIMING_DEFAULT) {
			to_cue_timing = AudioStreamInteractive::TRANSITION_TO_CUE_TIMING_IMMEDIATE;
		}
	}

	switch (to_cue_timing) {
		case AudioStreamInteractive::TRANSITION_TO_CUE_TIMING_AFTER_FILLER:
			result.to_start_t = MAX(result.filler_end_t + transition.to_fade_offset_beats * to_beat_sec, 0.0);
			break;
		case AudioStreamInteractive::TRANSITION_TO_CUE_TIMING_AFTER_FADE_OUT:
			result.to_start_t = MAX(result.from_end_t + transition.to_fade_offset_beats * to_beat_sec, 0.0);
			break;
		case AudioStreamInteractive::TRANSITION_TO_CUE_TIMING_IMMEDIATE:
			result.to_start_t = MAX(transition_start_t + transition.to_fade_offset_beats * to_beat_sec, 0.0);
			break;
		case AudioStreamInteractive::TRANSITION_TO_CUE_TIMING_DEFAULT:
			ERR_PRINT("Bug: to_cue_timing default not reassigned.");
			result.to_start_t = MAX(transition_start_t + transition.to_fade_offset_beats * to_beat_sec, 0.0);
			break;
	}

	if (fade_mode == AudioStreamInteractive::FADE_DISABLED || fade_mode == AudioStreamInteractive::FADE_OUT) {
		// No fading, immediately start at full volume.
		result.to_fade_speed = 0.0;
		result.to_fade_ease_exp = 0.0;
	} else {
		// Fade enable, prepare fade.
		result.to_fade_speed = transition.to_fade_beats > 0.0 ? 1.0 / (transition.to_fade_beats * to_beat_sec) : fade_speed;
		result.to_fade_ease_exp = transition.to_fade_ease_exp;
	}

	result.to_fade_end_t = result.to_fade_speed != 0.0 ? result.to_start_t + 1.0 / result.to_fade_speed : result.to_start_t;

	return result;
}

AudioStreamInteractive::AudioStreamInteractive() {
}

Ref<AudioStreamPlayback> AudioStreamInteractive::instantiate_playback() {
	Ref<AudioStreamPlaybackInteractive> playback_transitioner;
	playback_transitioner.instantiate();
	playback_transitioner->stream = Ref<AudioStreamInteractive>(this);
	return playback_transitioner;
}

void AudioStreamInteractive::set_clip_count(int p_count) {
	ERR_FAIL_COND(p_count < 0 || p_count > MAX_CLIPS);

	AudioServer::get_singleton()->lock();

	if (p_count < clip_count) {
		// Removing should stop players.
		version++;
	}

#ifdef TOOLS_ENABLED
	stream_name_cache = "";
	if (p_count < clip_count) {
		for (int i = 0; i < clip_count; i++) {
			if (clips[i].auto_advance_next_clip >= p_count) {
				clips[i].auto_advance_next_clip = 0;
				clips[i].auto_advance = AUTO_ADVANCE_DISABLED;
			}
		}

		for (KeyValue<TransitionKey, Transition> &K : transition_map) {
			if (K.value.filler_clip >= p_count) {
				K.value.use_filler_clip = false;
				K.value.filler_clip = 0;
			}
		}
		if (initial_clip >= p_count) {
			initial_clip = 0;
		}
	}
#endif
	clip_count = p_count;
	AudioServer::get_singleton()->unlock();

	notify_property_list_changed();
	emit_signal(SNAME("parameter_list_changed"));
	emit_changed();
}

void AudioStreamInteractive::set_initial_clip(int p_clip) {
	ERR_FAIL_INDEX(p_clip, clip_count);
	initial_clip = p_clip;
	emit_changed();
}

int AudioStreamInteractive::get_initial_clip() const {
	return initial_clip;
}

int AudioStreamInteractive::get_clip_count() const {
	return clip_count;
}

void AudioStreamInteractive::set_clip_name(int p_clip, const StringName &p_name) {
	ERR_FAIL_INDEX(p_clip, MAX_CLIPS);
	clips[p_clip].name = p_name;
	emit_changed();
}

StringName AudioStreamInteractive::get_clip_name(int p_clip) const {
	ERR_FAIL_COND_V(p_clip < -1 || p_clip >= MAX_CLIPS, StringName());
	if (p_clip == CLIP_ANY) {
		return RTR("All Clips");
	}
	return clips[p_clip].name;
}

void AudioStreamInteractive::set_clip_stream(int p_clip, const Ref<AudioStream> &p_stream) {
	ERR_FAIL_INDEX(p_clip, MAX_CLIPS);
	AudioServer::get_singleton()->lock();
	if (clips[p_clip].stream.is_valid()) {
		version++;
	}
	clips[p_clip].stream = p_stream;
	AudioServer::get_singleton()->unlock();
#ifdef TOOLS_ENABLED
	if (Engine::get_singleton()->is_editor_hint()) {
		if (clips[p_clip].name == StringName() && p_stream.is_valid()) {
			String n;
			if (!clips[p_clip].stream->get_name().is_empty()) {
				n = clips[p_clip].stream->get_name().replace_char(',', ' ');
			} else if (clips[p_clip].stream->get_path().is_resource_file()) {
				n = clips[p_clip].stream->get_path().get_file().get_basename().replace_char(',', ' ');
				n = n.capitalize();
			}

			if (n != "") {
				clips[p_clip].name = n;
			}
		}
	}
#endif

#ifdef TOOLS_ENABLED
	stream_name_cache = "";
	notify_property_list_changed(); // Hints change if stream changes.
	emit_signal(SNAME("parameter_list_changed"));
#endif

	emit_changed();
}

Ref<AudioStream> AudioStreamInteractive::get_clip_stream(int p_clip) const {
	ERR_FAIL_INDEX_V(p_clip, MAX_CLIPS, Ref<AudioStream>());
	return clips[p_clip].stream;
}

void AudioStreamInteractive::set_clip_auto_advance(int p_clip, AutoAdvanceMode p_mode) {
	ERR_FAIL_INDEX(p_clip, MAX_CLIPS);
	ERR_FAIL_INDEX(p_mode, 3);
	clips[p_clip].auto_advance = p_mode;
	notify_property_list_changed();
	emit_changed();
}

AudioStreamInteractive::AutoAdvanceMode AudioStreamInteractive::get_clip_auto_advance(int p_clip) const {
	ERR_FAIL_INDEX_V(p_clip, MAX_CLIPS, AUTO_ADVANCE_DISABLED);
	return clips[p_clip].auto_advance;
}

void AudioStreamInteractive::set_clip_auto_advance_next_clip(int p_clip, int p_index) {
	ERR_FAIL_INDEX(p_clip, MAX_CLIPS);
	clips[p_clip].auto_advance_next_clip = p_index;
	emit_changed();
}

int AudioStreamInteractive::get_clip_auto_advance_next_clip(int p_clip) const {
	ERR_FAIL_INDEX_V(p_clip, MAX_CLIPS, -1);
	return clips[p_clip].auto_advance_next_clip;
}

// TRANSITIONS

void AudioStreamInteractive::_set_transitions(const Dictionary &p_transitions) {
	_block_emit_changed();

	for (const KeyValue<Variant, Variant> &kv : p_transitions) {
		Vector2i k = kv.key;
		Dictionary data = kv.value;
		ERR_CONTINUE(!data.has("from_time"));
		ERR_CONTINUE(!data.has("to_time"));
		ERR_CONTINUE(!data.has("fade_mode"));
		ERR_CONTINUE(!data.has("fade_beats"));
		bool use_filler_clip = false;
		int filler_clip = 0;
		float filler_clip_offset_beats = 0.0;
		if (data.has("use_filler_clip") && data.has("filler_clip")) {
			use_filler_clip = data["use_filler_clip"];
			filler_clip = data["filler_clip"];
			if (data.has("filler_clip_offset_beats")) {
				filler_clip_offset_beats = float(data["filler_clip_offset_beats"]);
			}
		}
		bool hold_previous = data.has("hold_previous") ? bool(data["hold_previous"]) : false;
		TransitionToCueTiming to_cue_timing = data.has("to_cue_timing") ? TransitionToCueTiming(int(data["to_cue_timing"])) : TRANSITION_TO_CUE_TIMING_DEFAULT;
		float fade_offset_beats = data.has("fade_offset_beats") ? float(data["fade_offset_beats"]) : 0.0;
		float fade_ease_exp = data.has("fade_ease_exp") ? float(data["fade_ease_exp"]) : 0.0;
		float to_fade_beats = data.has("to_fade_beats") ? float(data["to_fade_beats"]) : 0.0;
		float to_fade_offset_beats = data.has("to_fade_offset_beats") ? float(data["to_fade_offset_beats"]) : 0.0;
		float to_fade_ease_exp = data.has("to_fade_ease_exp") ? float(data["to_fade_ease_exp"]) : 0.0;

		add_transition(k.x, k.y, TransitionFromTime(int(data["from_time"])), TransitionToTime(int(data["to_time"])), FadeMode(int(data["fade_mode"])), data["fade_beats"], use_filler_clip, filler_clip, hold_previous, to_cue_timing, fade_offset_beats, fade_ease_exp, to_fade_beats, to_fade_offset_beats, to_fade_ease_exp, filler_clip_offset_beats);
	}

	_unblock_emit_changed();
}

Dictionary AudioStreamInteractive::_get_transitions() const {
	Vector<Vector2i> keys;

	for (const KeyValue<TransitionKey, Transition> &K : transition_map) {
		keys.push_back(Vector2i(K.key.from_clip, K.key.to_clip));
	}
	keys.sort();
	Dictionary ret;
	for (int i = 0; i < keys.size(); i++) {
		const Transition &tr = transition_map[TransitionKey(keys[i].x, keys[i].y)];
		Dictionary data;
		data["from_time"] = tr.from_time;
		data["to_time"] = tr.to_time;
		data["fade_mode"] = tr.fade_mode;
		data["fade_beats"] = tr.fade_beats;
		if (tr.use_filler_clip) {
			data["use_filler_clip"] = true;
			data["filler_clip"] = tr.filler_clip;
			if (tr.filler_clip_offset_beats != 0.0) {
				data["filler_clip_offset_beats"] = tr.filler_clip_offset_beats;
			}
		}
		if (tr.hold_previous) {
			data["hold_previous"] = true;
		}
		if (tr.to_cue_timing != TRANSITION_TO_CUE_TIMING_DEFAULT) {
			data["to_cue_timing"] = tr.to_cue_timing;
		}
		if (tr.fade_offset_beats != 0.0) {
			data["fade_offset_beats"] = tr.fade_offset_beats;
		}
		if (tr.fade_ease_exp != 0.0) {
			data["fade_ease_exp"] = tr.fade_ease_exp;
		}
		if (tr.to_fade_beats != 0.0) {
			data["to_fade_beats"] = tr.to_fade_beats;
		}
		if (tr.to_fade_offset_beats != 0.0) {
			data["to_fade_offset_beats"] = tr.to_fade_offset_beats;
		}
		if (tr.to_fade_ease_exp != 0.0) {
			data["to_fade_ease_exp"] = tr.to_fade_ease_exp;
		}

		ret[keys[i]] = data;
	}
	return ret;
}

bool AudioStreamInteractive::has_transition(int p_from_clip, int p_to_clip) const {
	TransitionKey tk(p_from_clip, p_to_clip);
	return transition_map.has(tk);
}

void AudioStreamInteractive::erase_transition(int p_from_clip, int p_to_clip) {
	TransitionKey tk(p_from_clip, p_to_clip);
	ERR_FAIL_COND(!transition_map.has(tk));
	AudioDriver::get_singleton()->lock();
	transition_map.erase(tk);
	AudioDriver::get_singleton()->unlock();
	emit_changed();
}

PackedInt32Array AudioStreamInteractive::get_transition_list() const {
	PackedInt32Array ret;

	for (const KeyValue<TransitionKey, Transition> &K : transition_map) {
		ret.push_back(K.key.from_clip);
		ret.push_back(K.key.to_clip);
	}
	return ret;
}

AudioStreamInteractive::TransitionMixResult AudioStreamInteractive::mix_transition(int p_from_clip, int p_to_clip, float from_pos, bool p_is_auto_advance) const {
	ERR_FAIL_COND_V(p_from_clip < -1 || p_from_clip >= clip_count, TransitionMixResult());
	ERR_FAIL_COND_V(p_to_clip < -1 || p_to_clip >= clip_count, TransitionMixResult());

	Transition transition; // Use an empty transition by default

	TransitionKey tkeys[4] = {
		TransitionKey(p_from_clip, p_to_clip),
		TransitionKey(p_from_clip, AudioStreamInteractive::CLIP_ANY),
		TransitionKey(AudioStreamInteractive::CLIP_ANY, p_to_clip),
		TransitionKey(AudioStreamInteractive::CLIP_ANY, AudioStreamInteractive::CLIP_ANY)
	};

	for (int i = 0; i < 4; i++) {
		if (transition_map.has(tkeys[i])) {
			transition = transition_map[tkeys[i]];
			break;
		}
	}

	Ref<AudioStream> from_stream = p_from_clip == AudioStreamInteractive::CLIP_ANY ? nullptr : clips[p_from_clip].stream;
	Ref<AudioStream> to_stream = p_to_clip == AudioStreamInteractive::CLIP_ANY ? nullptr : clips[p_to_clip].stream;

	Ref<AudioStream> filler_stream = transition.use_filler_clip && transition.filler_clip >= 0 && transition.filler_clip < clip_count && p_from_clip != transition.filler_clip && p_to_clip != transition.filler_clip ? clips[transition.filler_clip].stream : nullptr;

	return AudioStreamInteractive::compute_mix(transition, from_pos, from_stream, to_stream, filler_stream, p_is_auto_advance);
}

void AudioStreamInteractive::add_transition(
		int p_from_clip, int p_to_clip, TransitionFromTime p_from_time, TransitionToTime p_to_time, FadeMode p_fade_mode, float p_fade_beats,
		bool p_use_filler_flip, int p_filler_clip, bool p_hold_previous,
		TransitionToCueTiming p_to_cue_timing,
		float p_fade_offset_beats, float p_fade_ease_exp,
		float p_to_fade_beats, float p_to_fade_offset_beats, float p_to_fade_ease_exp,
		float p_filler_clip_offset_beats) {
	ERR_FAIL_COND(p_from_clip < CLIP_ANY || p_from_clip >= clip_count);
	ERR_FAIL_COND(p_to_clip < CLIP_ANY || p_to_clip >= clip_count);
	ERR_FAIL_UNSIGNED_INDEX(p_from_time, TRANSITION_FROM_TIME_MAX);
	ERR_FAIL_UNSIGNED_INDEX(p_to_time, TRANSITION_TO_TIME_MAX);
	ERR_FAIL_UNSIGNED_INDEX(p_fade_mode, FADE_MAX);

	Transition tr;
	tr.from_time = p_from_time;
	tr.to_time = p_to_time;
	tr.fade_mode = p_fade_mode;
	tr.fade_beats = p_fade_beats;
	tr.use_filler_clip = p_use_filler_flip;
	tr.filler_clip = p_filler_clip;
	tr.hold_previous = p_hold_previous;
	tr.to_cue_timing = p_to_cue_timing;
	tr.fade_offset_beats = p_fade_offset_beats;
	tr.fade_ease_exp = p_fade_ease_exp;
	tr.to_fade_beats = p_to_fade_beats;
	tr.to_fade_offset_beats = p_to_fade_offset_beats;
	tr.to_fade_ease_exp = p_to_fade_ease_exp;
	tr.filler_clip_offset_beats = p_filler_clip_offset_beats;

	TransitionKey tk(p_from_clip, p_to_clip);

	AudioDriver::get_singleton()->lock();
	transition_map[tk] = tr;
	AudioDriver::get_singleton()->unlock();

	emit_changed();
}

AudioStreamInteractive::TransitionFromTime AudioStreamInteractive::get_transition_from_time(int p_from_clip, int p_to_clip) const {
	TransitionKey tk(p_from_clip, p_to_clip);
	ERR_FAIL_COND_V(!transition_map.has(tk), TRANSITION_FROM_TIME_END);
	return transition_map[tk].from_time;
}

AudioStreamInteractive::TransitionToTime AudioStreamInteractive::get_transition_to_time(int p_from_clip, int p_to_clip) const {
	TransitionKey tk(p_from_clip, p_to_clip);
	ERR_FAIL_COND_V(!transition_map.has(tk), TRANSITION_TO_TIME_START);
	return transition_map[tk].to_time;
}

AudioStreamInteractive::FadeMode AudioStreamInteractive::get_transition_fade_mode(int p_from_clip, int p_to_clip) const {
	TransitionKey tk(p_from_clip, p_to_clip);
	ERR_FAIL_COND_V(!transition_map.has(tk), FADE_DISABLED);
	return transition_map[tk].fade_mode;
}

float AudioStreamInteractive::get_transition_fade_beats(int p_from_clip, int p_to_clip) const {
	TransitionKey tk(p_from_clip, p_to_clip);
	ERR_FAIL_COND_V(!transition_map.has(tk), -1);
	return transition_map[tk].fade_beats;
}

bool AudioStreamInteractive::is_transition_using_filler_clip(int p_from_clip, int p_to_clip) const {
	TransitionKey tk(p_from_clip, p_to_clip);
	ERR_FAIL_COND_V(!transition_map.has(tk), false);
	return transition_map[tk].use_filler_clip;
}

int AudioStreamInteractive::get_transition_filler_clip(int p_from_clip, int p_to_clip) const {
	TransitionKey tk(p_from_clip, p_to_clip);
	ERR_FAIL_COND_V(!transition_map.has(tk), -1);
	return transition_map[tk].filler_clip;
}

bool AudioStreamInteractive::is_transition_holding_previous(int p_from_clip, int p_to_clip) const {
	TransitionKey tk(p_from_clip, p_to_clip);
	ERR_FAIL_COND_V(!transition_map.has(tk), false);
	return transition_map[tk].hold_previous;
}

AudioStreamInteractive::TransitionToCueTiming AudioStreamInteractive::get_transition_to_cue_timing(int p_from_clip, int p_to_clip) const {
	TransitionKey tk(p_from_clip, p_to_clip);
	ERR_FAIL_COND_V(!transition_map.has(tk), TRANSITION_TO_CUE_TIMING_DEFAULT);
	return transition_map[tk].to_cue_timing;
}

float AudioStreamInteractive::get_transition_fade_offset_beats(int p_from_clip, int p_to_clip) const {
	TransitionKey tk(p_from_clip, p_to_clip);
	ERR_FAIL_COND_V(!transition_map.has(tk), 0.0);
	return transition_map[tk].fade_offset_beats;
}

float AudioStreamInteractive::get_transition_fade_ease_exp(int p_from_clip, int p_to_clip) const {
	TransitionKey tk(p_from_clip, p_to_clip);
	ERR_FAIL_COND_V(!transition_map.has(tk), 0.0);
	return transition_map[tk].fade_ease_exp;
}

float AudioStreamInteractive::get_transition_to_fade_beats(int p_from_clip, int p_to_clip) const {
	TransitionKey tk(p_from_clip, p_to_clip);
	ERR_FAIL_COND_V(!transition_map.has(tk), 0.0);
	return transition_map[tk].to_fade_beats;
}

float AudioStreamInteractive::get_transition_to_fade_offset_beats(int p_from_clip, int p_to_clip) const {
	TransitionKey tk(p_from_clip, p_to_clip);
	ERR_FAIL_COND_V(!transition_map.has(tk), 0.0);
	return transition_map[tk].to_fade_offset_beats;
}

float AudioStreamInteractive::get_transition_to_fade_ease_exp(int p_from_clip, int p_to_clip) const {
	TransitionKey tk(p_from_clip, p_to_clip);
	ERR_FAIL_COND_V(!transition_map.has(tk), 0.0);
	return transition_map[tk].to_fade_ease_exp;
}

float AudioStreamInteractive::get_transition_filler_clip_offset_beats(int p_from_clip, int p_to_clip) const {
	TransitionKey tk(p_from_clip, p_to_clip);
	ERR_FAIL_COND_V(!transition_map.has(tk), 0.0);
	return transition_map[tk].filler_clip_offset_beats;
}

#ifdef TOOLS_ENABLED

PackedStringArray AudioStreamInteractive::_get_linked_undo_properties(const String &p_property, const Variant &p_new_value) const {
	PackedStringArray ret;

	if (p_property.begins_with("clip_") && p_property.ends_with("/stream")) {
		int clip = p_property.get_slicec('_', 1).to_int();
		if (clip < clip_count) {
			ret.push_back("clip_" + itos(clip) + "/name");
		}
	}

	if (p_property == "clip_count") {
		int new_clip_count = p_new_value;

		if (new_clip_count < clip_count) {
			for (int i = 0; i < clip_count; i++) {
				if (clips[i].auto_advance_next_clip >= new_clip_count) {
					ret.push_back("clip_" + itos(i) + "/auto_advance");
					ret.push_back("clip_" + itos(i) + "/next_clip");
				}
			}

			ret.push_back("_transitions");
			if (initial_clip >= new_clip_count) {
				ret.push_back("initial_clip");
			}
		}
	}
	return ret;
}

template <class T>
static void _test_and_swap(T &p_elem, uint32_t p_a, uint32_t p_b) {
	if ((uint32_t)p_elem == p_a) {
		p_elem = p_b;
	} else if (uint32_t(p_elem) == p_b) {
		p_elem = p_a;
	}
}

void AudioStreamInteractive::_inspector_array_swap_clip(uint32_t p_item_a, uint32_t p_item_b) {
	ERR_FAIL_UNSIGNED_INDEX(p_item_a, (uint32_t)clip_count);
	ERR_FAIL_UNSIGNED_INDEX(p_item_b, (uint32_t)clip_count);

	for (int i = 0; i < clip_count; i++) {
		_test_and_swap(clips[i].auto_advance_next_clip, p_item_a, p_item_b);
	}

	Vector<TransitionKey> to_remove;
	HashMap<TransitionKey, Transition, TransitionKeyHasher> to_add;

	for (KeyValue<TransitionKey, Transition> &K : transition_map) {
		if (K.key.from_clip == p_item_a || K.key.from_clip == p_item_b || K.key.to_clip == p_item_a || K.key.to_clip == p_item_b) {
			to_remove.push_back(K.key);
			TransitionKey new_key = K.key;
			_test_and_swap(new_key.from_clip, p_item_a, p_item_b);
			_test_and_swap(new_key.to_clip, p_item_a, p_item_b);
			to_add[new_key] = K.value;
		}
	}

	for (int i = 0; i < to_remove.size(); i++) {
		transition_map.erase(to_remove[i]);
	}

	for (KeyValue<TransitionKey, Transition> &K : to_add) {
		transition_map.insert(K.key, K.value);
	}

	SWAP(clips[p_item_a], clips[p_item_b]);

	stream_name_cache = "";

	notify_property_list_changed();
	emit_signal(SNAME("parameter_list_changed"));
	emit_changed();
}

String AudioStreamInteractive::_get_streams_hint() const {
	if (!stream_name_cache.is_empty()) {
		return stream_name_cache;
	}

	for (int i = 0; i < clip_count; i++) {
		if (i > 0) {
			stream_name_cache += ",";
		}
		String n = String(clips[i].name).replace_char(',', ' ');

		if (n == "" && clips[i].stream.is_valid()) {
			if (!clips[i].stream->get_name().is_empty()) {
				n = clips[i].stream->get_name().replace_char(',', ' ');
			} else if (clips[i].stream->get_path().is_resource_file()) {
				n = clips[i].stream->get_path().get_file().replace_char(',', ' ');
			}
		}

		if (n == "") {
			n = "Clip " + itos(i);
		}

		stream_name_cache += n;
	}

	return stream_name_cache;
}

#endif

void AudioStreamInteractive::_validate_property(PropertyInfo &r_property) const {
	String prop = r_property.name;

	if (Engine::get_singleton()->is_editor_hint() && prop == "switch_to") {
#ifdef TOOLS_ENABLED
		r_property.hint_string = _get_streams_hint();
#endif
		return;
	}

	if (Engine::get_singleton()->is_editor_hint() && prop == "initial_clip") {
#ifdef TOOLS_ENABLED
		if (get_clip_count() == 0) {
			// Hide property since there's no clip to choose from.
			r_property.usage = PROPERTY_USAGE_NO_EDITOR;
		}
		r_property.hint_string = _get_streams_hint();
#endif
	} else if (prop.begins_with("clip_") && prop != "clip_count") {
		int clip = prop.get_slicec('_', 1).to_int();
		if (clip >= clip_count) {
			r_property.usage = PROPERTY_USAGE_INTERNAL;
		} else if (prop == "clip_" + itos(clip) + "/next_clip") {
			if (clips[clip].auto_advance != AUTO_ADVANCE_ENABLED) {
				r_property.usage = PROPERTY_USAGE_NONE;
			} else if (Engine::get_singleton()->is_editor_hint()) {
#ifdef TOOLS_ENABLED
				r_property.hint_string = _get_streams_hint();
#endif
			}
		}
	}
}

void AudioStreamInteractive::get_parameter_list(List<Parameter> *r_parameters) {
	String clip_names;
	for (int i = 0; i < clip_count; i++) {
		clip_names += ",";
		clip_names += clips[i].name;
	}
	r_parameters->push_back(Parameter(PropertyInfo(Variant::STRING, "switch_to_clip", PROPERTY_HINT_ENUM, clip_names, PROPERTY_USAGE_EDITOR), ""));
}

void AudioStreamInteractive::_bind_methods() {
#ifdef TOOLS_ENABLED
	ClassDB::bind_method(D_METHOD("_get_linked_undo_properties", "for_property", "for_value"), &AudioStreamInteractive::_get_linked_undo_properties);
	ClassDB::bind_method(D_METHOD("_inspector_array_swap_clip", "a", "b"), &AudioStreamInteractive::_inspector_array_swap_clip);
#endif

	// CLIPS

	ClassDB::bind_method(D_METHOD("set_clip_count", "clip_count"), &AudioStreamInteractive::set_clip_count);
	ClassDB::bind_method(D_METHOD("get_clip_count"), &AudioStreamInteractive::get_clip_count);

	ClassDB::bind_method(D_METHOD("set_initial_clip", "clip_index"), &AudioStreamInteractive::set_initial_clip);
	ClassDB::bind_method(D_METHOD("get_initial_clip"), &AudioStreamInteractive::get_initial_clip);

	ClassDB::bind_method(D_METHOD("set_clip_name", "clip_index", "name"), &AudioStreamInteractive::set_clip_name);
	ClassDB::bind_method(D_METHOD("get_clip_name", "clip_index"), &AudioStreamInteractive::get_clip_name);

	ClassDB::bind_method(D_METHOD("set_clip_stream", "clip_index", "stream"), &AudioStreamInteractive::set_clip_stream);
	ClassDB::bind_method(D_METHOD("get_clip_stream", "clip_index"), &AudioStreamInteractive::get_clip_stream);

	ClassDB::bind_method(D_METHOD("set_clip_auto_advance", "clip_index", "mode"), &AudioStreamInteractive::set_clip_auto_advance);
	ClassDB::bind_method(D_METHOD("get_clip_auto_advance", "clip_index"), &AudioStreamInteractive::get_clip_auto_advance);

	ClassDB::bind_method(D_METHOD("set_clip_auto_advance_next_clip", "clip_index", "auto_advance_next_clip"), &AudioStreamInteractive::set_clip_auto_advance_next_clip);
	ClassDB::bind_method(D_METHOD("get_clip_auto_advance_next_clip", "clip_index"), &AudioStreamInteractive::get_clip_auto_advance_next_clip);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "clip_count", PROPERTY_HINT_RANGE, "1," + itos(MAX_CLIPS), PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_ARRAY, "Clips,clip_,page_size=999,unfoldable,numbered,swap_method=_inspector_array_swap_clip,add_button_text=" + String(TTRC("Add Clip"))), "set_clip_count", "get_clip_count");
	for (int i = 0; i < MAX_CLIPS; i++) {
		ADD_PROPERTYI(PropertyInfo(Variant::STRING_NAME, "clip_" + itos(i) + "/name", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_INTERNAL), "set_clip_name", "get_clip_name", i);
		ADD_PROPERTYI(PropertyInfo(Variant::OBJECT, "clip_" + itos(i) + "/stream", PROPERTY_HINT_RESOURCE_TYPE, AudioStream::get_class_static(), PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_INTERNAL), "set_clip_stream", "get_clip_stream", i);
		ADD_PROPERTYI(PropertyInfo(Variant::INT, "clip_" + itos(i) + "/auto_advance", PROPERTY_HINT_ENUM, "Disabled,Enabled,ReturnToHold", PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_INTERNAL), "set_clip_auto_advance", "get_clip_auto_advance", i);
		ADD_PROPERTYI(PropertyInfo(Variant::INT, "clip_" + itos(i) + "/next_clip", PROPERTY_HINT_ENUM, "", PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_INTERNAL), "set_clip_auto_advance_next_clip", "get_clip_auto_advance_next_clip", i);
	}

	// Needs to be registered after `clip_*` properties, as it depends on them.
	ADD_PROPERTY(PropertyInfo(Variant::INT, "initial_clip", PROPERTY_HINT_ENUM, "", PROPERTY_USAGE_DEFAULT), "set_initial_clip", "get_initial_clip");

	// TRANSITIONS

	ClassDB::bind_method(D_METHOD("add_transition", "from_clip", "to_clip", "from_time", "to_time", "fade_mode", "fade_beats", "use_filler_clip", "filler_clip", "hold_previous", "to_cue_timing", "fade_offset_beats", "fade_ease_exp", "to_fade_beats", "to_fade_offset_beats", "to_fade_ease_exp", "filler_clip_offset_beats"), &AudioStreamInteractive::add_transition, DEFVAL(false), DEFVAL(-1), DEFVAL(false), DEFVAL(TRANSITION_TO_CUE_TIMING_DEFAULT), DEFVAL(0.0), DEFVAL(0.0), DEFVAL(0.0), DEFVAL(0.0), DEFVAL(0.0), DEFVAL(0.0));
	ClassDB::bind_method(D_METHOD("has_transition", "from_clip", "to_clip"), &AudioStreamInteractive::has_transition);
	ClassDB::bind_method(D_METHOD("erase_transition", "from_clip", "to_clip"), &AudioStreamInteractive::erase_transition);
	ClassDB::bind_method(D_METHOD("get_transition_list"), &AudioStreamInteractive::get_transition_list);

	ClassDB::bind_method(D_METHOD("get_transition_from_time", "from_clip", "to_clip"), &AudioStreamInteractive::get_transition_from_time);
	ClassDB::bind_method(D_METHOD("get_transition_to_time", "from_clip", "to_clip"), &AudioStreamInteractive::get_transition_to_time);
	ClassDB::bind_method(D_METHOD("get_transition_fade_mode", "from_clip", "to_clip"), &AudioStreamInteractive::get_transition_fade_mode);
	ClassDB::bind_method(D_METHOD("get_transition_fade_beats", "from_clip", "to_clip"), &AudioStreamInteractive::get_transition_fade_beats);
	ClassDB::bind_method(D_METHOD("is_transition_using_filler_clip", "from_clip", "to_clip"), &AudioStreamInteractive::is_transition_using_filler_clip);
	ClassDB::bind_method(D_METHOD("get_transition_filler_clip", "from_clip", "to_clip"), &AudioStreamInteractive::get_transition_filler_clip);
	ClassDB::bind_method(D_METHOD("is_transition_holding_previous", "from_clip", "to_clip"), &AudioStreamInteractive::is_transition_holding_previous);
	ClassDB::bind_method(D_METHOD("get_transition_to_cue_timing", "from_clip", "to_clip"), &AudioStreamInteractive::get_transition_to_cue_timing);
	ClassDB::bind_method(D_METHOD("get_transition_fade_offset_beats", "from_clip", "to_clip"), &AudioStreamInteractive::get_transition_fade_offset_beats);
	ClassDB::bind_method(D_METHOD("get_transition_fade_ease_exp", "from_clip", "to_clip"), &AudioStreamInteractive::get_transition_fade_ease_exp);
	ClassDB::bind_method(D_METHOD("get_transition_to_fade_beats", "from_clip", "to_clip"), &AudioStreamInteractive::get_transition_to_fade_beats);
	ClassDB::bind_method(D_METHOD("get_transition_to_fade_offset_beats", "from_clip", "to_clip"), &AudioStreamInteractive::get_transition_to_fade_offset_beats);
	ClassDB::bind_method(D_METHOD("get_transition_to_fade_ease_exp", "from_clip", "to_clip"), &AudioStreamInteractive::get_transition_to_fade_ease_exp);
	ClassDB::bind_method(D_METHOD("get_transition_filler_clip_offset_beats", "from_clip", "to_clip"), &AudioStreamInteractive::get_transition_filler_clip_offset_beats);

	ClassDB::bind_method(D_METHOD("_set_transitions", "transitions"), &AudioStreamInteractive::_set_transitions);
	ClassDB::bind_method(D_METHOD("_get_transitions"), &AudioStreamInteractive::_get_transitions);

	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "_transitions", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR | PROPERTY_USAGE_INTERNAL), "_set_transitions", "_get_transitions");

	BIND_ENUM_CONSTANT(TRANSITION_FROM_TIME_IMMEDIATE);
	BIND_ENUM_CONSTANT(TRANSITION_FROM_TIME_NEXT_BEAT);
	BIND_ENUM_CONSTANT(TRANSITION_FROM_TIME_NEXT_BAR);
	BIND_ENUM_CONSTANT(TRANSITION_FROM_TIME_END);

	BIND_ENUM_CONSTANT(TRANSITION_TO_TIME_SAME_POSITION);
	BIND_ENUM_CONSTANT(TRANSITION_TO_TIME_START);
	BIND_ENUM_CONSTANT(TRANSITION_TO_TIME_PREVIOUS_POSITION);

	BIND_ENUM_CONSTANT(FADE_DISABLED);
	BIND_ENUM_CONSTANT(FADE_IN);
	BIND_ENUM_CONSTANT(FADE_OUT);
	BIND_ENUM_CONSTANT(FADE_CROSS);
	BIND_ENUM_CONSTANT(FADE_AUTOMATIC);

	BIND_ENUM_CONSTANT(AUTO_ADVANCE_DISABLED);
	BIND_ENUM_CONSTANT(AUTO_ADVANCE_ENABLED);
	BIND_ENUM_CONSTANT(AUTO_ADVANCE_RETURN_TO_HOLD);

	BIND_ENUM_CONSTANT(TRANSITION_TO_CUE_TIMING_DEFAULT);
	BIND_ENUM_CONSTANT(TRANSITION_TO_CUE_TIMING_AFTER_FILLER);
	BIND_ENUM_CONSTANT(TRANSITION_TO_CUE_TIMING_AFTER_FADE_OUT);
	BIND_ENUM_CONSTANT(TRANSITION_TO_CUE_TIMING_IMMEDIATE);

	BIND_CONSTANT(CLIP_ANY);
}

///////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////
AudioStreamPlaybackInteractive::AudioStreamPlaybackInteractive() {
}

AudioStreamPlaybackInteractive::~AudioStreamPlaybackInteractive() {
}

void AudioStreamPlaybackInteractive::stop() {
	if (!active) {
		return;
	}

	active = false;

	for (int i = 0; i < AudioStreamInteractive::MAX_CLIPS; i++) {
		if (states[i].playback.is_valid()) {
			states[i].playback->stop();
		}
		states[i].fade_speed = 0.0;
		states[i].fade_volume = 0.0;
		states[i].fade_wait = 0.0;
		states[i].reset_fade();
		states[i].active = false;
		states[i].auto_advance = -1;
		states[i].first_mix = true;
	}
}

void AudioStreamPlaybackInteractive::start(double p_from_pos) {
	start_clip(stream->initial_clip, 0.0);
}

void AudioStreamPlaybackInteractive::start_clip(int p_clip, double p_from_pos) {
	if (active) {
		stop();
	}

	if (version != stream->version) {
		for (int i = 0; i < AudioStreamInteractive::MAX_CLIPS; i++) {
			Ref<AudioStream> src_stream;
			if (i < stream->clip_count) {
				src_stream = stream->clips[i].stream;
			}
			if (states[i].stream != src_stream) {
				states[i].stream.unref();
				states[i].playback.unref();

				states[i].stream = src_stream;
				states[i].playback = src_stream->instantiate_playback();
			}
		}

		version = stream->version;
	}

	if (p_clip < 0 || p_clip >= stream->clip_count) {
		return; // No playback possible.
	}
	if (states[p_clip].playback.is_null()) {
		return; //no playback possible
	}
	active = true;

	playback_time = 0.0;

	_start_clip(p_clip, p_from_pos);
}

void AudioStreamPlaybackInteractive::_start_clip(int p_clip, double p_from_pos) {
	State &state = states[p_clip];
	state.active = true;
	state.fade_wait = 0;
	state.fade_volume = 1.0;
	state.fade_speed = 0;
	state.first_mix = true;

	state.playback->start(p_from_pos);

	playback_current = p_clip;

	if (stream->clips[p_clip].auto_advance == AudioStreamInteractive::AUTO_ADVANCE_ENABLED && stream->clips[p_clip].auto_advance_next_clip >= 0 && stream->clips[p_clip].auto_advance_next_clip < stream->clip_count && stream->clips[p_clip].auto_advance_next_clip != p_clip) {
		//prepare auto advance
		state.auto_advance = stream->clips[p_clip].auto_advance_next_clip;
	}
}

void AudioStreamPlaybackInteractive::_queue(int p_to_clip_index, bool p_is_auto_advance) {
	ERR_FAIL_INDEX(p_to_clip_index, stream->clip_count);
	ERR_FAIL_COND(states[p_to_clip_index].playback.is_null());

	if (playback_current == -1) {
		// Nothing to do, start.
		_start_clip(p_to_clip_index, 0.0);
		return;
	}

	for (int i = 0; i < stream->clip_count; i++) {
		if (i == playback_current || i == p_to_clip_index) {
			continue;
		}
		if (states[i].active && states[i].fade_wait > 0) { // Waiting to kick in, terminate because change of plans.
			states[i].playback->stop();
			states[i].reset_fade();
			states[i].active = false;
		}
	}

	State &from_state = states[playback_current];
	State &to_state = states[p_to_clip_index];

	AudioStreamInteractive::Transition transition; // Use an empty transition by default

	AudioStreamInteractive::TransitionKey tkeys[4] = {
		AudioStreamInteractive::TransitionKey(playback_current, p_to_clip_index),
		AudioStreamInteractive::TransitionKey(playback_current, AudioStreamInteractive::CLIP_ANY),
		AudioStreamInteractive::TransitionKey(AudioStreamInteractive::CLIP_ANY, p_to_clip_index),
		AudioStreamInteractive::TransitionKey(AudioStreamInteractive::CLIP_ANY, AudioStreamInteractive::CLIP_ANY)
	};

	for (int i = 0; i < 4; i++) {
		if (stream->transition_map.has(tkeys[i])) {
			transition = stream->transition_map[tkeys[i]];
			break;
		}
	}

	float current_pos = from_state.playback->get_playback_position();

	bool has_filler_clip = transition.use_filler_clip && transition.filler_clip >= 0 && transition.filler_clip < (int)stream->clip_count && states[transition.filler_clip].playback.is_valid() && playback_current != transition.filler_clip && p_to_clip_index != transition.filler_clip;

	AudioStreamInteractive::TransitionMixResult mix = AudioStreamInteractive::compute_mix(transition, current_pos, from_state.stream, to_state.stream, has_filler_clip ? states[transition.filler_clip].stream : nullptr, p_is_auto_advance);

	if (p_is_auto_advance) {
		transition.from_time = AudioStreamInteractive::TRANSITION_FROM_TIME_END;
		if (transition.to_time == AudioStreamInteractive::TRANSITION_TO_TIME_SAME_POSITION) {
			transition.to_time = AudioStreamInteractive::TRANSITION_TO_TIME_START;
		}
	}

	float dst_seek_to = 0;
	if (transition.to_time == AudioStreamInteractive::TRANSITION_TO_TIME_PREVIOUS_POSITION && to_state.stream->get_length() > 0.0) {
		dst_seek_to = to_state.previous_position;
	} else if (transition.to_time == AudioStreamInteractive::TRANSITION_TO_TIME_SAME_POSITION && transition.from_time != AudioStreamInteractive::TRANSITION_FROM_TIME_END && to_state.stream->get_length() > 0.0) {
		// Seeking to basically same position as when we start fading.
		dst_seek_to = current_pos + mix.start_t;
		float end;
		if (to_state.stream->get_bpm() > 0 && to_state.stream->get_beat_count()) {
			end = to_state.stream->get_beat_count() * mix.to_beat_sec;
		} else {
			end = to_state.stream->get_length();
		}

		if (dst_seek_to > end) {
			// Seeking too far away.
			dst_seek_to = 0; //past end, loop to beginning.
		}
	} else {
		// Seek to Start
		dst_seek_to = 0.0;
	}

	int auto_advance_to = -1;

	if (stream->clips[p_to_clip_index].auto_advance == AudioStreamInteractive::AUTO_ADVANCE_ENABLED) {
		int next_clip = stream->clips[p_to_clip_index].auto_advance_next_clip;
		if (next_clip >= 0 && next_clip < (int)stream->clip_count && states[next_clip].playback.is_valid() && next_clip != p_to_clip_index && (!transition.use_filler_clip || next_clip != transition.filler_clip)) {
			auto_advance_to = next_clip;
		}
	}

	if (return_memory != -1 && stream->clips[p_to_clip_index].auto_advance == AudioStreamInteractive::AUTO_ADVANCE_RETURN_TO_HOLD) {
		auto_advance_to = return_memory;
		return_memory = -1;
	}

	if (transition.hold_previous) {
		return_memory = playback_current;
	}

	from_state.fade_wait = mix.from_fade_start_t;
	from_state.fade_speed = mix.from_fade_speed;
	from_state.fade_ease_exp = mix.from_fade_ease_exp;
	from_state.fade_ease_t = 1.0;
	from_state.fade_ease_volume = from_state.fade_volume;

	to_state.playback->start(dst_seek_to);
	to_state.active = true;
	to_state.first_mix = true;
	to_state.fade_volume = mix.to_fade_speed != 0.0 ? 0.0 : 1.0;
	to_state.fade_wait = mix.to_start_t;
	to_state.fade_speed = mix.to_fade_speed;
	to_state.fade_ease_exp = mix.to_fade_ease_exp;
	to_state.fade_ease_t = 0.0;
	to_state.fade_ease_volume = 1.0;
	to_state.auto_advance = auto_advance_to;

	if (has_filler_clip) {
		State &filler_state = states[transition.filler_clip];

		filler_state.playback->start(0);
		filler_state.active = true;

		// Filler state does not fade (bake fade in the audio clip if you want fading.
		filler_state.fade_volume = 1.0;
		filler_state.fade_speed = 0.0;

		filler_state.fade_wait = mix.filler_start_t;
		filler_state.first_mix = true;
	}
}

void AudioStreamPlaybackInteractive::seek(double p_time) {
	// Seek not supported
}

int AudioStreamPlaybackInteractive::mix(AudioFrame *p_buffer, float p_rate_scale, int p_frames) {
	if (active && version != stream->version) {
		stop();
	}

	if (switch_request != -1) {
		_queue(switch_request, false);
		switch_request = -1;
	}

	if (!active) {
		return 0;
	}

	int todo = p_frames;

	while (todo) {
		int to_mix = MIN(todo, BUFFER_SIZE);
		_mix_internal(to_mix);
		for (int i = 0; i < to_mix; i++) {
			p_buffer[i] = mix_buffer[i];
		}
		p_buffer += to_mix;
		todo -= to_mix;
	}

	playback_time += double(p_frames) / double(AudioServer::get_singleton()->get_mix_rate());

	return p_frames;
}

void AudioStreamPlaybackInteractive::_mix_internal(int p_frames) {
	for (int i = 0; i < p_frames; i++) {
		mix_buffer[i] = AudioFrame(0, 0);
	}

	bool any_active = false;
	for (int i = 0; i < stream->clip_count; i++) {
		if (!states[i].active) {
			continue;
		}

		_mix_internal_state(i, p_frames);

		any_active = states[i].active || any_active;
	}
	if (!any_active) {
		active = false;
		playback_current = -1;
	}
}

void AudioStreamPlaybackInteractive::_mix_internal_state(int p_state_idx, int p_frames) {
	State &state = states[p_state_idx];
	double mix_rate = double(AudioServer::get_singleton()->get_mix_rate());
	double frame_inc = 1.0 / mix_rate;

	int from_frame = 0;
	int queue_next = -1;

	if (state.first_mix) {
		// Did not start mixing yet, wait.
		double mix_time = p_frames * frame_inc;
		if (state.fade_wait < mix_time) {
			// time to start!
			from_frame = state.fade_wait * mix_rate;
			state.fade_wait = 0;
			if (state.fade_speed == 0.0) {
				queue_next = state.auto_advance;
			}
			playback_current = p_state_idx;
			state.first_mix = false;
		} else {
			// This is for fade in of new stream.
			state.fade_wait -= mix_time;
			return; // Nothing to do
		}
	}

	state.previous_position = state.playback->get_playback_position();
	state.playback->mix(temp_buffer + from_frame, 1.0, p_frames - from_frame);

	double frame_fade_inc = state.fade_speed * frame_inc;
	for (int i = from_frame; i < p_frames; i++) {
		if (state.fade_wait) {
			// This is for fade out of existing stream;
			state.fade_wait -= frame_inc;
			if (state.fade_wait < 0.0) {
				state.fade_wait = 0.0;
			}
		} else if (frame_fade_inc > 0) {
			if (state.fade_ease_exp) {
				state.fade_ease_t += frame_fade_inc;
				state.fade_volume = state.fade_ease_volume * Math::ease(state.fade_ease_t, state.fade_ease_exp);
			} else {
				state.fade_volume += frame_fade_inc;
			}
			if (state.fade_volume >= 1.0) {
				state.fade_speed = 0.0;
				frame_fade_inc = 0.0;
				state.fade_volume = 1.0;
				queue_next = state.auto_advance;
			}
		} else if (frame_fade_inc < 0.0) {
			if (state.fade_ease_exp) {
				state.fade_ease_t += frame_fade_inc;
				state.fade_volume = state.fade_ease_volume * Math::ease(state.fade_ease_t, state.fade_ease_exp);
			} else {
				state.fade_volume += frame_fade_inc;
			}
			if (state.fade_volume <= 0.0) {
				state.fade_speed = 0.0;
				frame_fade_inc = 0.0;
				state.fade_volume = 0.0;
				state.playback->stop(); // Stop playback and break, no point to continue mixing
				break;
			}
		}

		mix_buffer[i] += temp_buffer[i] * state.fade_volume;
		state.previous_position += frame_inc;
	}

	if (!state.playback->is_playing()) {
		// It finished because it either reached end or faded out, so deactivate and continue.
		state.active = false;
	}
	if (queue_next != -1) {
		_queue(queue_next, true);
	}
}

void AudioStreamPlaybackInteractive::tag_used_streams() {
	for (int i = 0; i < stream->clip_count; i++) {
		if (states[i].active && !states[i].first_mix && states[i].playback->is_playing()) {
			states[i].stream->tag_used(states[i].playback->get_playback_position());
		}
	}
	stream->tag_used(0);
}

void AudioStreamPlaybackInteractive::switch_to_clip_by_name(const StringName &p_name) {
	if (p_name == StringName()) {
		switch_request = -1;
		return;
	}

	ERR_FAIL_COND_MSG(stream.is_null(), "Attempted to switch while not playing back any stream.");

	for (int i = 0; i < stream->get_clip_count(); i++) {
		if (stream->get_clip_name(i) == p_name) {
			switch_request = i;
			return;
		}
	}
	ERR_FAIL_MSG("Clip not found: " + String(p_name));
}

void AudioStreamPlaybackInteractive::set_parameter(const StringName &p_name, const Variant &p_value) {
	if (p_name == SNAME("switch_to_clip")) {
		switch_to_clip_by_name(p_value);
	}
}

Variant AudioStreamPlaybackInteractive::get_parameter(const StringName &p_name) const {
	if (p_name == SNAME("switch_to_clip")) {
		for (int i = 0; i < stream->get_clip_count(); i++) {
			if (switch_request != -1) {
				if (switch_request == i) {
					return String(stream->get_clip_name(i));
				}
			} else if (playback_current == i) {
				return String(stream->get_clip_name(i));
			}
		}
		return "";
	}

	return Variant();
}

void AudioStreamPlaybackInteractive::switch_to_clip(int p_index) {
	switch_request = p_index;
}

int AudioStreamPlaybackInteractive::get_current_clip_index() const {
	return playback_current;
}

int AudioStreamPlaybackInteractive::get_loop_count() const {
	return 0; // Looping not supported
}

double AudioStreamPlaybackInteractive::get_playback_position() const {
	return playback_time;
}

bool AudioStreamPlaybackInteractive::is_playing() const {
	return active;
}

void AudioStreamPlaybackInteractive::_bind_methods() {
	ClassDB::bind_method(D_METHOD("switch_to_clip_by_name", "clip_name"), &AudioStreamPlaybackInteractive::switch_to_clip_by_name);
	ClassDB::bind_method(D_METHOD("switch_to_clip", "clip_index"), &AudioStreamPlaybackInteractive::switch_to_clip);
	ClassDB::bind_method(D_METHOD("get_current_clip_index"), &AudioStreamPlaybackInteractive::get_current_clip_index);
}
