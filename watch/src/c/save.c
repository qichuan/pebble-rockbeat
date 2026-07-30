#include "save.h"

#include "rb_config.h"

static uint32_t s_high_score;
static uint16_t s_best_combo;
static bool s_sound = true;
static bool s_haptics = true;

// Booleans are stored as value+1, because persist_read_int() cannot distinguish
// a stored 0 from a missing key. 0 therefore means "never set", and the
// defaults above stand.
static bool prv_read_flag(uint32_t key, bool fallback) {
  if (!persist_exists(key)) {
    return fallback;
  }
  const int32_t raw = persist_read_int(key);
  return (raw == 0) ? fallback : (raw == 2);
}

static void prv_write_flag(uint32_t key, bool value) {
  persist_write_int(key, value ? 2 : 1);
}

void save_load(void) {
  const bool current =
      persist_exists(RB_PERSIST_KEY_VERSION) &&
      persist_read_int(RB_PERSIST_KEY_VERSION) == RB_SAVE_VERSION;

  if (!current) {
    // Either a first run or a schema we do not understand. Starting clean is
    // safe here: the only stored state is a high score and two toggles.
    s_high_score = 0;
    s_best_combo = 0;
    s_sound = true;
    s_haptics = true;

    // Every key this app reads is written here, explicitly, even though the
    // defaults above already hold. Do NOT rely on persist_exists() to report an
    // unwritten key as absent -- on emery it returns true for keys this app has
    // never touched, and persist_read_int() then hands back a stale value. That
    // is exactly how the sound toggle ended up silently disabled: the flag key
    // read back as 1, which the encoding below means "off".
    persist_write_int(RB_PERSIST_KEY_HIGH_SCORE, 0);
    persist_write_int(RB_PERSIST_KEY_BEST_COMBO, 0);
    prv_write_flag(RB_PERSIST_KEY_SOUND, true);
    prv_write_flag(RB_PERSIST_KEY_HAPTICS, true);
    persist_write_int(RB_PERSIST_KEY_VERSION, RB_SAVE_VERSION);
    return;
  }

  s_high_score = persist_exists(RB_PERSIST_KEY_HIGH_SCORE)
                     ? (uint32_t)persist_read_int(RB_PERSIST_KEY_HIGH_SCORE)
                     : 0;
  s_best_combo = persist_exists(RB_PERSIST_KEY_BEST_COMBO)
                     ? (uint16_t)persist_read_int(RB_PERSIST_KEY_BEST_COMBO)
                     : 0;
  s_sound = prv_read_flag(RB_PERSIST_KEY_SOUND, true);
  s_haptics = prv_read_flag(RB_PERSIST_KEY_HAPTICS, true);
}

uint32_t save_high_score(void) {
  return s_high_score;
}

uint16_t save_best_combo(void) {
  return s_best_combo;
}

bool save_record(uint32_t score, uint16_t max_combo) {
  const bool new_score = score > s_high_score;

  if (new_score) {
    s_high_score = score;
    persist_write_int(RB_PERSIST_KEY_HIGH_SCORE, (int32_t)score);
  }
  if (max_combo > s_best_combo) {
    s_best_combo = max_combo;
    persist_write_int(RB_PERSIST_KEY_BEST_COMBO, (int32_t)max_combo);
  }
  return new_score;
}

bool save_sound_enabled(void) {
  return s_sound;
}

bool save_haptics_enabled(void) {
  return s_haptics;
}

void save_set_sound(bool enabled) {
  s_sound = enabled;
  prv_write_flag(RB_PERSIST_KEY_SOUND, enabled);
}

void save_set_haptics(bool enabled) {
  s_haptics = enabled;
  prv_write_flag(RB_PERSIST_KEY_HAPTICS, enabled);
}
