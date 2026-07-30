#include "audio.h"

#include "clock.h"
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
#ifdef _PBL_API_EXISTS_speaker_stream_open

// ---------------------------------------------------------------------------
// One cycle of a sine, 256 entries. int8 gives ~48dB, far past what a watch
// speaker resolves, and keeps the per-sample maths to one multiply and a shift.
// 256 bytes of flash.
// ---------------------------------------------------------------------------

static const int8_t k_sine[256] = {
     0,    3,    6,    9,   12,   16,   19,   22,   25,   28,   31,   34,   37,   40,   43,   46,
    49,   51,   54,   57,   60,   63,   65,   68,   71,   73,   76,   78,   81,   83,   85,   88,
    90,   92,   94,   96,   98,  100,  102,  104,  106,  107,  109,  111,  112,  113,  115,  116,
   117,  118,  120,  121,  122,  122,  123,  124,  125,  125,  126,  126,  126,  127,  127,  127,
   127,  127,  127,  127,  126,  126,  126,  125,  125,  124,  123,  122,  122,  121,  120,  118,
   117,  116,  115,  113,  112,  111,  109,  107,  106,  104,  102,  100,   98,   96,   94,   92,
    90,   88,   85,   83,   81,   78,   76,   73,   71,   68,   65,   63,   60,   57,   54,   51,
    49,   46,   43,   40,   37,   34,   31,   28,   25,   22,   19,   16,   12,    9,    6,    3,
     0,   -3,   -6,   -9,  -12,  -16,  -19,  -22,  -25,  -28,  -31,  -34,  -37,  -40,  -43,  -46,
   -49,  -51,  -54,  -57,  -60,  -63,  -65,  -68,  -71,  -73,  -76,  -78,  -81,  -83,  -85,  -88,
   -90,  -92,  -94,  -96,  -98, -100, -102, -104, -106, -107, -109, -111, -112, -113, -115, -116,
  -117, -118, -120, -121, -122, -122, -123, -124, -125, -125, -126, -126, -126, -127, -127, -127,
  -127, -127, -127, -127, -126, -126, -126, -125, -125, -124, -123, -122, -122, -121, -120, -118,
  -117, -116, -115, -113, -112, -111, -109, -107, -106, -104, -102, -100,  -98,  -96,  -94,  -92,
   -90,  -88,  -85,  -83,  -81,  -78,  -76,  -73,  -71,  -68,  -65,  -63,  -60,  -57,  -54,  -51,
   -49,  -46,  -43,  -40,  -37,  -34,  -31,  -28,  -25,  -22,  -19,  -16,  -12,   -9,   -6,   -3,
};

// ---------------------------------------------------------------------------
// Voices
// ---------------------------------------------------------------------------

typedef struct {
  bool active;
  uint32_t phase;
  uint32_t phase_inc;
  uint32_t phase2;
  uint32_t phase2_inc;
  uint16_t env;        // 8.8 fixed point; the high byte is the 0..255 amplitude
  uint16_t noise_env;  // 8.8; zero for a don
  uint8_t env_shift;
  uint8_t noise_shift;
  uint16_t samples_left;
} RbVoice;

static RbVoice s_voices[RB_AUDIO_VOICES];
static int8_t s_pcm[RB_AUDIO_CHUNK_SAMPLES];
static uint16_t s_lfsr = 0xACE1u;

static bool s_open;
static bool s_enabled = true;
static bool s_suppressed;
static uint32_t s_open_ms;
static uint32_t s_samples_written;

// ---------------------------------------------------------------------------
// Backing music: raw signed 8-bit 8kHz PCM, streamed from a resource in
// chunks. Only one frame's worth is ever in RAM -- the resource itself is
// 688KB, far past what could be loaded whole into a 128KB app.
// ---------------------------------------------------------------------------

// The music is stored at the same rate as the stream, so one source byte maps to
// exactly one output sample -- no resampling, no interpolation.
#define MUSIC_BYTES_MAX RB_AUDIO_CHUNK_SAMPLES
#define MUSIC_START_SAMPLE ((uint32_t)RB_MUSIC_START_MS * (RB_AUDIO_RATE_HZ / 1000))

static ResHandle s_music;
static uint32_t s_music_bytes;
static int8_t s_music_buf[MUSIC_BYTES_MAX];

static AppTimer *s_pump_timer;

// ---------------------------------------------------------------------------
// Voice allocation -- O(1)-ish and allocation-free, called straight from the
// button handler.
// ---------------------------------------------------------------------------

static RbVoice *prv_claim(void) {
  for (uint8_t i = 0; i < RB_AUDIO_VOICES; i++) {
    if (!s_voices[i].active) {
      return &s_voices[i];
    }
  }
  // All busy: steal the quietest, since its disappearance is least audible.
  RbVoice *victim = &s_voices[0];
  for (uint8_t i = 1; i < RB_AUDIO_VOICES; i++) {
    if (s_voices[i].env < victim->env) {
      victim = &s_voices[i];
    }
  }
  return victim;
}

static void prv_trigger(uint16_t f1, uint16_t f2, uint8_t env_shift, uint16_t duration_ms,
                        uint8_t amplitude, bool with_noise) {
  RbVoice *const v = prv_claim();

  v->phase = 0;
  v->phase2 = 0;
  v->phase_inc = RB_PHASE_INC(f1);
  v->phase2_inc = RB_PHASE_INC(f2);
  v->env = (uint16_t)((uint16_t)amplitude << 8);
  v->env_shift = env_shift;
  v->noise_env = with_noise ? v->env : 0;
  v->noise_shift = RB_KA_NOISE_SHIFT;
  v->samples_left = (uint16_t)(((uint32_t)duration_ms * RB_AUDIO_RATE_HZ) / 1000u);
  v->active = true;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

static int8_t prv_noise(void) {
  // 16-bit xorshift LFSR -- one shift-xor triple, no table, no division.
  s_lfsr ^= (uint16_t)(s_lfsr << 13);
  s_lfsr ^= (uint16_t)(s_lfsr >> 9);
  s_lfsr ^= (uint16_t)(s_lfsr << 7);
  return (int8_t)(s_lfsr & 0xFF);
}

// Mixes the backing track into s_pcm for a chunk of `count` stream samples
// starting at stream position `start`.
//
// The music position is derived from the STREAM position rather than from a
// counter of its own. That is the whole sync story: a sample written at stream
// position P is heard exactly P/16000 seconds after the stream opened, so
// placing the music sample for time T at position T*16000 makes it heard at T.
// The notes come off the same clock, so picture and music cannot drift apart --
// and the dependency still runs one way only, audio following the clock.
static void prv_mix_music(uint32_t start, uint32_t count) {
  if (s_music == NULL) {
    return;
  }

  // Nothing to do while the stream is still inside the pre-roll.
  const uint32_t end = start + count;
  if (end <= MUSIC_START_SAMPLE) {
    return;
  }
  const uint32_t first = (start > MUSIC_START_SAMPLE) ? start : MUSIC_START_SAMPLE;

  const uint32_t src_first = first - MUSIC_START_SAMPLE;
  const uint32_t src_last = end - 1 - MUSIC_START_SAMPLE;
  if (src_first >= s_music_bytes) {
    return;  // the excerpt has run out
  }

  uint32_t need = src_last - src_first + 1;
  if (src_first + need > s_music_bytes) {
    need = s_music_bytes - src_first;
  }
  if (need > MUSIC_BYTES_MAX) {
    need = MUSIC_BYTES_MAX;
  }
  const uint32_t got = resource_load_byte_range(s_music, src_first, (uint8_t *)s_music_buf, need);
  if (got == 0) {
    return;
  }

  for (uint32_t i = 0; i < count; i++) {
    const uint32_t pos = start + i;
    if (pos < MUSIC_START_SAMPLE) {
      continue;
    }
    const uint32_t idx = (pos - MUSIC_START_SAMPLE) - src_first;
    if (idx >= got) {
      break;
    }

    int32_t v = (int32_t)s_pcm[i] + ((int32_t)s_music_buf[idx] >> RB_MUSIC_GAIN_SHIFT);
    if (v > 127) {
      v = 127;
    } else if (v < -127) {
      v = -127;
    }
    s_pcm[i] = (int8_t)v;
  }
}

static void prv_render(uint32_t count) {
  for (uint32_t i = 0; i < count; i++) {
    int32_t acc = 0;

    for (uint8_t vi = 0; vi < RB_AUDIO_VOICES; vi++) {
      RbVoice *const v = &s_voices[vi];
      if (!v->active) {
        continue;
      }

      const uint8_t amp = (uint8_t)(v->env >> 8);
      // Shifts chosen so a single hit peaks near full scale without clipping:
      // a don reaches ~94 of 127, a ka ~125. Stacked voices saturate gently,
      // which on a drum reads as loudness rather than as distortion.
      acc += ((int32_t)k_sine[(v->phase >> 24) & 0xFF] * amp) >> 9;
      acc += ((int32_t)k_sine[(v->phase2 >> 24) & 0xFF] * amp) >> 10;
      if (v->noise_env != 0) {
        acc += ((int32_t)prv_noise() * (int32_t)(v->noise_env >> 8)) >> 10;
        v->noise_env = (uint16_t)(v->noise_env - (v->noise_env >> v->noise_shift));
      }

      v->phase += v->phase_inc;
      v->phase2 += v->phase2_inc;
      v->env = (uint16_t)(v->env - (v->env >> v->env_shift));

      // Retire on either bound: the envelope going inaudible, or the note's
      // nominal length running out.
      if (v->samples_left == 0 || v->env < 0x0100) {
        v->active = false;
      } else {
        v->samples_left--;
      }
    }

    if (acc > 127) {
      acc = 127;
    } else if (acc < -127) {
      acc = -127;
    }
    s_pcm[i] = (int8_t)acc;
  }
}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

// Self-rescheduling pump timer. Runs independently of the render frame so the
// stream is fed on its own cadence -- see RB_AUDIO_PUMP_MS for why that is
// required and not merely tidier.
static void prv_pump_timer(void *data) {
  (void)data;
  s_pump_timer = NULL;
  audio_pump(clock_now_ms());
  if (s_open) {
    s_pump_timer = app_timer_register(RB_AUDIO_PUMP_MS, prv_pump_timer, NULL);
  }
}

static void prv_pump_schedule(void) {
  if (s_pump_timer == NULL) {
    s_pump_timer = app_timer_register(RB_AUDIO_PUMP_MS, prv_pump_timer, NULL);
  }
}

void audio_init(void) {
  for (uint8_t i = 0; i < RB_AUDIO_VOICES; i++) {
    s_voices[i].active = false;
  }
  s_open = false;
  s_samples_written = 0;

  // Just a handle plus a length -- the 688KB of PCM stays in flash and is read
  // a frame at a time.
  s_music = resource_get_handle(RESOURCE_ID_MUSIC_PCM);
  s_music_bytes = (s_music != NULL) ? (uint32_t)resource_size(s_music) : 0;
#if RB_DEBUG_LOG_AUDIO
  APP_LOG(APP_LOG_LEVEL_DEBUG, "music resource: %lu bytes (%lus)",
          (unsigned long)s_music_bytes, (unsigned long)(s_music_bytes / RB_MUSIC_RATE_HZ));
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

void audio_song_start(void) {
#if RB_DEBUG_LOG_AUDIO
  APP_LOG(APP_LOG_LEVEL_DEBUG, "audio_song_start: open=%d enabled=%d", (int)s_open,
          (int)s_enabled);
#endif
  if (s_open || !s_enabled) {
    return;
  }

  // Checked once per song rather than per frame. An app cannot override the
  // system mute, so when it is set there is no point synthesising anything --
  // this saves the CPU as well as being correct.
  const bool muted = speaker_is_muted();
  const bool quiet = quiet_time_is_active();
  s_suppressed = muted || quiet;
#if RB_DEBUG_LOG_AUDIO
  APP_LOG(APP_LOG_LEVEL_DEBUG, "audio start: muted=%d quiet=%d", (int)muted, (int)quiet);
#endif
  if (s_suppressed) {
    return;
  }

  const bool opened = speaker_stream_open(SpeakerPcmFormat_16kHz_8bit, RB_AUDIO_VOLUME);
#if RB_DEBUG_LOG_AUDIO
  APP_LOG(APP_LOG_LEVEL_DEBUG, "speaker_stream_open -> %d", (int)opened);
#endif
  if (!opened) {
    return;
  }

  s_open = true;
  s_samples_written = 0;
  s_open_ms = 0;
  prv_pump_schedule();

  // Prime with silence so the very first hit cannot underrun.
  const uint32_t prime = (RB_AUDIO_PRIME_MS * RB_AUDIO_RATE_HZ) / 1000u;
  for (uint32_t i = 0; i < prime && i < RB_AUDIO_CHUNK_SAMPLES; i++) {
    s_pcm[i] = 0;
  }
  const uint32_t n = (prime < RB_AUDIO_CHUNK_SAMPLES) ? prime : RB_AUDIO_CHUNK_SAMPLES;
  s_samples_written += speaker_stream_write(s_pcm, n);
}

void audio_song_stop(void) {
  if (s_pump_timer != NULL) {
    app_timer_cancel(s_pump_timer);
    s_pump_timer = NULL;
  }
  if (!s_open) {
    return;
  }
  speaker_stream_close();  // drains, so the final hit is still heard
  s_open = false;
  s_samples_written = 0;
  for (uint8_t i = 0; i < RB_AUDIO_VOICES; i++) {
    s_voices[i].active = false;
  }
}

void audio_play_lane(uint8_t lane, uint8_t note_type) {
  if (!s_open || !s_enabled || s_suppressed) {
    return;
  }

  const bool big = (note_type == RB_NOTE_BIG);
  const uint8_t amp = big ? RB_ENV_BIG : RB_ENV_NORMAL;

  if (lane == RB_LANE_MID) {
    if (big) {
      prv_trigger(RB_BIG_DON_F1, RB_BIG_DON_F2, RB_BIG_DON_ENV_SHIFT, RB_BIG_DON_MS, amp, false);
    } else {
      prv_trigger(RB_DON_F1, RB_DON_F2, RB_DON_ENV_SHIFT, RB_DON_MS, amp, false);
    }
  } else {
    if (big) {
      prv_trigger(RB_BIG_KA_F1, RB_BIG_KA_F2, RB_KA_ENV_SHIFT, RB_BIG_KA_MS, amp, true);
    } else {
      prv_trigger(RB_KA_F1, RB_KA_F2, RB_KA_ENV_SHIFT, RB_KA_MS, amp, true);
    }
  }
}

void audio_pump(uint32_t now_ms) {
  if (!s_open || !s_enabled || s_suppressed) {
    return;
  }

  if (s_open_ms == 0) {
    s_open_ms = now_ms;
  }

  // Keep the stream filled to RB_AUDIO_LEAD_MS ahead of the clock and no
  // further. Whatever sits queued in the firmware buffer IS the delay between a
  // press and its sound, so the target is a shallow-but-never-empty buffer.
  // Rendering a fixed chunk every frame would instead drive the buffer to its
  // ceiling and add however deep that ceiling happens to be -- a number the SDK
  // does not document.
  //
  // Overflow: (elapsed + lead) * 16000 stays inside uint32 for ~268s of
  // continuous playback, comfortably beyond the 35s demo song, and the counter
  // is reset on every song start.
  const uint32_t target =
      ((now_ms - s_open_ms + RB_AUDIO_LEAD_MS) * (uint32_t)RB_AUDIO_RATE_HZ) / 1000u;
  if (target <= s_samples_written) {
    return;
  }

  uint32_t remaining = target - s_samples_written;
  const uint32_t cap = (uint32_t)RB_AUDIO_WRITE_MAX * RB_AUDIO_SLICES_PER_PUMP;
  if (remaining > cap) {
    remaining = cap;
  }

  // Bounded slice loop -- at most RB_AUDIO_SLICES_PER_PUMP iterations, never a
  // spin. The write cap is 512 samples per call (measured), while a frame needs
  // 528, so a single slice would permanently starve the stream.
  for (uint8_t slice = 0; slice < RB_AUDIO_SLICES_PER_PUMP && remaining > 0; slice++) {
    uint32_t want = remaining;
    if (want > RB_AUDIO_WRITE_MAX) {
      want = RB_AUDIO_WRITE_MAX;
    }

    prv_render(want);
    prv_mix_music(s_samples_written, want);
    const uint32_t accepted = speaker_stream_write(s_pcm, want);

    // BACKPRESSURE: a short write means the buffer is full and those samples
    // are gone. Advance by `want`, NOT by `accepted`.
    //
    // This counter decides which part of the song the next chunk contains, so
    // it has to stay tied to the game clock. Advancing by `accepted` would
    // re-send the dropped region next pump, pushing the music permanently
    // further behind the notes -- a rhythm game that slowly desynchronises is
    // worse than one with an occasional few ms of silence.
    s_samples_written += want;
    remaining -= want;

#if RB_DEBUG_LOG_AUDIO
    if (accepted != want) {
      APP_LOG(APP_LOG_LEVEL_DEBUG, "pcm backpressure: want=%lu accepted=%lu dropped=%lu",
              (unsigned long)want, (unsigned long)accepted, (unsigned long)(want - accepted));
    }
#endif
  }
}

#else  // No speaker API in this SDK -- everything degrades to a no-op.

void audio_init(void) {}
bool audio_is_available(void) { return false; }
void audio_set_enabled(bool enabled) { (void)enabled; }
bool audio_is_enabled(void) { return false; }
void audio_song_start(void) {}
void audio_song_stop(void) {}
void audio_play_lane(uint8_t lane, uint8_t note_type) { (void)lane; (void)note_type; }
void audio_pump(uint32_t now_ms) { (void)now_ms; }

#endif  // _PBL_API_EXISTS_speaker_stream_open
