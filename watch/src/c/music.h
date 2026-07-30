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
//    The music does not walk away from the notes.
//  * Restarting a chunk COLD, after the sequencer has been left idle a couple of
//    hundred ms, is a different matter -- it comes back ~200ms short. So chaining
//    is the good path and should stay the normal one; see RB_MUSIC_RESYNC_MS for
//    why a re-sync that fires too eagerly makes the problem it is correcting.
//  * The FIRST speaker_play_tracks() call SWALLOWS ~200ms of the chunk it is
//    given, rather than costing latency before it. Every later chunk then
//    inherits that head start, so the whole song plays early without it. See
//    RB_MUSIC_OFFSET_MS -- and note the sign, which earlier builds had backwards.
//  * A PCM stream CANNOT coexist with the sequencer: speaker_stream_open()
//    returns false while tracks are playing (the music itself survives -- it
//    finishes with reason Done, not Preempted). This is why the reactive don/ka
//    hit sounds are gone: the two audio sources are mutually exclusive, and the
//    drums are in the music track anyway.
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

uint16_t music_chunk_count(void);

// NULL past the end -- which is how audio.c detects the song is over.
const MusicChunk *music_chunk(uint16_t index);

uint32_t music_total_ms(void);
