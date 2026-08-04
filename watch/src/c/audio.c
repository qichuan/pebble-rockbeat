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

// Which song's music is playing. Set once at audio_song_start() and read by
// every music_chunk() lookup, so a chunk can never be fetched from a song other
// than the one whose chart is on screen.
static uint8_t s_song;

// The chunk waiting to be handed to the sequencer. EVERY chunk goes through
// this, including the first and including a mid-song boundary -- see the note
// on scheduling below. `s_release_at_ms` is when to make the speaker call;
// `s_due_ms` is when the chunk's first note should actually sound, and the two
// differ by the measured cost of the call.
static bool s_armed;
static uint16_t s_armed_chunk;
static uint32_t s_release_at_ms;
static uint32_t s_due_ms;

// Set by the speaker's finish callback, consumed by the app task. `volatile`
// because the two may run in different contexts and the compiler must not cache
// either across the frame loop's read.
static volatile bool s_finished;
static volatile uint8_t s_finish_reason;

// Where the chunk currently sounding was supposed to stop sounding. Only read
// to log the calibration residual; nothing schedules off it.
static uint32_t s_content_end_ms;

// Song time at which a chunk's first note should SOUND. One definition, used by
// every scheduling decision, so nothing can disagree about where the music
// belongs.
static uint32_t prv_chunk_due_ms(const MusicChunk *chunk) {
  const int32_t due_ms =
      (int32_t)RB_MUSIC_START_MS + (int32_t)chunk->start_ms + RB_MUSIC_OFFSET_MS;
  return (due_ms > 0) ? (uint32_t)due_ms : 0u;
}

// ---------------------------------------------------------------------------
// Scheduling: every chunk is anchored to the song clock, INDEPENDENTLY.
//
// The obvious design -- and the one this had until it was measured on real
// hardware -- is to chain: hand the next chunk over when the current one reports
// finishing. It is wrong, and the emulator cannot show why, because the cost it
// hides is zero there and large on the watch.
//
// MEASURED on a Pebble Time 2: a speaker_play_tracks() call does not start
// sounding immediately. It costs ~170ms every time -- see RB_MUSIC_CALL_MS for
// the samples and for why it is one constant rather than a cold-start ramp.
// Chaining pays that cost ONCE PER CHUNK and then carries it forever, because
// the next chunk cannot start until the current one ends and the current one
// already started late. The lag ratcheted up at every boundary -- 522ms, 752ms,
// 883ms behind the notes -- and nothing could pull it back, because music
// running late cannot be fast-forwarded: the note list would have to be
// re-emitted and there is nowhere to do that.
//
// So the chain is gone. Each chunk is released at
//
//     due_ms - (the measured cost of the call)
//
// which puts the sound on the note rather than behind it, and -- the part that
// actually matters -- makes every chunk's error INDEPENDENT of every previous
// chunk's. A boundary that goes badly costs that boundary and nothing after it.
// Error is bounded by how well the latency constant is measured plus one frame
// of release granularity, for a song of any length.
//
// The previous chunk is stopped explicitly, because the release now happens
// while it is still sounding. speaker_stop() is documented ("stop any active
// speaker playback immediately") where an overlapping speaker_play_tracks() is
// not -- SpeakerFinishReasonPreempted is about a higher-priority system source
// taking the output, not about an app's own second call, so relying on it to do
// a handover would be building on an undocumented behaviour.
//
// The cost is real and worth stating: the last RB_MUSIC_CALL_MS of every
// chunk is cut off and never heard. That is silence moved rather than silence
// added -- chaining put the same gap immediately AFTER the boundary, where it
// delayed real notes; this puts it immediately BEFORE, where it eats the tail of
// a note that was about to be cut off by the next one anyway.
//
// ---------------------------------------------------------------------------
// The finish callback RECORDS, it does not act.
//
// It may run in the speaker driver's own context, and everything it might want
// to do -- speaker_play_tracks(), speaker_set_finish_callback() -- is a call
// back into the driver that just called us. Re-entering a driver from its own
// completion callback is the kind of thing that works on an emulator, whose
// implementation is far more forgiving, and wedges or panics real firmware. This
// app did exactly that at every chunk boundary, roughly every 16 seconds, and
// the watch rebooted after a while.
//
// (The SDK header now says the callback "runs on the app task". That may well be
// true on current firmware; it was not worth re-testing, because with scheduling
// on the song clock the callback has no work to do anyway.)
//
// Every speaker call is made from audio_tick(), on the app task, where blocking
// is safe and the current song time is available directly rather than as a copy
// that is up to a frame stale.
// ---------------------------------------------------------------------------

static void prv_finished(SpeakerFinishReason reason, void *ctx) {
  (void)ctx;
  if (!s_playing) {
    return;  // our own speaker_stop() on the way out; nothing to report
  }
  s_finish_reason = (uint8_t)reason;
  s_finished = true;
}

// Point s_armed at `index`, or clear it if the song has no such chunk.
//
// One latency for every chunk, including the first of a song: measured across
// three songs, a speaker call costs the same whether or not anything has sounded
// before it. See RB_MUSIC_CALL_MS, which records the measurement and the one
// outlier that does not fit it.
static void prv_arm(uint16_t index) {
  const MusicChunk *const chunk = music_chunk(s_song, index);
  if (chunk == NULL) {
    s_armed = false;  // past the last chunk; the song's music is done
    return;
  }

  const uint32_t due_ms = prv_chunk_due_ms(chunk);

  s_armed_chunk = index;
  s_due_ms = due_ms;
  s_release_at_ms = (due_ms > RB_MUSIC_CALL_MS) ? (due_ms - RB_MUSIC_CALL_MS) : 0u;
  s_armed = true;
}

// Hand the armed chunk to the sequencer, and arm the one after it.
static void prv_release(uint32_t elapsed_ms) {
  const MusicChunk *const chunk = music_chunk(s_song, s_armed_chunk);
  if (chunk == NULL) {  // defensive; prv_arm() does not arm a missing chunk
    s_armed = false;
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

#if RB_DEBUG_LOG_AUDIO
  APP_LOG(APP_LOG_LEVEL_DEBUG, "release chunk %u: song=%lums due=%lums slip=%ldms",
          (unsigned)s_armed_chunk, (unsigned long)elapsed_ms, (unsigned long)s_due_ms,
          (long)((int32_t)elapsed_ms - (int32_t)s_release_at_ms));
#endif

  // The previous chunk is still sounding -- that is the point of releasing
  // early. Stop it rather than letting two note lists overlap.
  if (s_playing) {
    speaker_stop();
  }

  const uint16_t index = s_armed_chunk;
  s_content_end_ms = s_due_ms + chunk->duration_ms;

  if (speaker_play_tracks(tracks, count, RB_AUDIO_VOLUME)) {
    s_playing = true;
  } else {
#if RB_DEBUG_LOG_AUDIO
    APP_LOG(APP_LOG_LEVEL_DEBUG, "speaker_play_tracks failed at chunk %u", (unsigned)index);
#endif
    // Do NOT give up on the song. Failing here used to stop the music for good,
    // which turns one bad handover into "the second half has no music" -- the
    // whole remainder lost to a single transient. Arming the next chunk below
    // costs one chunk instead, and unlike the old chained design there is
    // nothing to recover: the next release was never going to depend on this
    // call succeeding.
    s_playing = false;
  }

  prv_arm((uint16_t)(index + 1));
  (void)elapsed_ms;
}

// Runs on the app task once the finish callback has flagged something.
//
// Nothing is scheduled from here any more. Mid-song a chunk is always stopped
// before its content runs out, so the only finish that should arrive naturally
// is the last chunk of the song.
static void prv_handle_finished(uint32_t elapsed_ms) {
  s_finished = false;

  const uint8_t reason = s_finish_reason;

  if (reason == (uint8_t)SpeakerFinishReasonStopped) {
    return;  // ours, from prv_release(); s_playing is about to be true again
  }

  if (reason == (uint8_t)SpeakerFinishReasonDone) {
#if RB_DEBUG_LOG_AUDIO
    // THE calibration signal. A chunk that runs to its natural end finishes this
    // far from where its content was charted to end, and that difference is
    // exactly (actual call latency - RB_MUSIC_CALL_MS). Positive means the
    // constant is too small. Only the final chunk of a song reports it, because
    // every other chunk is stopped early by design -- so it is one sample per
    // playthrough, and worth collecting from all three songs.
    APP_LOG(APP_LOG_LEVEL_DEBUG, "chunk done: song=%lums expected=%lums resid=%ldms",
            (unsigned long)elapsed_ms, (unsigned long)s_content_end_ms,
            (long)((int32_t)elapsed_ms - (int32_t)s_content_end_ms));
#endif
    s_playing = false;
    if (!s_armed) {
      speaker_set_finish_callback(NULL, NULL);  // song's music is over
    }
    return;
  }

  // Preempted or Error: something else has taken the speaker. Stop rather than
  // fight for it -- a rhythm game that keeps re-grabbing the output would
  // stutter, and audio is only an output here.
#if RB_DEBUG_LOG_AUDIO
  APP_LOG(APP_LOG_LEVEL_DEBUG, "music ended early, reason=%d", (int)reason);
#endif
  s_playing = false;
  s_armed = false;
  speaker_set_finish_callback(NULL, NULL);
}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

void audio_init(void) {
  s_playing = false;
  s_armed = false;
  s_finished = false;
  s_content_end_ms = 0;
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

  // Find the first chunk that has not already been passed. On a fresh song that
  // is chunk 0; on resume it is the next boundary, and the gap until then stays
  // silent rather than playing the wrong part of the song.
  const uint16_t count = music_chunk_count(s_song);
  uint16_t index = count;
  for (uint16_t i = 0; i < count; i++) {
    const MusicChunk *const chunk = music_chunk(s_song, i);
    if (prv_chunk_due_ms(chunk) >= elapsed_ms) {
      index = i;
      break;
    }
  }
  if (index >= count) {
    return;  // past the end of the music
  }

  prv_arm(index);
  speaker_set_finish_callback(prv_finished, NULL);
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
//
// It is also where every speaker call is made from -- see the note on
// prv_finished().
//
// There is no watchdog here any more, and its absence is the point. The old one
// existed because the whole handover chain hung off a single finish callback, so
// anything that swallowed that callback stopped the music permanently and
// silently. Nothing hangs off the callback now: the next release is already
// scheduled against the song clock before the current chunk starts sounding, so
// a chunk that never reports finishing costs nothing at all. A watchdog that
// forced a handover would now be the only thing capable of double-starting one.
void audio_tick(uint32_t elapsed_ms) {
  if (s_finished) {
    prv_handle_finished(elapsed_ms);
  }
  if (!s_armed || elapsed_ms < s_release_at_ms) {
    return;
  }
  prv_release(elapsed_ms);
}

void audio_song_stop(void) {
  s_armed = false;
  if (!s_playing) {
    return;
  }
  // Cleared BEFORE stopping: speaker_stop() fires the finish callback, and
  // prv_finished() checks this flag precisely so a stop on the way out is not
  // mistaken for something worth reporting.
  s_playing = false;
  speaker_set_finish_callback(NULL, NULL);
  speaker_stop();
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
