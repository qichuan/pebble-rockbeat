#include <pebble.h>

#include "audio.h"
#include "chart.h"
#include "clock.h"
#include "feedback.h"
#include "game.h"
#include "input.h"
#include "rb_config.h"
#include "render.h"
#include "save.h"

static Window *s_window;
static Layer *s_canvas;
static AppTimer *s_timer;
static const Chart *s_chart;

// Which song the title screen has selected. Everything downstream -- the chart,
// the music, the high score -- is indexed by this one value, so they cannot get
// out of step with each other.
static uint8_t s_song;

// ---------------------------------------------------------------------------
// Frame timer -- runs only while a song is actually playing. Title, pause and
// results are static, so an idle 30Hz wakeup would be pure battery cost.
// ---------------------------------------------------------------------------

static void prv_frame(void *data);

static void prv_timer_start(void) {
  if (s_timer == NULL) {
    s_timer = app_timer_register(RB_FRAME_MS, prv_frame, NULL);
  }
}

static void prv_timer_stop(void) {
  if (s_timer != NULL) {
    app_timer_cancel(s_timer);
    s_timer = NULL;
  }
}

// ---------------------------------------------------------------------------
// Screen transitions
// ---------------------------------------------------------------------------

static void prv_enter_title(void) {
  game_set_screen(RB_SCREEN_TITLE);
  prv_timer_stop();
  audio_song_stop();
  feedback_silence();
  layer_mark_dirty(s_canvas);
}

static void prv_start_song(void) {
  feedback_reset();
  s_chart = chart_get(s_song);
  game_start(s_chart, clock_now_ms());
  game_set_screen(RB_SCREEN_PLAYING);
  audio_song_start(s_song, game_elapsed_ms());
  prv_timer_start();
  layer_mark_dirty(s_canvas);
}

static void prv_enter_results(void) {
  game_set_screen(RB_SCREEN_RESULTS);
  prv_timer_stop();
  audio_song_stop();
  feedback_silence();
  // The only persist write in the app, and deliberately not in the frame loop:
  // a flash write mid-song could stall a frame.
  save_record(s_song, game_score(), game_max_combo());
  layer_mark_dirty(s_canvas);
}

static void prv_pause(void) {
  game_pause(clock_now_ms());
  game_set_screen(RB_SCREEN_PAUSED);
  prv_timer_stop();
  audio_song_stop();
  feedback_silence();
  layer_mark_dirty(s_canvas);
}

static void prv_resume(void) {
  game_resume(clock_now_ms());
  game_set_screen(RB_SCREEN_PLAYING);
  audio_song_start(s_song, game_elapsed_ms());
  prv_timer_start();
  layer_mark_dirty(s_canvas);
}

// ---------------------------------------------------------------------------
// Hit path
//
// The shortest route from a button press to a score. game_judge_hit() returns a
// judgment; turning that into sound and vibration happens HERE, not inside
// game.c -- which is why the timing loop can never depend on an output.
// ---------------------------------------------------------------------------

static void prv_lane_hit(uint8_t lane, uint32_t press_now_ms) {
  const ChartNote *note = NULL;
  const RbJudgment judgment = game_judge_hit(lane, press_now_ms, &note);

  feedback_hit(lane, judgment, note, game_elapsed_ms());

#if RB_DEBUG_LOG_JUDGMENTS
  {
    const int32_t offset_ms =
        (note != NULL) ? (int32_t)(press_now_ms - game_origin_ms()) - (int32_t)note->hit_time_ms
                       : 0;
    APP_LOG(APP_LOG_LEVEL_DEBUG, "lane=%u judge=%d off=%ldms combo=%u score=%lu",
            (unsigned)lane, (int)judgment, (long)offset_ms, (unsigned)game_combo(),
            (unsigned long)game_score());
  }
#endif

  // Redraw immediately rather than waiting up to a frame: the flash should
  // appear on the press, not 33ms later.
  layer_mark_dirty(s_canvas);
}

// ---------------------------------------------------------------------------
// Input dispatch -- one provider serves every screen; the handlers branch on
// game_screen() rather than re-subscribing on each transition.
// ---------------------------------------------------------------------------

static void prv_on_lane_hit(uint8_t lane, uint32_t press_now_ms) {
  switch (game_screen()) {
    case RB_SCREEN_PLAYING:
      if (lane < RB_LANE_COUNT) {
        prv_lane_hit(lane, press_now_ms);
      }
      break;

    case RB_SCREEN_TITLE:
      // UP and DOWN move the selection, matching the buttons' physical order --
      // UP goes up the list. SELECT plays whatever is selected.
      if (lane == RB_LANE_MID) {
        prv_start_song();
      } else {
        const uint8_t count = chart_count();
        if (lane == RB_LANE_TOP) {
          s_song = (uint8_t)((s_song + count - 1) % count);
        } else {
          s_song = (uint8_t)((s_song + 1) % count);
        }
        s_chart = chart_get(s_song);
        // The title screen shows the selected song's chart and best, so it has
        // to be the loaded one even before play starts.
        game_start(s_chart, 0);
#if RB_DEBUG_LOG_JUDGMENTS
        // Selection is only verifiable this way once `pebble emu-button` has
        // wedged the emulator's screenshot service, which pressing a button to
        // change the selection reliably does. Logs keep working.
        APP_LOG(APP_LOG_LEVEL_DEBUG, "song %u/%u selected: %s",
                (unsigned)s_song, (unsigned)chart_count(), s_chart->title);
#endif
        layer_mark_dirty(s_canvas);
      }
      break;

    case RB_SCREEN_PAUSED:
      if (lane == RB_LANE_MID) {
        prv_resume();
      } else if (lane == RB_LANE_TOP) {
        prv_start_song();
      } else {
        prv_enter_title();
      }
      break;

    case RB_SCREEN_RESULTS:
      prv_enter_title();
      break;

    default:
      break;
  }
}

static void prv_on_back(void) {
  switch (game_screen()) {
    case RB_SCREEN_PLAYING:
      prv_pause();
      break;
    case RB_SCREEN_PAUSED:
    case RB_SCREEN_RESULTS:
      prv_enter_title();
      break;
    default:
      window_stack_pop(true);  // leave the app from the title screen
      break;
  }
}

// ---------------------------------------------------------------------------
// Debug autoplay -- drives the whole judgment path with no button presses, so
// gameplay frames can be captured before the emulator's screenshot service is at
// risk of being wedged by `pebble emu-button`. Compiled out when off.
// ---------------------------------------------------------------------------

#if RB_DEBUG_AUTOPLAY
// Must run BEFORE game_step(), or a note that fell due and expired inside a
// single frame gap would be auto-missed before the synthetic press could land.
// That is not hypothetical: the emulator's first frames after app launch are
// hundreds of ms apart, which was enough to lose the opening notes.
static void prv_autoplay(uint32_t now_ms) {
  const uint32_t origin_ms = game_origin_ms();
  const uint32_t elapsed_ms = now_ms - origin_ms;

  for (uint16_t i = game_first_live(); i < s_chart->note_count; i++) {
    const uint32_t target_ms = s_chart->notes[i].hit_time_ms + RB_DEBUG_AUTOPLAY_OFFSET_MS;
    if (target_ms > elapsed_ms) {
      break;
    }
    if (game_note_judgment(i) != RB_JUDGE_NONE) {
      continue;
    }
#if RB_DEBUG_AUTOPLAY_MISS_EVERY
    // Index-based so the same notes are dropped every run -- a random miss would
    // make screenshots non-reproducible.
    if ((i % RB_DEBUG_AUTOPLAY_MISS_EVERY) == 0) {
      continue;
    }
#endif
    prv_lane_hit(s_chart->notes[i].lane, origin_ms + target_ms);
  }
}
#endif

// ---------------------------------------------------------------------------
// Frame loop
//
// Mutates state and marks the layer dirty; it draws nothing itself and decides
// no timing. Both note positions and judgments derive from clock_now_ms(), so a
// late or dropped frame changes what is seen, never what is scored.
// ---------------------------------------------------------------------------

static void prv_frame(void *data) {
  s_timer = NULL;
  (void)data;

  uint32_t now_ms = clock_now_ms();

#if RB_DEBUG_LOG_AUDIO
  {
    // Frame pacing. If this drifts well above RB_FRAME_MS the picture stutters,
    // but scoring is unaffected -- that separation is the point of the design,
    // and it is worth confirming rather than assuming.
    static uint32_t s_prev_ms;
    static uint16_t s_frames;
    static time_t s_wall0;
    static uint32_t s_clock0;
    if (s_wall0 == 0) {
      s_wall0 = time(NULL);
      s_clock0 = now_ms;
    }
    if (s_prev_ms != 0 && (++s_frames % 30) == 0) {
      // clock vs wall is the one that matters: they must advance together. When
      // they did not, the song raced ahead of the music and the speaker could
      // not be fed fast enough. See the rate discipline in clock.c.
      const long wall_s = (long)(time(NULL) - s_wall0);
      APP_LOG(APP_LOG_LEVEL_DEBUG, "frame=%lums clock=%lums wall=%lds elapsed=%lums",
              (unsigned long)(now_ms - s_prev_ms), (unsigned long)(now_ms - s_clock0), wall_s,
              (unsigned long)game_elapsed_ms());
    }
    s_prev_ms = now_ms;
  }
#endif

#if RB_DEBUG_FREEZE_AT_MS
  // Hold the song still so a chosen moment can be captured; `pebble screenshot`
  // is a ~1s round trip and would otherwise always miss it.
  //
  // Clamping the clock reading -- rather than skipping the step -- is what makes
  // the capture reproducible: the emulator's first frames after app launch can
  // be hundreds of ms apart, so simply stopping once elapsed passes the
  // threshold freezes at an arbitrary overshoot instead of the value asked for.
  // Clamping here, before anything reads it, also keeps autoplay and the game
  // step on the same timeline.
  {
    const uint32_t freeze_at = game_origin_ms() + RB_DEBUG_FREEZE_AT_MS;
    if (now_ms > freeze_at) {
      now_ms = freeze_at;
    }
  }
#endif

#if RB_DEBUG_AUTOPLAY
  prv_autoplay(now_ms);
#endif

  game_step(now_ms);

  if (game_is_finished()) {
    prv_enter_results();
    return;  // the timer is stopped; do not reschedule
  }

  layer_mark_dirty(s_canvas);

  // Audio is released against SONG time, never against an AppTimer. The two are
  // not interchangeable -- song time excludes pauses, and the clock's tick is not
  // the real-time interval it was asked for -- so a timer set for "1800ms from
  // now" and a song clock reading 1800 are different moments. Releasing on the
  // song clock is what makes the music land on the notes rather than near them.
  //
  // Passing elapsed in keeps the one-way dependency intact -- the clock drives
  // the audio, and audio_tick() only ever reads it.
  audio_tick(game_elapsed_ms());

  s_timer = app_timer_register(RB_FRAME_MS, prv_frame, NULL);
}

// ---------------------------------------------------------------------------
// Window lifecycle
// ---------------------------------------------------------------------------

static void prv_window_load(Window *window) {
  Layer *const root = window_get_root_layer(window);
  s_canvas = layer_create(layer_get_bounds(root));
  layer_set_update_proc(s_canvas, render_update_proc);
  layer_add_child(root, s_canvas);
}

static void prv_window_unload(Window *window) {
  (void)window;
  layer_destroy(s_canvas);
  s_canvas = NULL;
}

static void prv_window_appear(Window *window) {
  (void)window;
  clock_init();
#if RB_DEBUG_AUTOSTART
  s_song = (chart_count() > RB_DEBUG_AUTOSTART_SONG) ? RB_DEBUG_AUTOSTART_SONG : 0;
  prv_start_song();
#else
  prv_enter_title();
#endif
}

static void prv_window_disappear(Window *window) {
  (void)window;
  prv_timer_stop();
  audio_song_stop();
  feedback_silence();
  clock_stop();
}

static void prv_init(void) {
#if RB_DEBUG_LOG_AUDIO
  // Sizing data for the "stream the music from the phone instead" question.
  APP_LOG(APP_LOG_LEVEL_DEBUG, "appmessage max: inbox=%lu outbox=%lu bytes",
          (unsigned long)app_message_inbox_size_maximum(),
          (unsigned long)app_message_outbox_size_maximum());
#endif
  save_load();
  audio_init();
  // Sound and haptics are always on. They were once togglable from the title
  // screen, which is where the song selector now lives; the buttons could not
  // do both, and a rhythm game with the sound off is not the thing anyway.
  audio_set_enabled(true);
  feedback_set_haptics(true);

  // A resource-backed chart would be preferred if one existed; the loader is
  // stubbed in v1, so this always falls through to the compiled-in songs.
  static Chart loaded;
  s_song = 0;
  s_chart = chart_load_from_resource(0, &loaded) ? &loaded : chart_get(s_song);
  game_start(s_chart, 0);  // populate chart-dependent state for the title screen

  const RbInputHandlers handlers = {
    .on_lane_hit = prv_on_lane_hit,
    .on_back = prv_on_back,
  };
  input_init(&handlers);

  s_window = window_create();
  window_set_background_color(s_window, GColorBlack);
  window_set_click_config_provider(s_window, input_click_config_provider);
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = prv_window_load,
    .appear = prv_window_appear,
    .disappear = prv_window_disappear,
    .unload = prv_window_unload,
  });
  window_stack_push(s_window, true);
}

static void prv_deinit(void) {
  window_destroy(s_window);
}

int main(void) {
  prv_init();
  app_event_loop();
  prv_deinit();
}
