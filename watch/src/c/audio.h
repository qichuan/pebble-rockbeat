#pragma once

// ---------------------------------------------------------------------------
// Drum sounds, synthesised into a raw PCM stream the app owns outright.
//
// Because the app mixes every voice itself into one stream, there is exactly one
// audio source and preemption cannot happen -- the question of whether one SDK
// playback call interrupts another never arises. It also means a backing music
// track would be extra voices in the same mixer rather than a rewrite.
//
// EVERY function here is fire-and-forget. play_don()/play_ka() are O(1): they
// claim a voice slot and return, never touching the speaker, never looping,
// never blocking. Nothing in this module is ever read by the timing or scoring
// path -- that is what makes the game immune to speaker latency.
//
// The whole implementation sits behind PBL_API_EXISTS(speaker_stream_open) and
// degrades to no-ops, so the game plays and scores identically with no speaker.
// ---------------------------------------------------------------------------

#include <pebble.h>
#include <stdbool.h>
#include <stdint.h>

#include "chart.h"

void audio_init(void);

// False when the SDK exposes no speaker at all, so the UI can hide the toggle.
bool audio_is_available(void);

void audio_set_enabled(bool enabled);
bool audio_is_enabled(void);

void audio_song_start(void);  // opens and primes the stream
void audio_song_stop(void);   // closes; already-buffered audio still plays out

// MID is the drum head ("don"); TOP and BOTTOM are the rim ("ka") -- the
// standard taiko centre/rim split.
void audio_play_lane(uint8_t lane, uint8_t note_type);

// Renders and writes one frame's worth of samples. Call last in the frame, after
// every state mutation and after layer_mark_dirty().
void audio_pump(uint32_t now_ms);
