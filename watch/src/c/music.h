#pragma once

// ---------------------------------------------------------------------------
// Backing music as note-sequencer data, generated from the MIDI by
// tools/make_chart.py. music.c is a GENERATED file.
//
// The watch has no MIDI file parser, but it does have a note sequencer:
// speaker_play_tracks() takes arrays of {midi_note, waveform, duration_ms,
// velocity}. So the MIDI is parsed at build time and shipped as those arrays.
// That is why this project no longer carries a 911KB PCM resource -- the same
// section is under 3KB of note tables.
//
// It is ONE track: the melody line, extracted from the MIDI by the skyline
// algorithm and transposed into the speaker's usable band. A four-track
// reduction of the full arrangement was tried first and sounded noisy no matter
// how it was tuned -- see the README. Four fixed-amplitude waveforms cannot
// carry a dense pop arrangement on a driver this small; one clean line can.
//
// Everything below was MEASURED on the emery emulator; the SDK header documents
// only the two cap constants and none of the behaviour.
//
//  * SPEAKER_MAX_NOTES (256) is a PER-TRACK cap, not a per-call budget: 4
//    tracks of 128 (512 total) plays fine.
//  * Exceeding it FAULTS the app rather than returning false. The generator
//    therefore refuses to emit an over-long chunk; nothing here range-checks it
//    at runtime because by then it is too late.
//  * Chaining the next chunk from the finish callback costs NO measurable gap,
//    and no rate error: consecutive 8135ms chunks came 8073/8186/8103/8193/8083ms
//    apart on the song clock, which is +-60ms of jitter about the right answer.
//    ON HARDWARE THIS IS FALSE, and it is the one measurement here that mattered
//    -- see the block below.
//  * The FIRST speaker_play_tracks() call SWALLOWS ~200ms of the chunk it is
//    given, rather than costing latency before it, so every later chunk inherits
//    a head start and the whole song plays early. Restarting a chunk cold, after
//    the sequencer has idled a couple of hundred ms, comes back ~200ms short for
//    the same reason. ALSO FALSE ON HARDWARE, and inverted.
//  * A PCM stream CANNOT coexist with the sequencer: speaker_stream_open()
//    returns false while tracks are playing (the music itself survives -- it
//    finishes with reason Done, not Preempted). This is why the reactive don/ka
//    hit sounds are gone: the two audio sources are mutually exclusive, and the
//    drums are in the music track anyway.
//
// MEASURED ON A REAL PEBBLE TIME 2, and the emulator is the mirror image of it:
//
//  * EVERY speaker_play_tracks() call costs latency BEFORE sound appears, rather
//    than swallowing content after it: ~170ms, measured at 158-179ms across six
//    chunks of two songs. It is a fixed per-call cost -- independent of chunk
//    duration, of note count, and of whether anything sounded before it.
//  * Chaining therefore ACCUMULATES that cost, because a chunk cannot start
//    until the previous one ends and the previous one already started late. The
//    lag reached 522/752/883ms across one song and could not come back: music
//    running late cannot be fast-forwarded.
//
// So audio.c does not chain. Each chunk is released against the song clock, that
// much before it is due, and the previous one is stopped. See RB_MUSIC_CALL_MS
// and the scheduling block in audio.c.
//
// The general lesson is worth more than the numbers: the emulator's speaker is
// not a model of the watch's, it is the OPPOSITE of it, and a timing constant
// derived from it can be confidently wrong in the wrong direction.
// ---------------------------------------------------------------------------

#include <pebble.h>
#include <stdint.h>

#define MUSIC_TRACKS 4

typedef struct {
  const SpeakerNote *notes[MUSIC_TRACKS];
  uint16_t counts[MUSIC_TRACKS];
  uint8_t track_count;   // 1 today; the array allows an arrangement to come back
  uint32_t start_ms;     // offset into the song at which this chunk sounds
  uint32_t duration_ms;  // every track in a chunk is padded to exactly this
} MusicChunk;

// One entry per playable song, indexed the same way as chart_get(). The two
// tables are generated from the same list in one pass, so song N's music and
// song N's chart always come from the same MIDI and the same tempo map.
typedef struct {
  const MusicChunk *chunks;
  uint16_t chunk_count;
  uint32_t total_ms;
} MusicSong;

uint16_t music_chunk_count(uint8_t song);

// NULL past the end -- which is how audio.c detects the song is over. Also NULL
// for an unknown song, so a bad index falls silent rather than playing the
// wrong tune.
const MusicChunk *music_chunk(uint8_t song, uint16_t index);

uint32_t music_total_ms(uint8_t song);
