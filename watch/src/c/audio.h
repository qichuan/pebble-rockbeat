#pragma once

// ---------------------------------------------------------------------------
// Backing music, played by the watch's note sequencer.
//
// The MIDI is parsed at build time into SpeakerNote tracks (see music.h) and
// handed to speaker_play_tracks() a chunk at a time, each chunk released
// against the song clock a measured latency before it is due to sound. It is
// NOT chained from the previous chunk's finish callback -- that accumulated the
// speaker's per-call startup cost on real hardware. See the scheduling block in
// audio.c.
//
// EVERY function here is fire-and-forget, and nothing in this module is ever
// read by the timing or scoring path. That is what keeps the game immune to
// speaker latency: the song clock drives the audio, never the reverse.
//
// The module graph enforces it. audio.c does not include clock.h at all -- song
// time arrives as a parameter, the same discipline game.c follows. So there is
// no clock for this module to read even by accident, and no way for a slow or
// stalled speaker to feed back into note timing.
//
// There are no reactive hit sounds any more. A PCM stream cannot coexist with
// the sequencer (speaker_stream_open() returns false while tracks play,
// measured -- see music.h), so the two are mutually exclusive and the music
// wins. The drum part is in the music, so the player still hears the beat they
// are hitting; hit CONFIRMATION is haptics plus the on-screen flash.
//
// The whole implementation sits behind the speaker feature macro and degrades
// to no-ops, so the game plays and scores identically with no speaker at all.
// ---------------------------------------------------------------------------

#include <stdbool.h>
#include <stdint.h>

void audio_init(void);

// False when the SDK exposes no speaker at all, so the UI can hide the toggle.
bool audio_is_available(void);

void audio_set_enabled(bool enabled);
bool audio_is_enabled(void);

// `elapsed_ms` is song time, so this doubles as resume. Resuming mid-chunk
// waits for the next chunk boundary rather than restarting the current chunk:
// the sequencer cannot be started from the middle of a chunk, and a few seconds
// of silence is far better than a few seconds of music against the wrong notes.
// `song` indexes the same table as chart_get().
void audio_song_start(uint8_t song, uint32_t elapsed_ms);

// Releases the music when SONG time reaches its start. Call once per frame
// from the frame timer, after the game has stepped. Must be song time, not
// wall time -- see the comment on the implementation.
void audio_tick(uint32_t elapsed_ms);

void audio_song_stop(void);
