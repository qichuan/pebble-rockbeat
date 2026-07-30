#include "audio.h"

#include <pebble.h>

#include "music.h"
#include "rb_config.h"

// Testing the SDK's feature macro directly rather than through
// PBL_API_EXISTS(): that macro expands to `defined(...)`, which GCC warns about
// as non-portable (-Wexpansion-to-defined). This is the identical condition
// without the warning.
//
// Note this is deliberately NOT `#ifdef PBL_SPEAKER`. Gabbro declares the real
// speaker functions but does not get that build define -- only emery and flint
// do -- so keying off PBL_SPEAKER would silently disable audio on a device that
// has a speaker. On the platforms that genuinely lack one, the SDK defines the
// speaker calls as macros expanding to (0).
#ifdef _PBL_API_EXISTS_speaker_play_tracks

static bool s_enabled = true;
static bool s_playing;
static bool s_suppressed;
static uint16_t s_next_chunk;

// The chunk waiting to be released, and the song time to release it at. Armed
// by audio_song_start(), fired by audio_tick(). Deliberately NOT an AppTimer --
// see the comment on audio_tick().
static bool s_armed;
static uint16_t s_armed_chunk;
static uint32_t s_armed_at_ms;

static void prv_play_chunk(uint16_t index);

// ---------------------------------------------------------------------------
// Chunk chaining
//
// Measured: chaining from the finish callback costs no audible gap, and drift
// against the game clock over six consecutive chunks was jitter (+17/-5/-12/
// +41/-30ms) rather than a rate error. So the sequencer stays locked to the
// notes without any correction, and a chunk boundary is simply where the next
// batch is handed over.
// ---------------------------------------------------------------------------

static void prv_finished(SpeakerFinishReason reason, void *ctx) {
  (void)ctx;
  if (!s_playing) {
    return;  // our own speaker_stop(); not a chain point
  }

  // Anything other than a clean finish means something else has taken the
  // speaker. Stop rather than fight for it -- a rhythm game that keeps
  // re-grabbing the output would stutter, and audio is only an output here.
  if (reason != SpeakerFinishReasonDone) {
#if RB_DEBUG_LOG_AUDIO
    APP_LOG(APP_LOG_LEVEL_DEBUG, "music ended early, reason=%d", (int)reason);
#endif
    s_playing = false;
    speaker_set_finish_callback(NULL, NULL);
    return;
  }

  prv_play_chunk(s_next_chunk);
}

static void prv_play_chunk(uint16_t index) {
  const MusicChunk *const chunk = music_chunk(index);
  if (chunk == NULL) {  // song over
    s_playing = false;
    speaker_set_finish_callback(NULL, NULL);
    return;
  }

  SpeakerTrack tracks[MUSIC_TRACKS];
  for (uint8_t t = 0; t < MUSIC_TRACKS; t++) {
    tracks[t].notes = chunk->notes[t];
    tracks[t].num_notes = chunk->counts[t];
    // Only the percussion track is sample-backed; the rest use their waveform.
    tracks[t].sample = (t == RB_MUSIC_PERC_TRACK) ? music_drum_sample() : NULL;
  }

  s_next_chunk = (uint16_t)(index + 1);
  if (!speaker_play_tracks(tracks, MUSIC_TRACKS, RB_AUDIO_VOLUME)) {
#if RB_DEBUG_LOG_AUDIO
    APP_LOG(APP_LOG_LEVEL_DEBUG, "speaker_play_tracks failed at chunk %u", (unsigned)index);
#endif
    s_playing = false;
    speaker_set_finish_callback(NULL, NULL);
  }
}

static void prv_begin(uint16_t index) {
  s_armed = false;
  s_playing = true;
  speaker_set_finish_callback(prv_finished, NULL);
  prv_play_chunk(index);
}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

void audio_init(void) {
  s_playing = false;
  s_next_chunk = 0;
  s_armed = false;
#if RB_DEBUG_LOG_AUDIO
  APP_LOG(APP_LOG_LEVEL_DEBUG, "music: %u chunks, %lums",
          (unsigned)music_chunk_count(), (unsigned long)music_total_ms());
#endif
}

bool audio_is_available(void) {
  return true;
}

void audio_set_enabled(bool enabled) {
  s_enabled = enabled;
  if (!enabled) {
    audio_song_stop();
  }
}

bool audio_is_enabled(void) {
  return s_enabled;
}

void audio_song_start(uint32_t elapsed_ms) {
  if (s_playing || s_armed || !s_enabled) {
    return;
  }

  // Checked once per song rather than per frame. An app cannot override the
  // system mute, so when it is set there is nothing to be gained by playing.
  const bool muted = speaker_is_muted();
  const bool quiet = quiet_time_is_active();
  s_suppressed = muted || quiet;
#if RB_DEBUG_LOG_AUDIO
  APP_LOG(APP_LOG_LEVEL_DEBUG, "audio start at %lums: muted=%d quiet=%d",
          (unsigned long)elapsed_ms, (int)muted, (int)quiet);
#endif
  if (s_suppressed) {
    return;
  }

  // Find the first chunk that has not already been passed. On a fresh song
  // that is chunk 0; on resume it is the next boundary, and the gap until then
  // stays silent rather than playing the wrong part of the song.
  const uint16_t count = music_chunk_count();
  uint16_t index = count;
  uint32_t start_at_ms = 0;
  for (uint16_t i = 0; i < count; i++) {
    const MusicChunk *const chunk = music_chunk(i);
    const uint32_t song_ms = RB_MUSIC_START_MS + chunk->start_ms;
    if (song_ms >= elapsed_ms) {
      index = i;
      start_at_ms = song_ms;
      break;
    }
  }
  if (index >= count) {
    return;  // past the end of the music
  }

  // The FIRST speaker_play_tracks() call costs ~200ms of startup latency that
  // later ones do not, so it is issued that much early. Measured, not guessed;
  // without it the whole track sits a fifth of a beat behind the notes.
  s_armed_chunk = index;
  s_armed_at_ms = (start_at_ms > RB_MUSIC_LATENCY_MS)
                      ? (start_at_ms - RB_MUSIC_LATENCY_MS) : 0;
  s_armed = true;
  audio_tick(elapsed_ms);  // already due if the song is mid-flight
}

// Called once per frame with SONG time.
//
// The release has to be keyed to the song clock rather than to an AppTimer,
// because the two disagree by a lot exactly when it matters. clock.c spends its
// first seconds measuring its own tick rate and loses ~2s of song time doing it
// (verified: the clock-to-wall offset climbs to ~2s over the first 10s and is
// then flat for the rest of the song). A real-time timer armed for "1800ms from
// now" therefore fires while the song clock still reads ~1300 -- starting the
// music the better part of a second ahead of the notes, and staying there,
// since the offset never comes back.
//
// Reading the clock here rather than sampling one inside audio.c also keeps the
// dependency one-way: audio is still a pure output.
void audio_tick(uint32_t elapsed_ms) {
  if (!s_armed || elapsed_ms < s_armed_at_ms) {
    return;
  }
  prv_begin(s_armed_chunk);
}

void audio_song_stop(void) {
  s_armed = false;
  if (!s_playing) {
    return;
  }
  // Cleared BEFORE stopping: speaker_stop() fires the finish callback, and a
  // chain step from it here would restart the music we are trying to end.
  s_playing = false;
  speaker_set_finish_callback(NULL, NULL);
  speaker_stop();
  s_next_chunk = 0;
}

#else  // No speaker API in this SDK -- everything degrades to a no-op.

static bool s_enabled = true;

void audio_init(void) {}
bool audio_is_available(void) { return false; }
void audio_set_enabled(bool enabled) { s_enabled = enabled; }
bool audio_is_enabled(void) { return s_enabled; }
void audio_song_start(uint32_t elapsed_ms) { (void)elapsed_ms; }
void audio_tick(uint32_t elapsed_ms) { (void)elapsed_ms; }
void audio_song_stop(void) {}

#endif
