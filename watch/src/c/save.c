#include "save.h"

#include "chart.h"
#include "rb_config.h"

// One record per song. A single shared best across two songs would be
// meaningless -- the songs are different lengths and different densities, so
// their scores are not comparable and one would permanently mask the other.
static uint32_t s_high_score[RB_MAX_SONGS];
static uint16_t s_best_combo[RB_MAX_SONGS];

// Keys are allocated per song from a base, so adding a song needs no new
// constants. RB_MAX_SONGS bounds the range that can ever be claimed.
static uint32_t prv_score_key(uint8_t song) {
  return RB_PERSIST_KEY_HIGH_SCORE_BASE + song;
}

static uint32_t prv_combo_key(uint8_t song) {
  return RB_PERSIST_KEY_BEST_COMBO_BASE + song;
}

void save_load(void) {
  const bool current =
      persist_exists(RB_PERSIST_KEY_VERSION) &&
      persist_read_int(RB_PERSIST_KEY_VERSION) == RB_SAVE_VERSION;

  if (!current) {
    // Either a first run or a schema we do not understand. Starting clean is
    // safe here: the only stored state is per-song bests.
    //
    // Every key this app reads is written here, explicitly, even though the
    // zeroed statics above already hold. Do NOT rely on persist_exists() to
    // report an unwritten key as absent -- on emery it returns true for keys
    // this app has never touched, and persist_read_int() then hands back a
    // stale value. That is exactly how the old sound toggle ended up silently
    // disabled: its flag key read back as 1, which the encoding then in use
    // meant "off". The toggles are gone, but the hazard is not.
    for (uint8_t i = 0; i < RB_MAX_SONGS; i++) {
      s_high_score[i] = 0;
      s_best_combo[i] = 0;
      persist_write_int(prv_score_key(i), 0);
      persist_write_int(prv_combo_key(i), 0);
    }
    persist_write_int(RB_PERSIST_KEY_VERSION, RB_SAVE_VERSION);
    return;
  }

  for (uint8_t i = 0; i < RB_MAX_SONGS; i++) {
    s_high_score[i] = persist_exists(prv_score_key(i))
                          ? (uint32_t)persist_read_int(prv_score_key(i))
                          : 0;
    s_best_combo[i] = persist_exists(prv_combo_key(i))
                          ? (uint16_t)persist_read_int(prv_combo_key(i))
                          : 0;
  }
}

uint32_t save_high_score(uint8_t song) {
  return (song < RB_MAX_SONGS) ? s_high_score[song] : 0;
}

uint16_t save_best_combo(uint8_t song) {
  return (song < RB_MAX_SONGS) ? s_best_combo[song] : 0;
}

bool save_record(uint8_t song, uint32_t score, uint16_t max_combo) {
  if (song >= RB_MAX_SONGS) {
    return false;
  }

  const bool new_score = score > s_high_score[song];
  if (new_score) {
    s_high_score[song] = score;
    persist_write_int(prv_score_key(song), (int32_t)score);
  }
  if (max_combo > s_best_combo[song]) {
    s_best_combo[song] = max_combo;
    persist_write_int(prv_combo_key(song), (int32_t)max_combo);
  }
  return new_score;
}
