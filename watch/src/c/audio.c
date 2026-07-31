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

// Which song's music is playing. Set once at audio_song_start() and read by
// every music_chunk() lookup, so a chunk can never be fetched from a song other
// than the one whose chart is on screen.
static uint8_t s_song;

// Set by the speaker's finish callback, consumed by the app task. `volatile`
// because the two run in different contexts and the compiler must not cache
// either across the frame loop's read.
static volatile bool s_finished;
static volatile uint8_t s_finish_reason;

// Song time by which the playing chunk should have reported finishing. The
// watchdog in audio_tick() uses it; see the note there.
static uint32_t s_deadline_ms;

static void prv_play_chunk(uint16_t index, uint32_t elapsed_ms);

// Song time at which a chunk should be handed to the sequencer. One definition,
// used by both the initial release and the boundary re-anchor, so the two can
// never disagree about where the music belongs.
static uint32_t prv_chunk_due_ms(const MusicChunk *chunk) {
  const int32_t due_ms =
      (int32_t)RB_MUSIC_START_MS + (int32_t)chunk->start_ms + RB_MUSIC_OFFSET_MS;
  return (due_ms > 0) ? (uint32_t)due_ms : 0u;
}

// ---------------------------------------------------------------------------
// Chunk chaining, and re-anchoring the music to the song clock
//
// A chunk boundary is where the next batch of notes is handed over -- and it is
// also the ONLY place the music can be re-synchronised, because the sequencer
// plays a note list and reports no position, so nothing can be corrected part
// way through a chunk.
//
// That matters because the two timelines are driven by different things: the
// notes advance on the song clock, the speaker advances on real time. Any
// difference between those rates accumulates. It was measured at 1346ms by the
// first boundary and ~2100ms by the fourth -- the music was most of two beats
// ahead of the notes it was supposed to accompany. Most of that was the song
// clock losing time while it calibrated (fixed in clock.c), but nothing here
// could have detected it, let alone corrected it.
//
// So the handover compares song time against where the next chunk is supposed to
// start. Within RB_MUSIC_RESYNC_MS it goes out as soon as the app task can send
// it; beyond it, when the music has run ahead, the chunk is held until the song
// clock catches up -- a short silence at a note boundary, in exchange for the
// music and the notes being the same thing again.
//
// Only early is corrected. Music that is running LATE cannot be fast-forwarded:
// the note list would have to be re-emitted, and there is nowhere to do that.
//
// ---------------------------------------------------------------------------
// The finish callback RECORDS, it does not act.
//
// It runs in the speaker driver's own context, and everything it might want to
// do -- speaker_play_tracks(), speaker_set_finish_callback() -- is a call back
// into the driver that just called us. Re-entering a driver from its own
// completion callback is the kind of thing that works on an emulator, whose
// implementation is far more forgiving, and wedges or panics real firmware. This
// app did exactly that at every chunk boundary, roughly every 8 seconds, and the
// watch rebooted after a while.
//
// So the callback now only sets a flag. Every speaker call is made from the app
// task, in audio_tick(), where blocking is safe and the current song time is
// available directly rather than as a copy that is up to a frame stale.
//
// The cost is that a handover waits for the next frame, so a chunk boundary is
// no longer gapless. That is a real regression in continuity, and it is accepted
// deliberately: a gap measured in one frame is not something a player can pick
// out at a note boundary, and a reboot is.
// ---------------------------------------------------------------------------

static void prv_finished(SpeakerFinishReason reason, void *ctx) {
  (void)ctx;
  if (!s_playing) {
    return;  // our own speaker_stop(); not a chain point
  }
  s_finish_reason = (uint8_t)reason;
  s_finished = true;
}

// Runs on the app task, once the finish callback has flagged a boundary.
static void prv_handle_finished(uint32_t elapsed_ms) {
  s_finished = false;

  // Anything other than a clean finish means something else has taken the
  // speaker. Stop rather than fight for it -- a rhythm game that keeps
  // re-grabbing the output would stutter, and audio is only an output here.
  if (s_finish_reason != (uint8_t)SpeakerFinishReasonDone) {
#if RB_DEBUG_LOG_AUDIO
    APP_LOG(APP_LOG_LEVEL_DEBUG, "music ended early, reason=%d", (int)s_finish_reason);
#endif
    s_playing = false;
    speaker_set_finish_callback(NULL, NULL);
    return;
  }

  const MusicChunk *const next = music_chunk(s_song, s_next_chunk);
  if (next == NULL) {
    prv_play_chunk(s_next_chunk, elapsed_ms);  // ends it, clears the callback
    return;
  }

  // Positive error means the chunk is not due yet, i.e. the music has run AHEAD
  // of the notes.
  const uint32_t due_ms = prv_chunk_due_ms(next);
  const int32_t error_ms = (int32_t)due_ms - (int32_t)elapsed_ms;

#if RB_DEBUG_LOG_AUDIO
  // "late" is the quantity being tuned: this boundary is when the next chunk
  // starts sounding, compared against the song time its notes were charted at.
  // It is measured against the UNSHIFTED ideal on purpose -- comparing against
  // due_ms would be comparing the offset with itself, which is a number that
  // cannot be driven to zero by changing it.
  APP_LOG(APP_LOG_LEVEL_DEBUG, "chunk %u boundary: song=%lums late=%ldms (due=%lums)",
          (unsigned)s_next_chunk, (unsigned long)elapsed_ms,
          (long)((int32_t)elapsed_ms - (int32_t)(RB_MUSIC_START_MS + next->start_ms)),
          (unsigned long)due_ms);
#endif

  if (error_ms > RB_MUSIC_RESYNC_MS) {
    s_armed_chunk = s_next_chunk;
    s_armed_at_ms = due_ms;
    s_armed = true;
    return;  // audio_tick() releases it when the song clock arrives
  }

  prv_play_chunk(s_next_chunk, elapsed_ms);
}

static void prv_play_chunk(uint16_t index, uint32_t elapsed_ms) {
  const MusicChunk *const chunk = music_chunk(s_song, index);
  if (chunk == NULL) {  // song over
    s_playing = false;
    speaker_set_finish_callback(NULL, NULL);
    return;
  }

  SpeakerTrack tracks[MUSIC_TRACKS];
  const uint8_t count = (chunk->track_count < MUSIC_TRACKS) ? chunk->track_count
                                                            : MUSIC_TRACKS;
  for (uint8_t t = 0; t < count; t++) {
    tracks[t].notes = chunk->notes[t];
    tracks[t].num_notes = chunk->counts[t];
    tracks[t].sample = NULL;  // waveform synthesis; no sample-backed track now
  }

  s_next_chunk = (uint16_t)(index + 1);

  // How long this chunk should take, plus slack, in song time. Recomputed from
  // the CURRENT clock reading rather than from where the chunk was due, so a
  // late start does not make the watchdog trigger-happy.
  s_deadline_ms = elapsed_ms + chunk->duration_ms + RB_MUSIC_STALL_MS;

  if (!speaker_play_tracks(tracks, count, RB_AUDIO_VOLUME)) {
#if RB_DEBUG_LOG_AUDIO
    APP_LOG(APP_LOG_LEVEL_DEBUG, "speaker_play_tracks failed at chunk %u", (unsigned)index);
#endif
    // Do NOT give up on the song. Failing here used to stop the music for good,
    // which turns one bad handover into "the second half has no music" -- the
    // whole remainder lost to a single transient. Skip to the next chunk at its
    // scheduled time instead, so the cost is one chunk and the music comes back.
    const MusicChunk *const next = music_chunk(s_song, s_next_chunk);
    if (next != NULL) {
      s_armed_chunk = s_next_chunk;
      s_armed_at_ms = prv_chunk_due_ms(next);
      s_armed = true;
    } else {
      s_playing = false;
      speaker_set_finish_callback(NULL, NULL);
    }
  }
}

static void prv_begin(uint16_t index, uint32_t elapsed_ms) {
#if RB_DEBUG_LOG_AUDIO
  APP_LOG(APP_LOG_LEVEL_DEBUG, "begin chunk %u: song=%lums armed_at=%lums late=%ldms",
          (unsigned)index, (unsigned long)elapsed_ms, (unsigned long)s_armed_at_ms,
          (long)((int32_t)elapsed_ms - (int32_t)s_armed_at_ms));
#else
  (void)elapsed_ms;
#endif
  s_armed = false;
  s_playing = true;
  speaker_set_finish_callback(prv_finished, NULL);
  prv_play_chunk(index, elapsed_ms);
}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

void audio_init(void) {
  s_playing = false;
  s_next_chunk = 0;
  s_armed = false;
  s_finished = false;
  s_deadline_ms = 0;
  s_song = 0;
#if RB_DEBUG_LOG_AUDIO
  APP_LOG(APP_LOG_LEVEL_DEBUG, "music: song 0 has %u chunks, %lums",
          (unsigned)music_chunk_count(s_song), (unsigned long)music_total_ms(s_song));
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

void audio_song_start(uint8_t song, uint32_t elapsed_ms) {
  if (s_playing || s_armed || !s_enabled) {
    return;
  }
  s_song = song;

  // Checked once per song rather than per frame. An app cannot override the
  // system mute, so when it is set there is nothing to be gained by playing.
  const bool muted = speaker_is_muted();
  const bool quiet = quiet_time_is_active();
  s_suppressed = muted || quiet;
#if RB_DEBUG_LOG_AUDIO
  APP_LOG(APP_LOG_LEVEL_DEBUG, "audio start song %u at %lums: muted=%d quiet=%d",
          (unsigned)song, (unsigned long)elapsed_ms, (int)muted, (int)quiet);
#endif
  if (s_suppressed) {
    return;
  }

  // Find the first chunk that has not already been passed. On a fresh song
  // that is chunk 0; on resume it is the next boundary, and the gap until then
  // stays silent rather than playing the wrong part of the song.
  const uint16_t count = music_chunk_count(s_song);
  uint16_t index = count;
  uint32_t start_at_ms = 0;
  for (uint16_t i = 0; i < count; i++) {
    const MusicChunk *const chunk = music_chunk(s_song, i);
    const uint32_t song_ms = prv_chunk_due_ms(chunk);
    if (song_ms >= elapsed_ms) {
      index = i;
      start_at_ms = song_ms;
      break;
    }
  }
  if (index >= count) {
    return;  // past the end of the music
  }

  // start_at_ms already carries RB_MUSIC_OFFSET_MS, which is what lines the
  // sequencer's startup behaviour up with the notes.
  s_armed_chunk = index;
  s_armed_at_ms = start_at_ms;
  s_armed = true;
  audio_tick(elapsed_ms);  // already due if the song is mid-flight
}

// Called once per frame with SONG time.
//
// The release is keyed to the song clock rather than to an AppTimer because they
// are not the same timeline: song time excludes pauses, and starts where the
// chart starts. A chunk armed for a song time is released at that point in the
// music regardless of what the app has been doing in between.
//
// Taking the clock as a parameter rather than sampling one inside audio.c keeps
// the dependency one-way: audio is still a pure output, and the timing loop
// still cannot come to depend on it.
// It is also where every speaker call is made from, including the ones a chunk
// boundary asks for -- see the note on prv_finished().
void audio_tick(uint32_t elapsed_ms) {
  // Watchdog: a chunk that never reports finishing must not take the rest of the
  // song with it.
  //
  // The whole handover chain hangs off one callback, so anything that swallows
  // it stops the music permanently and silently -- no error, nothing in the log,
  // just a song that goes quiet partway and stays quiet. That is not
  // hypothetical: a chunk over ~32s reproducibly never reports finishing, which
  // is why chunks are capped by duration in the generator. This is the runtime
  // half of that defence, and it covers the cases the cap cannot predict.
  //
  // The deadline is the chunk's own duration plus RB_MUSIC_STALL_MS of slack, so
  // ordinary lateness -- a resync wait, a slow frame, accumulated drift -- can
  // never trip it. Only a chunk that has genuinely stopped can.
  if (s_playing && !s_armed && !s_finished && elapsed_ms > s_deadline_ms) {
#if RB_DEBUG_LOG_AUDIO
    APP_LOG(APP_LOG_LEVEL_DEBUG, "chunk %u never finished by %lums -- forcing handover",
            (unsigned)(s_next_chunk - 1), (unsigned long)s_deadline_ms);
#endif
    speaker_stop();       // it may still be holding the output
    s_finish_reason = (uint8_t)SpeakerFinishReasonDone;
    s_finished = true;
  }

  if (s_finished) {
    prv_handle_finished(elapsed_ms);
  }
  if (!s_armed || elapsed_ms < s_armed_at_ms) {
    return;
  }
  prv_begin(s_armed_chunk, elapsed_ms);
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
  // Drop any boundary the callback flagged on the way out, so the next song does
  // not start by servicing the previous one's last chunk.
  s_finished = false;
}

#else  // No speaker API in this SDK -- everything degrades to a no-op.

static bool s_enabled = true;

void audio_init(void) {}
bool audio_is_available(void) { return false; }
void audio_set_enabled(bool enabled) { s_enabled = enabled; }
bool audio_is_enabled(void) { return s_enabled; }
void audio_song_start(uint8_t song, uint32_t elapsed_ms) { (void)song; (void)elapsed_ms; }
void audio_tick(uint32_t elapsed_ms) { (void)elapsed_ms; }
void audio_song_stop(void) {}

#endif
