#include "chart.h"

// ---------------------------------------------------------------------------
// GENERATED FILE -- do not hand-edit. Regenerate with tools/make_chart.py,
// which also re-cuts watch/resources/data/music.pcm from the same excerpt so the
// chart and the audio cannot drift apart.
//
// "Prelude Drive" -- charted from the audio itself, not hand-placed.
//
// Source: a remix of Chopin's Prelude Op. 28 No. 4. The stage is a 58.7s
// excerpt from 69.525s to 128.192s of the original -- 22 bars at
// 90 BPM, bar-aligned, taken from the track's sustained high-energy section.
//
// The length is set by audio quality, not by musical taste: the excerpt is
// stored as raw 16kHz 8-bit PCM at 16000 bytes/sec, so ~59s is what fits inside
// emery's 1024KB resource budget. See tools/make_chart.py.
//
// Pipeline: STFT (11.6ms hop) -> positive spectral flux per frequency band,
// averaged PER BIN and z-scaled -> peak-pick against a moving-average threshold
// -> tempo by comb-filtering the onset envelope -> snap to eighths.
//
// The per-bin normalisation is load-bearing: the treble band has 456 FFT bins
// against the bass band's 10, so comparing raw summed flux reports "treble" for
// almost every onset.
//
// Lanes follow the dominant band, which is why the chart reads the way the music
// sounds: bass and kick on BOTTOM, piano body on MIDDLE, melody on TOP. The drop
// threshold is applied per band rather than globally, otherwise the loudest band
// crowds the others out and one lane goes nearly unused.
//
// 97 notes, 1.65/s. Spacing guarantees: no two notes in the SAME
// lane within 333ms (above 2*RB_MISS_MS, so judgment windows can never
// overlap -- tools/run_tests.sh asserts this), and no two notes at all within
// 300ms, which caps how dense a loud bar can get.
// ---------------------------------------------------------------------------

static const ChartNote s_demo_notes[] = {
  {   2333, RB_LANE_MID , RB_NOTE_NORMAL },
  {   2667, RB_LANE_TOP , RB_NOTE_NORMAL },
  {   3000, RB_LANE_BOT , RB_NOTE_NORMAL },
  {   3667, RB_LANE_BOT , RB_NOTE_NORMAL },
  {   4000, RB_LANE_TOP , RB_NOTE_BIG    },
  {   4333, RB_LANE_MID , RB_NOTE_NORMAL },
  {   5333, RB_LANE_TOP , RB_NOTE_BIG    },
  {   6000, RB_LANE_MID , RB_NOTE_NORMAL },
  {   6333, RB_LANE_BOT , RB_NOTE_NORMAL },
  {   7000, RB_LANE_BOT , RB_NOTE_NORMAL },
  {   7333, RB_LANE_MID , RB_NOTE_NORMAL },
  {   8000, RB_LANE_MID , RB_NOTE_NORMAL },
  {   8667, RB_LANE_MID , RB_NOTE_NORMAL },
  {   9333, RB_LANE_TOP , RB_NOTE_NORMAL },
  {   9667, RB_LANE_MID , RB_NOTE_NORMAL },
  {  10000, RB_LANE_MID , RB_NOTE_NORMAL },
  {  11667, RB_LANE_MID , RB_NOTE_NORMAL },
  {  12000, RB_LANE_TOP , RB_NOTE_NORMAL },
  {  12667, RB_LANE_MID , RB_NOTE_NORMAL },
  {  13000, RB_LANE_MID , RB_NOTE_NORMAL },
  {  14667, RB_LANE_TOP , RB_NOTE_NORMAL },
  {  16000, RB_LANE_MID , RB_NOTE_NORMAL },
  {  16333, RB_LANE_BOT , RB_NOTE_NORMAL },
  {  16667, RB_LANE_MID , RB_NOTE_NORMAL },
  {  17667, RB_LANE_TOP , RB_NOTE_NORMAL },
  {  18000, RB_LANE_MID , RB_NOTE_NORMAL },
  {  20000, RB_LANE_TOP , RB_NOTE_BIG    },
  {  21333, RB_LANE_TOP , RB_NOTE_NORMAL },
  {  21667, RB_LANE_BOT , RB_NOTE_NORMAL },
  {  22667, RB_LANE_TOP , RB_NOTE_NORMAL },
  {  25333, RB_LANE_TOP , RB_NOTE_BIG    },
  {  25667, RB_LANE_BOT , RB_NOTE_NORMAL },
  {  26000, RB_LANE_MID , RB_NOTE_NORMAL },
  {  26667, RB_LANE_TOP , RB_NOTE_BIG    },
  {  27333, RB_LANE_MID , RB_NOTE_NORMAL },
  {  27667, RB_LANE_BOT , RB_NOTE_NORMAL },
  {  28000, RB_LANE_TOP , RB_NOTE_BIG    },
  {  28333, RB_LANE_MID , RB_NOTE_NORMAL },
  {  28667, RB_LANE_MID , RB_NOTE_NORMAL },
  {  29333, RB_LANE_TOP , RB_NOTE_NORMAL },
  {  30000, RB_LANE_MID , RB_NOTE_NORMAL },
  {  30333, RB_LANE_MID , RB_NOTE_NORMAL },
  {  30667, RB_LANE_TOP , RB_NOTE_NORMAL },
  {  31333, RB_LANE_BOT , RB_NOTE_NORMAL },
  {  32000, RB_LANE_TOP , RB_NOTE_BIG    },
  {  32333, RB_LANE_MID , RB_NOTE_NORMAL },
  {  32667, RB_LANE_MID , RB_NOTE_NORMAL },
  {  33333, RB_LANE_TOP , RB_NOTE_NORMAL },
  {  33667, RB_LANE_MID , RB_NOTE_NORMAL },
  {  34333, RB_LANE_BOT , RB_NOTE_NORMAL },
  {  34667, RB_LANE_MID , RB_NOTE_NORMAL },
  {  36000, RB_LANE_TOP , RB_NOTE_NORMAL },
  {  36333, RB_LANE_MID , RB_NOTE_NORMAL },
  {  36667, RB_LANE_MID , RB_NOTE_NORMAL },
  {  37000, RB_LANE_BOT , RB_NOTE_NORMAL },
  {  37333, RB_LANE_TOP , RB_NOTE_NORMAL },
  {  37667, RB_LANE_MID , RB_NOTE_NORMAL },
  {  38000, RB_LANE_MID , RB_NOTE_NORMAL },
  {  38667, RB_LANE_TOP , RB_NOTE_NORMAL },
  {  39000, RB_LANE_TOP , RB_NOTE_NORMAL },
  {  39333, RB_LANE_MID , RB_NOTE_NORMAL },
  {  40000, RB_LANE_TOP , RB_NOTE_NORMAL },
  {  40667, RB_LANE_BOT , RB_NOTE_NORMAL },
  {  41333, RB_LANE_TOP , RB_NOTE_BIG    },
  {  41667, RB_LANE_BOT , RB_NOTE_NORMAL },
  {  42000, RB_LANE_BOT , RB_NOTE_NORMAL },
  {  42667, RB_LANE_TOP , RB_NOTE_NORMAL },
  {  43333, RB_LANE_BOT , RB_NOTE_NORMAL },
  {  44000, RB_LANE_MID , RB_NOTE_NORMAL },
  {  44333, RB_LANE_BOT , RB_NOTE_NORMAL },
  {  44667, RB_LANE_MID , RB_NOTE_NORMAL },
  {  45333, RB_LANE_TOP , RB_NOTE_NORMAL },
  {  46000, RB_LANE_TOP , RB_NOTE_NORMAL },
  {  46667, RB_LANE_MID , RB_NOTE_BIG    },
  {  47000, RB_LANE_BOT , RB_NOTE_NORMAL },
  {  47333, RB_LANE_MID , RB_NOTE_NORMAL },
  {  48000, RB_LANE_TOP , RB_NOTE_NORMAL },
  {  48333, RB_LANE_BOT , RB_NOTE_NORMAL },
  {  48667, RB_LANE_MID , RB_NOTE_NORMAL },
  {  49333, RB_LANE_MID , RB_NOTE_NORMAL },
  {  49667, RB_LANE_BOT , RB_NOTE_NORMAL },
  {  50000, RB_LANE_BOT , RB_NOTE_NORMAL },
  {  50333, RB_LANE_BOT , RB_NOTE_NORMAL },
  {  50667, RB_LANE_TOP , RB_NOTE_NORMAL },
  {  51000, RB_LANE_MID , RB_NOTE_NORMAL },
  {  51333, RB_LANE_BOT , RB_NOTE_NORMAL },
  {  52000, RB_LANE_TOP , RB_NOTE_NORMAL },
  {  53000, RB_LANE_BOT , RB_NOTE_NORMAL },
  {  53333, RB_LANE_TOP , RB_NOTE_NORMAL },
  {  54000, RB_LANE_MID , RB_NOTE_NORMAL },
  {  54333, RB_LANE_BOT , RB_NOTE_NORMAL },
  {  54667, RB_LANE_TOP , RB_NOTE_BIG    },
  {  55000, RB_LANE_BOT , RB_NOTE_NORMAL },
  {  57000, RB_LANE_MID , RB_NOTE_NORMAL },
  {  57333, RB_LANE_MID , RB_NOTE_NORMAL },
  {  58000, RB_LANE_MID , RB_NOTE_NORMAL },
  {  60000, RB_LANE_MID , RB_NOTE_BIG    },
};

// The music resource begins at RB_MUSIC_START_MS, the same offset the first note
// sits at, so the audio and the chart share one origin.
static const Chart s_demo_chart = {
  .title = "Prelude Drive",
  .notes = s_demo_notes,
  .note_count = (uint16_t)(sizeof(s_demo_notes) / sizeof(s_demo_notes[0])),
  .bpm = 90,
  .lead_in_ms = 2000,
  .end_ms = 63166,
};

const Chart *chart_get_builtin(void) {
  return &s_demo_chart;
}

bool chart_load_from_resource(uint32_t resource_id, Chart *out_chart) {
  // Not implemented in v1 -- see the format documented in chart.h. Callers are
  // expected to fall back to chart_get_builtin().
  (void)resource_id;
  (void)out_chart;
  return false;
}
