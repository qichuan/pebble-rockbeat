#include "audio.h"

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

void audio_init(void) {
  for (uint8_t i = 0; i < RB_AUDIO_VOICES; i++) {
    s_voices[i].active = false;
  }
  s_open = false;
  s_samples_written = 0;
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

  // Prime with silence so the very first hit cannot underrun.
  const uint32_t prime = (RB_AUDIO_PRIME_MS * RB_AUDIO_RATE_HZ) / 1000u;
  for (uint32_t i = 0; i < prime && i < RB_AUDIO_CHUNK_SAMPLES; i++) {
    s_pcm[i] = 0;
  }
  const uint32_t n = (prime < RB_AUDIO_CHUNK_SAMPLES) ? prime : RB_AUDIO_CHUNK_SAMPLES;
  s_samples_written += speaker_stream_write(s_pcm, n);
}

void audio_song_stop(void) {
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

  uint32_t want = target - s_samples_written;
  if (want > RB_AUDIO_CHUNK_SAMPLES) {
    want = RB_AUDIO_CHUNK_SAMPLES;
  }

  prv_render(want);
  const uint32_t accepted = speaker_stream_write(s_pcm, want);
  s_samples_written += accepted;

#if RB_DEBUG_LOG_AUDIO
  if (accepted != want) {
    APP_LOG(APP_LOG_LEVEL_DEBUG, "pcm backpressure: want=%lu accepted=%lu",
            (unsigned long)want, (unsigned long)accepted);
  }
#endif

  // BACKPRESSURE: a short write means the firmware buffer is full. Drop the
  // remainder -- never retry, never carry it forward. Retrying would block the
  // frame; carrying it would push already-stale audio at a steadily growing
  // delay behind the game, and in a rhythm game a hit heard 200ms late is worse
  // than one not heard at all. `want` grows by exactly what was dropped on the
  // next pump and is capped by CHUNK, so the stream self-corrects.
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
