/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "muse_ui.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "mbedtls/base64.h"
#include "src/draw/lv_image_decoder_private.h"   /* custom decoder */
#include "src/misc/lv_area_private.h"            /* lv_area_intersect, for the ring */

#include "esp_app_desc.h"   /* museStopWatch dev.17: the history upload names its firmware */
#include "msw_stack.h"      /* museStopWatch dev.20: PSRAM task stacks (dash, batthist) */
#include "muse_battery.h"   /* museStopWatch dev.17: the battery history */
#include "muse_ble.h"
#include "muse_board.h"
#include "muse_input.h"
#include "muse_chat.h"
#include "muse_console.h"
#include "muse_lang.h"
#include "muse_link.h"
#include "muse_mem.h"
#include "muse_menu.h"
#include "muse_pixel.h"
#include "muse_settings.h"
#include "muse_settings_ui.h"
#include "muse_state.h"
#include "muse_text.h"
#include "muse_wifi.h"
#include "muse_voice.h"
#if CONFIG_MUSE_VIDEO
#include "muse_video.h"
#endif
#if CONFIG_MUSE_BOARD_SENSECAP_WATCHER || CONFIG_MUSE_BOARD_M5STACK_STOPWATCH
#include "muse_review.h"
#endif
#if CONFIG_MUSE_BOARD_M5STACK_STOPWATCH
#include "cJSON.h"   /* museStopWatch: the dashboard edition is JSON */
#include "nvs.h"     /* museStopWatch: the care state and face mode (dev.14) */
#endif
#if CONFIG_MUSE_WATCHER_CAMERA
#include "boards/watcher_camera.h"
#endif

static const char *TAG = "muse_ui";

#define METER_SEGS 14
#define METER_SEG_PX 9
#define METER_GAP_PX 4
#define RING_RANGE 1000
#define CAPTION_W 256           /* 16 columns of unscii_16, the width reply captions wrap to */
#define CAPTION_LINE_SPACE 2
#define LATIN_BAND_MARGIN 26    /* a small screen's accented captions clear its button icons */
#define LONG_MUSE_TOP 44        /* long layout: Muse's top, under the state and name rows, while a reply shows */
#define LONG_HINTS_H 135        /* long layout: the bottom strip, for the key hints, speaker and page dots */
#define LONG_LINE_SPACE 4
#define ART_BLANK_ROWS 3        /* Muse's art never reaches the grid's bottom rows */
#define MINI_CELL_PX 2          /* Muse's grid cells over a reply that's read */
#define ANSWER_MS 300           /* Muse making room for a reply, and back */
#define SPEAKER_PX 64
#define SPEAKER_GROW_PX 8       /* how much the speaker button swells while held */
#define SPEAKER_HOLD_MS 400     /* LVGL's long press */

#define COLOR_DIM 0x8b84a8
#define COLOR_CAPTION 0xd8d2ff
#define COLOR_RING_BG 0x140f22
#define COLOR_METER_OFF 0x1d1733
#define COLOR_ACCENT 0xa77dff
#define COLOR_DOT_OFF 0x3a3358
#define COLOR_LIT 0xf2efff
#define COLOR_CARD_BG 0x201d33    /* dashboard section card, a step above the tile's black */
#define COLOR_CARD_BAR 0xa78bfa   /* dashboard section card title accent bar */
#define SETTINGS_TICK_S 0.25f

/* Text on 128 px screens, as in the button menu (muse_menu.c). */
#if LV_FONT_MONTSERRAT_12
#define FONT_COMPACT (&lv_font_montserrat_12)
#else
#define FONT_COMPACT (&lv_font_montserrat_14)
#endif

/*
 * Two layouts: the full one (round 466 px or similar) with a progress ring,
 * level meter and settings tile, and a compact one for tiny screens (128 px)
 * where Muse fills the screen and text overlays it.
 */
static int s_w, s_h;
static bool s_small;
static bool s_tall;         /* compact, with room above and below Muse (StickS3) */
static bool s_long;         /* tall enough for a page of the reply under Muse */
static int s_canvas_px;     /* Muse's size on screen */
static int s_dy;            /* full layout: offset from a 466 px tall screen */
static lv_indev_t *s_indev;
static lv_obj_t *s_tv;
static bool s_swipe_enabled = true;
static bool s_touch_talk_down;
#if CONFIG_MUSE_VOICE_NOTE_TOGGLE
static bool s_note_listening;   /* a toggled note is recording: keep its face up */
#endif
/* True while the face tile is the visible tile. Updated in the LVGL task;
 * read from any task (single word, benign if briefly stale). */
static volatile bool s_face_active = true;
static lv_obj_t *s_face;

static void update_tileview_swipe(void)
{
    if (!s_tv) return;
    bool enabled = s_swipe_enabled && !s_touch_talk_down;
#if CONFIG_MUSE_VOICE_NOTE_TOGGLE
    enabled = enabled && !s_note_listening;
#endif
    if (enabled != lv_obj_has_flag(s_tv, LV_OBJ_FLAG_SCROLLABLE)) {
        lv_obj_set_flag(s_tv, LV_OBJ_FLAG_SCROLLABLE, enabled);
    }
}

static void tile_changed_cb(lv_event_t *e)
{
    lv_obj_t *tv = lv_event_get_target(e);
    s_face_active = (lv_tileview_get_tile_active(tv) == s_face);
}

/* From any task: the face tile is the currently visible tile. */
bool muse_ui_face_active(void)
{
    return s_face_active;
}
static lv_obj_t *s_settings;
/* museStopWatch: the mini-app launcher, left of the face (components/muse_diy_stopwatch/miniapp). */
static lv_obj_t *s_launcher;
void msw_launcher_build(lv_obj_t *tile) __attribute__((weak));
/* museStopWatch: the reply history, below the face (swipe up for it). */
static lv_obj_t *s_history;
/* museStopWatch: quick settings, above the face (swipe down for them). */
static lv_obj_t *s_quick;
/* museStopWatch: the dashboard, right of the face (swipe left for it);
 * Settings moved one tile further right and is opened from quick settings. */
static lv_obj_t *s_dash;
#if CONFIG_MUSE_BOARD_M5STACK_STOPWATCH
static lv_obj_t *s_hist_body, *s_hist_pos, *s_hist_chat;
static muse_review_view_t s_hist_view;
static char s_hist_buf[MUSE_CAPTION_MAX];
static int s_hist_cols, s_hist_lines, s_hist_cjk_cols, s_hist_cjk_lines;
#if CONFIG_MUSE_LATIN_FONT
static int s_hist_latin_cols, s_hist_latin_lines;
#endif
/* museStopWatch: quick chat switching from the history tile (ui/msw_side.c). */
const char *msw_chat_name(void) __attribute__((weak));
void msw_chat_cycle(int dir) __attribute__((weak));
#endif
#define TILES 3
static lv_obj_t *s_dots[TILES];

/* The tileview's scroll position with the face showing: past the launcher. */
static int face_scroll_x(void)
{
    return s_face ? lv_obj_get_x(s_face) : 0;
}

/* 0 launcher, 1 face, 2 right of the face (dots left to right). museStopWatch:
 * the dashboard sits at 2 now, with Settings one more tile to the right. */
static int tile_page(void)
{
    lv_obj_t *t = lv_tileview_get_tile_active(s_tv);
    return t == s_settings ? 2 : t == s_launcher ? 0 :
#if CONFIG_MUSE_BOARD_M5STACK_STOPWATCH
           t == s_dash ? 2 :
#endif
           1;
}
static lv_obj_t *s_wifi_icon;
static lv_obj_t *s_ble_icon;
static lv_obj_t *s_cover;
static lv_obj_t *s_pair;
static lv_obj_t *s_pair_code;
static lv_obj_t *s_pair_title;
static lv_obj_t *s_pair_hint;
static lv_obj_t *s_canvas;
static lv_obj_t *s_mic_icon;
static lv_obj_t *s_ptt_hint, *s_spk_hint, *s_pwr_hint;
#if LV_USE_SNAPSHOT
static volatile bool s_bench_demo;
void muse_ui_bench_demo(bool enabled)
{
    s_bench_demo = enabled;   /* display-only: never changes BLE identity/setup */
}
#endif
static lv_obj_t *s_ring;
static lv_obj_t *s_bar;     /* compact layout's stand-in for the ring */
#if CONFIG_MUSE_SMALL_FULL_REPLY
#define FULL_REPLY_TOP 18   /* under the compact status line */
static const lv_font_t *s_full_font;   /* the reply's font, from the Text size setting */
static int s_full_size = -1;           /* the setting s_full_font was picked for */
static int s_full_reply_h;  /* the caption's height while it fills the screen */
static bool s_full_reply;
static bool s_caption_shown;
#endif
static lv_obj_t *s_state_lbl;
static lv_obj_t *s_name_lbl;    /* this gadget's own name, to tell it from the next one */
static lv_obj_t *s_cv_lbl;      /* museStopWatch: "CV: ..." while voice.play's voice speaks */
/* museStopWatch: the voice's name while voice.play's audio is heard (main/msw_play.c). */
const char *msw_voice_credit(void) __attribute__((weak));
/* museStopWatch: the local time (muse_diy_stopwatch/ui/msw_time.c, JST,
 * RTC-backed); false until the clock is set. dev.14's care meters settle
 * against it, and the clock face reads it. */
bool msw_time_now(struct tm *local) __attribute__((weak));
static lv_obj_t *s_power_lbl;
static lv_obj_t *s_caption_lbl;
static int s_cap_h, s_cap_y;   /* the caption's built height and centre offset, to restore it */
#if CONFIG_MUSE_BOARD_M5STACK_STOPWATCH
static lv_obj_t *s_full_lbl;   /* the reply label up large, Muse shrunk below it */
static float s_full_since;     /* when the kept reply went up (auto-clears seconds later) */
static lv_obj_t *s_quick_bright, *s_quick_bright_val, *s_quick_vol, *s_quick_vol_val;
static lv_obj_t *s_ask_ok, *s_ask_no;   /* approval buttons over a question's kept reply */
static bool s_ask_shown;
/* dev.14: the quick tile's sound-mode buttons (the yellow button's old job). */
static lv_obj_t *s_quick_snd[3];
static lv_obj_t *s_quick_snd_lbl[3];
static int s_quick_snd_shown = -1;
/* dev.14: the clock face the yellow button switches to, and the care
 * (tamagotchi) panel over the avatar. Built in the section after
 * update_quick(); the state itself lives just below. */
#if LV_FONT_MONTSERRAT_48
#define FONT_CLOCK (&lv_font_montserrat_48)
#else
#define FONT_CLOCK (&lv_font_montserrat_28)
#endif
static lv_obj_t *s_clock_box, *s_clock_time, *s_clock_date;
/* dev.16: the clock's small rows — the edition's weather and next event
 * (render_dash keeps the strings in s_dash_wx / s_dash_next), and the
 * running timer's remaining time. dev.14's battery row is gone: the
 * header chrome at the top of the face already shows the battery. */
static lv_obj_t *s_clock_wx, *s_clock_next, *s_clock_timer, *s_clock_train;
static char s_dash_wx[96];
static char s_dash_next[128];
/* dev.22: the next-train cache (the section by render_dash()). The
 * edition's trains block, validated and kept in NVS so the clock face
 * can count departures down from the RTC alone, with no fetch of its
 * own. Tables are minutes from midnight (up to 25:59 for the small
 * hours), one per day type: 0 weekday, 1 Saturday, 2 holiday. */
#define TRAINS_MAX 160
static struct {
    bool loaded, present;
    uint8_t walk;            /* minutes from home to the platform */
    char date[11];           /* the edition's days.date ("YYYY-MM-DD"); "" = no flags */
    uint8_t dt_today, dt_tom;
    uint8_t n[3];
    uint16_t mins[3][TRAINS_MAX];
    uint32_t hash;           /* change detection, so NVS is written once per timetable */
} s_trains;
static void trains_line(char *buf, size_t cap, const struct tm *local, bool have_time);   /* the dash section, below */
static void trains_consider(cJSON *root);   /* the dash section, below */
static int64_t s_dash_info_us;   /* when the edition behind those strings landed (the dash section's s_dash_ok_us, copied by render_dash) */
static bool s_clock_mode;              /* the face shows the clock */
static bool s_clock_shown;             /* the clock box is up right now */
static volatile int s_face_toggles;    /* yellow presses, applied in the LVGL task */
static int s_face_toggles_done;
static lv_obj_t *s_care_panel, *s_care_head;
static lv_obj_t *s_care_belly_fill, *s_care_belly_val, *s_care_mood_fill, *s_care_mood_val;
static lv_obj_t *s_care_food;
static bool s_care_open;
static float s_care_food_from, s_care_food_until;
static lv_obj_t *s_acc_bell, *s_acc_ribbon_l, *s_acc_ribbon_r, *s_acc_ribbon_c;
static int s_acc_stage = -1;           /* the stage the face size/accessories show */
static void care_open_panel(void);     /* below update_quick() */
/* dev.16: the countdown timer (the section after update_care()). The
 * dialled length and the state live here; the finish is timed by an
 * esp_timer so it lands while the watch sleeps, and its callback only
 * flags it and wakes the screen — update_timer() does the rest. */
static lv_obj_t *s_timer_panel, *s_timer_time, *s_timer_state, *s_timer_main_lbl, *s_timer_cancel_btn;
static lv_obj_t *s_timer_alert, *s_timer_alert_time;
static bool s_timer_open;
static volatile int s_timer_open_reqs;   /* muse_ui_request_timer(), any task */
static int s_timer_open_done;
static volatile bool s_timer_fired;      /* set by the esp_timer callback */
static esp_timer_handle_t s_timer_esp;
static int64_t s_timer_end_us;           /* when the running timer ends; 0 = not running */
static int s_timer_left_s;               /* the seconds left, while paused */
static int s_timer_set_s = 180;          /* the dialled length */
static bool s_timer_paused;
static float s_timer_alert_until;        /* the alert overlay's deadline, frame time */
static float s_timer_alert_last;         /* the last alert pulse/chirp, frame time */
static int timer_left_s(void);           /* below update_care(); -1 while not counting */
/* dev.22: the pomodoro (the section after update_timer()). A phase
 * chain on the timer's esp_timer pattern — 25 min focus, 5 min break,
 * a long break after 4 focuses, all Settings-tunable — with its state
 * in the care NVS handle so a session survives sleep and reboots. It
 * and the plain timer are exclusive: starting one cancels the other. */
enum { POMO_FOCUS = 0, POMO_BREAK = 1, POMO_LONG = 2, POMO_IDLE = 3 };
static struct {
    bool loaded;
    int focus_min, break_min, long_min, sets;   /* the Settings > Pomodoro values */
    uint8_t phase;           /* POMO_*; POMO_IDLE = no session */
    uint8_t done;            /* focuses finished in the current round */
    bool paused;
    int left_s;              /* seconds left, while paused */
    bool arm_pending;        /* a restored session re-arms in update_pomodoro() */
} s_pomo = { .focus_min = 25, .break_min = 5, .long_min = 15, .sets = 4, .phase = POMO_IDLE };
static lv_obj_t *s_pomo_panel, *s_pomo_time, *s_pomo_state, *s_pomo_dots, *s_pomo_main_lbl;
static lv_obj_t *s_pomo_skip_btn, *s_pomo_end_btn;
static lv_obj_t *s_pomo_alert, *s_pomo_alert_title, *s_pomo_alert_sub;
static bool s_pomo_open;
static volatile int s_pomo_open_reqs;    /* muse_ui_request_pomodoro(), any task */
static int s_pomo_open_done;
static volatile bool s_pomo_fired;       /* set by the esp_timer callback */
static esp_timer_handle_t s_pomo_esp;
static int64_t s_pomo_end_us;            /* monotonic end of the running phase; 0 = not running */
static float s_pomo_alert_until;         /* the alert overlay's deadline, frame time */
static float s_pomo_alert_last;          /* the last alert pulse/chirp, frame time */
static int s_pomo_alert_ended;           /* the phase the live alert ended (POMO_*) */
static void pomo_load(void);             /* the pomodoro section, below */
static void pomo_save(void);             /* the pomodoro section, below */
static void pomo_close_panel(void);      /* the pomodoro section, below */
static void pomo_end_session(void);      /* the pomodoro section, below */
/* dev.16: the yellow button's long-press assignment (Settings > Yellow
 * button), in the care NVS handle beside face_mode. The action defaults
 * to the timer; the app id matters only for MUSE_YEL_LONG_APP. */
static uint8_t s_yel_long_act;           /* MUSE_YEL_LONG_* (muse_ui.h) */
static char s_yel_long_app[24];
#endif
#if CONFIG_MUSE_BOARD_M5STACK_STOPWATCH
/*
 * dev.14: the avatar's care state — the tamagotchi under the care panel.
 *
 * Belly and mood (0..100) drain slowly against the wall clock; feeding
 * and petting (the panel, below update_quick()) refill them and add to
 * the lifetime bond, whose thresholds are the three evolution stages.
 * Everything persists in the "muse" NVS namespace under care_* keys,
 * plus face_mode for the yellow button's clock face. The state lives in
 * this file, not muse_settings.c: the settings host harness pins that
 * file's NVS writes to its own keys. Decay settles only in whole points
 * and the anchor advances by exactly the settled span, so settling
 * often (panel opens, feeds, a once-a-minute pass in the tick) never
 * loses or double-counts time; while the clock is unset nothing decays.
 */
#define CARE_FULL 100
#define CARE_POINT_SECS 600    /* one point of belly/mood per ten minutes */
#define CARE_STAGE2_BOND 50
#define CARE_STAGE3_BOND 150

static struct {
    bool loaded;
    uint8_t belly, mood;   /* 0..CARE_FULL */
    uint16_t bond;         /* lifetime affection points */
    uint32_t anchor;       /* the epoch the meters were last settled at */
} s_care;
static nvs_handle_t s_care_nvs;

static bool care_now(uint32_t *out)
{
    struct tm local;
    if (!msw_time_now || !msw_time_now(&local)) {
        return false;   /* the clock is not set yet */
    }
    *out = (uint32_t)time(NULL);
    return true;
}

static void care_settle(void)
{
    uint32_t now;
    if (!care_now(&now)) {
        return;
    }
    if (!s_care.anchor || now <= s_care.anchor) {
        s_care.anchor = now;   /* first run, or the clock stepped back */
        return;
    }
    uint32_t away = now - s_care.anchor;
    if (away > 7u * 24 * 3600) {
        away = 7u * 24 * 3600;   /* a week away empties the meters, no more */
    }
    uint32_t points = away / CARE_POINT_SECS;
    if (!points) {
        return;
    }
    s_care.belly = points >= s_care.belly ? 0 : s_care.belly - points;
    s_care.mood = points >= s_care.mood ? 0 : s_care.mood - points;
    s_care.anchor += points * CARE_POINT_SECS;
}

static void care_save(void)
{
    if (!s_care_nvs) {
        return;
    }
    nvs_set_u8(s_care_nvs, "care_belly", s_care.belly);
    nvs_set_u8(s_care_nvs, "care_mood", s_care.mood);
    nvs_set_u16(s_care_nvs, "care_bond", s_care.bond);
    nvs_set_u32(s_care_nvs, "care_ts", s_care.anchor);
    nvs_set_u8(s_care_nvs, "face_mode", s_clock_mode ? 1 : 0);
    nvs_commit(s_care_nvs);
}

static void care_load(void)
{
    if (s_care.loaded) {
        return;
    }
    s_care.loaded = true;
    s_care.belly = 80;   /* a new avatar starts fed and cheerful */
    s_care.mood = 80;
    if (nvs_open("muse", NVS_READWRITE, &s_care_nvs) != ESP_OK) {
        s_care_nvs = 0;   /* RAM only this boot */
    }
    if (s_care_nvs) {
        uint8_t b;
        uint16_t bond;
        uint32_t ts;
        if (nvs_get_u8(s_care_nvs, "care_belly", &b) == ESP_OK && b <= CARE_FULL) s_care.belly = b;
        if (nvs_get_u8(s_care_nvs, "care_mood", &b) == ESP_OK && b <= CARE_FULL) s_care.mood = b;
        if (nvs_get_u16(s_care_nvs, "care_bond", &bond) == ESP_OK) s_care.bond = bond;
        if (nvs_get_u32(s_care_nvs, "care_ts", &ts) == ESP_OK) s_care.anchor = ts;
        if (nvs_get_u8(s_care_nvs, "face_mode", &b) == ESP_OK) s_clock_mode = b != 0;
        if (nvs_get_u8(s_care_nvs, "yel_l_act", &b) == ESP_OK && b <= MUSE_YEL_LONG_POMODORO) s_yel_long_act = b;
        size_t yn = sizeof(s_yel_long_app);
        if (nvs_get_str(s_care_nvs, "yel_l_app", s_yel_long_app, &yn) != ESP_OK) s_yel_long_app[0] = '\0';
    }
    care_settle();   /* the time away since the last save counts too */
    pomo_load();     /* dev.22: the pomodoro's settings and session ride the same handle */
}

/* 0, 1 or 2: the evolution stage the lifetime bond has reached. */
static int care_stage(void)
{
    care_load();
    return s_care.bond >= CARE_STAGE3_BOND ? 2 : s_care.bond >= CARE_STAGE2_BOND ? 1 : 0;
}

/* The yellow button (muse_input.c, any task): count the press; the LVGL
 * task applies it in update_face_mode(). Returns the face the press is
 * taking the watch to (true: the clock), for the button's hint. */
bool muse_ui_toggle_face_mode(void)
{
    care_load();
    int pending = s_face_toggles - s_face_toggles_done;
    bool target = s_clock_mode ^ (pending & 1) ^ 1;
    s_face_toggles++;
    return target;
}

bool muse_ui_clock_mode(void)
{
    return s_clock_mode;
}

/* dev.16: the yellow button's long-press assignment. The getters settle
 * the load first (care_load opens the shared NVS handle); the setter
 * saves at once, like every other setting. */
int muse_ui_yellow_long_action(void)
{
    care_load();
    return s_yel_long_act;
}

const char *muse_ui_yellow_long_app(void)
{
    care_load();
    return s_yel_long_app;
}

void muse_ui_set_yellow_long(int action, const char *app_id)
{
    care_load();
    s_yel_long_act = action >= 0 && action <= MUSE_YEL_LONG_POMODORO ? (uint8_t)action : MUSE_YEL_LONG_TIMER;
    snprintf(s_yel_long_app, sizeof(s_yel_long_app), "%s", app_id ? app_id : "");
    if (s_care_nvs) {
        nvs_set_u8(s_care_nvs, "yel_l_act", s_yel_long_act);
        nvs_set_str(s_care_nvs, "yel_l_app", s_yel_long_app);
        nvs_commit(s_care_nvs);
    }
}

/* dev.16: a timer-open request (any task); update_timer() applies it. */
void muse_ui_request_timer(void)
{
    s_timer_open_reqs++;
}

/* dev.22: a pomodoro-open request (any task); update_pomodoro() applies it. */
void muse_ui_request_pomodoro(void)
{
    s_pomo_open_reqs++;
}
#endif
/* The avatar's resting size: on the StopWatch a cared-for avatar grows a
 * little per evolution stage (dev.14); everywhere else the canvas size. */
static int face_px(void)
{
#if CONFIG_MUSE_BOARD_M5STACK_STOPWATCH
    return s_canvas_px * (100 + 6 * care_stage()) / 100;
#else
    return s_canvas_px;
#endif
}
static lv_obj_t *s_reply_lbl;   /* full layout: the reply's page while answering */
#if CONFIG_MUSE_BOARD_SENSECAP_WATCHER
static lv_obj_t *s_review_position;
#endif
static lv_obj_t *s_meter[METER_SEGS];
static lv_obj_t *s_speaker;
static lv_obj_t *s_speaker_icon;
static lv_obj_t *s_aux_icon;
#if CONFIG_MUSE_BOARD_M5STACK_STOPWATCH && CONFIG_MUSE_REALTIME_VOICE
static lv_obj_t *s_talk_hint_lbl, *s_end_hint_lbl;
#define CALL_HINT_OUT 26   /* px outwards from the rim icon's x: the 320 px avatar's sparkles reach ~125 */
#endif
static lv_obj_t *s_image;   /* display.draw_url, over the face */
#if CONFIG_MUSE_WATCHER_CAMERA
static lv_obj_t *s_camera_hint;
#endif
static lv_image_dsc_t s_image_dsc;  /* its data is set once the image is shown */
/* The download writes the pixels without the display lock, so a big JPEG isn't
 * held up by each frame; this guards the buffer and what changed in it. */
static SemaphoreHandle_t s_image_mutex;
static uint16_t *s_image_buf;
static lv_area_t s_image_area;
static bool s_image_dirty;
static bool s_ready;

static float s_level;
static int s_shown_state = -1;
static const char *s_shown_name;
static const char *s_idle_name = "READY";   /* idle's label: set by the Wi-Fi state */
static int s_shown_lit = -1;
static uint32_t s_shown_accent;
static uint32_t s_shown_status_accent;   /* the label and ring: WORKING recolours within THINKING */
static bool s_meter_visible = true;
static int s_ring_value = -1;
static uint32_t s_caption_version;
static float s_next_power_update;
static float s_next_settings_tick;
static bool s_dark;
static int s_brightness = -1;       /* last applied */
static int s_preview_brightness = -1;
static int s_shown_page = -1;
static int s_shown_speaker = -1;
static muse_mode_t s_last_mode = MUSE_MODE_COUNT;
static muse_mode_t s_last_epaper_pose = MUSE_MODE_COUNT;

/*
 * While Muse is thinking or speaking it shrinks to make room for the reply:
 * a little when the reply is heard, over a few lines to follow along, and
 * to the top when it's only read, over a page.
 */
typedef struct {
    int px, y;                /* Muse's size and centre */
    int cols, lines;          /* the reply's page */
    int w, h, top;            /* and where it goes */
    lv_text_align_t align;
    lv_obj_t *hides[4];       /* what it covers */
} answer_layout_t;

enum { ANSWER_HEARD, ANSWER_READ };
static answer_layout_t s_answers[2];
static int s_answer = -1;       /* the layout showing, or -1 */
static int s_page_for = -1;     /* the layout the reply's page is sized for */
static bool s_page_restore;     /* the history tile had the pager: size it for the face again */
static int s_big_y;             /* Muse's centre at full size */
static int s_muse_y;            /* and now */
static bool s_muse_up;          /* long layout: up out of the reply's way */
static int s_from_px, s_from_y, s_to_px, s_to_y;

static const char *const MODE_NAMES[MUSE_MODE_COUNT] = {
    [MUSE_MODE_BOOT] = "WAKING UP",
    [MUSE_MODE_IDLE] = "READY",
    [MUSE_MODE_LISTENING] = "LISTENING",
    [MUSE_MODE_THINKING] = "THINKING",
    [MUSE_MODE_SPEAKING] = "SPEAKING",
    [MUSE_MODE_ERROR] = "ERROR",
    [MUSE_MODE_OFF] = "GOODBYE",
};

/*
 * Muse reaches LVGL as an image whose pixels are made on demand: a decoder
 * scales the 64 px grid up one strip of rows at a time, so the full-size frame
 * never sits in RAM and LVGL never runs its (slow) image transform.
 *
 * LVGL splits each refresh into a tile per draw unit and draws them at once,
 * each with its own open decoder, so each one takes its own strip: with one
 * shared strip, a tile blended rows the other had just scaled into it.
 */
#define STRIP_ROWS 16
#define STRIPS LV_DRAW_SW_DRAW_UNIT_CNT

#define MUSE_PIXEL_MAX_PX 512   /* muse_pixel_set_size's cap */
#define DIRTY_RECTS_MAX 8       /* with the meter and labels, well inside LVGL's 32 areas */
#define DIRTY_MERGE_PX 2048     /* pixels that cost about one walk of the widget tree */

static lv_image_dsc_t s_muse_src;
static lv_draw_buf_t *s_strips[STRIPS];
static bool s_strip_busy[STRIPS];
static uint16_t *s_cells;       /* each cell's colour as last invalidated */
static uint16_t *s_cell_row;    /* one screen row of Muse */
static bool s_cells_valid;

static lv_result_t muse_dec_info(lv_image_decoder_t *dec, lv_image_decoder_dsc_t *dsc, lv_image_header_t *header)
{
    (void)dec;
    if (dsc->src != &s_muse_src) {
        return LV_RESULT_INVALID;
    }
    *header = s_muse_src.header;
    return LV_RESULT_OK;
}

/* LVGL holds its decoder lock around open and close. */
static lv_result_t muse_dec_open(lv_image_decoder_t *dec, lv_image_decoder_dsc_t *dsc)
{
    (void)dec;
    for (int i = 0; i < STRIPS; i++) {
        if (!s_strip_busy[i]) {
            s_strip_busy[i] = true;
            dsc->user_data = s_strips[i];
            return LV_RESULT_OK;   /* the rest of the work is in get_area */
        }
    }
    return LV_RESULT_INVALID;
}

static lv_result_t muse_dec_get_area(lv_image_decoder_t *dec, lv_image_decoder_dsc_t *dsc,
                                     const lv_area_t *full, lv_area_t *area)
{
    (void)dec;
    int32_t y1 = area->y1 == LV_COORD_MIN ? full->y1 : area->y2 + 1;
    if (y1 > full->y2) {
        return LV_RESULT_INVALID;
    }
    int32_t y2 = LV_MIN(y1 + STRIP_ROWS - 1, full->y2);
    int32_t w = lv_area_get_width(full);
    lv_draw_buf_t *buf = lv_draw_buf_reshape(dsc->user_data, LV_COLOR_FORMAT_RGB565, w, y2 - y1 + 1, LV_STRIDE_AUTO);
    if (!buf) {
        return LV_RESULT_INVALID;
    }
    muse_pixel_scale((uint16_t *)buf->data, buf->header.stride / sizeof(uint16_t), full->x1, full->x2, y1, y2);
    area->x1 = full->x1;
    area->x2 = full->x2;
    area->y1 = y1;
    area->y2 = y2;
    dsc->decoded = buf;
    return LV_RESULT_OK;
}

static void muse_dec_close(lv_image_decoder_t *dec, lv_image_decoder_dsc_t *dsc)
{
    (void)dec;
    for (int i = 0; i < STRIPS; i++) {
        if (s_strips[i] == dsc->user_data) {
            s_strip_busy[i] = false;   /* kept for the next open */
        }
    }
}

static void muse_image_init(void)
{
    s_muse_src.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_muse_src.header.cf = LV_COLOR_FORMAT_RGB565;
    s_muse_src.header.w = s_canvas_px;
    s_muse_src.header.h = s_canvas_px;
    s_muse_src.header.stride = s_canvas_px * sizeof(uint16_t);
    s_muse_src.data = (const uint8_t *)&s_muse_src;   /* LVGL skips sources without data */
    muse_pixel_set_size(s_canvas_px);

    for (int i = 0; i < STRIPS; i++) {
        s_strips[i] = lv_draw_buf_create(s_canvas_px, STRIP_ROWS, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO);
        assert(s_strips[i]);
    }
    lv_image_decoder_t *dec = lv_image_decoder_create();
    assert(dec);
    lv_image_decoder_set_info_cb(dec, muse_dec_info);
    lv_image_decoder_set_open_cb(dec, muse_dec_open);
    lv_image_decoder_set_get_area_cb(dec, muse_dec_get_area);
    lv_image_decoder_set_close_cb(dec, muse_dec_close);
    dec->name = "muse";

    s_cells = heap_caps_malloc(MUSE_PX_W * MUSE_PX_H * sizeof(uint16_t), MUSE_BIG_CAPS);
    s_cell_row = heap_caps_malloc(MUSE_PIXEL_MAX_PX * sizeof(uint16_t), MUSE_BIG_CAPS);
    if (!s_cells || !s_cell_row) {
        free(s_cells);
        free(s_cell_row);
        s_cells = s_cell_row = NULL;   /* redraw all of Muse every frame */
    }
}

/*
 * Most frames change only a few of Muse's cells, so only those are redrawn.
 * Each frame reads back one screen pixel of every cell (its first, which the
 * faint grid never dims) and compares it with the last frame's. A row's
 * changed cells make one span, and neighbouring spans merge while the pixels
 * drawn for nothing cost less than LVGL's walk of the widget tree for one more
 * area.
 */
static int32_t cell_px(int cell, int size)
{
    return (cell * size + MUSE_PX_W - 1) / MUSE_PX_W;   /* a cell's first screen pixel */
}

static int32_t rect_cells(const lv_area_t *r)
{
    return lv_area_get_width(r) * lv_area_get_height(r);
}

static void invalidate_muse(void)
{
    if (!s_cells) {
        lv_obj_invalidate(s_canvas);
        return;
    }
    int size = (int)s_muse_src.header.w;
    lv_area_t r[MUSE_PX_H];   /* in cells */
    int n = 0;
    for (int cy = 0; cy < MUSE_PX_H; cy++) {
        int32_t y = cell_px(cy, size);
        muse_pixel_scale(s_cell_row, size, 0, size - 1, y, y);
        uint16_t *last = &s_cells[cy * MUSE_PX_W];
        int x1 = -1, x2 = -1;
        for (int cx = 0; cx < MUSE_PX_W; cx++) {
            uint16_t c = s_cell_row[cell_px(cx, size)];
            if (c != last[cx] || !s_cells_valid) {
                last[cx] = c;
                x1 = x1 < 0 ? cx : x1;
                x2 = cx;
            }
        }
        if (x2 >= 0) {
            r[n++] = (lv_area_t){x1, cy, x2, cy};
        }
    }
    s_cells_valid = true;

    int64_t merge_cells = (int64_t)DIRTY_MERGE_PX * MUSE_PX_W * MUSE_PX_H / (size * size);
    while (n > 1) {
        int best = 0;
        int32_t best_cost = INT32_MAX;
        for (int i = 0; i + 1 < n; i++) {
            lv_area_t j = {LV_MIN(r[i].x1, r[i + 1].x1), r[i].y1, LV_MAX(r[i].x2, r[i + 1].x2), r[i + 1].y2};
            int32_t cost = rect_cells(&j) - rect_cells(&r[i]) - rect_cells(&r[i + 1]);
            if (cost < best_cost) {
                best = i;
                best_cost = cost;
            }
        }
        if (n <= DIRTY_RECTS_MAX && best_cost > merge_cells) {
            break;
        }
        r[best].x1 = LV_MIN(r[best].x1, r[best + 1].x1);
        r[best].x2 = LV_MAX(r[best].x2, r[best + 1].x2);
        r[best].y2 = r[best + 1].y2;
        memmove(&r[best + 1], &r[best + 2], (n - best - 2) * sizeof(r[0]));
        n--;
    }

    lv_area_t at;
    lv_obj_get_coords(s_canvas, &at);
    for (int i = 0; i < n; i++) {
        lv_area_t a = {
            at.x1 + cell_px(r[i].x1, size),
            at.y1 + cell_px(r[i].y1, size),
            at.x1 + cell_px(r[i].x2 + 1, size) - 1,
            at.y1 + cell_px(r[i].y2 + 1, size) - 1,
        };
        lv_obj_invalidate_area(s_canvas, &a);
    }
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color);

/* A microphone from primitives: LVGL's symbol font has none. */
static lv_obj_t *make_mic(lv_obj_t *parent, int size)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, size, size);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
    int stroke = size / 10 > 1 ? size / 10 : 1;

    lv_obj_t *head = lv_obj_create(box);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, size * 3 / 8, size * 9 / 16);
    lv_obj_set_style_radius(head, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(head, LV_OPA_COVER, 0);
    lv_obj_align(head, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *cradle = lv_arc_create(box);
    lv_obj_set_size(cradle, size * 5 / 8 + stroke, size * 5 / 8 + stroke);
    lv_arc_set_bg_angles(cradle, 0, 180);
    lv_obj_remove_style(cradle, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(cradle, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_pad_all(cradle, 0, 0);
    lv_obj_set_style_arc_opa(cradle, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(cradle, stroke, LV_PART_MAIN);
    lv_obj_align(cradle, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *stem = lv_obj_create(box);
    lv_obj_remove_style_all(stem);
    lv_obj_set_size(stem, stroke, size * 7 / 8 - size * 5 / 8);
    lv_obj_set_style_bg_opa(stem, LV_OPA_COVER, 0);
    lv_obj_align(stem, LV_ALIGN_TOP_MID, 0, size * 5 / 8);

    lv_obj_t *base = lv_obj_create(box);
    lv_obj_remove_style_all(base);
    lv_obj_set_size(base, size * 3 / 8, stroke);
    lv_obj_set_style_bg_opa(base, LV_OPA_COVER, 0);
    lv_obj_align(base, LV_ALIGN_TOP_MID, 0, size * 7 / 8);
    return box;
}

static void color_mic(lv_obj_t *icon, uint32_t color)
{
    if (!icon) return;
    for (uint32_t i = 0; i < lv_obj_get_child_count(icon); i++) {
        lv_obj_t *part = lv_obj_get_child(icon, i);
        lv_obj_set_style_bg_color(part, lv_color_hex(color), 0);
        lv_obj_set_style_arc_color(part, lv_color_hex(color), LV_PART_MAIN);
    }
}

static void set_mic_color(uint32_t color)
{
    color_mic(s_mic_icon, color);   /* legacy face icon only */
}

static void on_touch_mic(lv_event_t *e)
{
    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED:
        s_touch_talk_down = true;
        update_tileview_swipe();
        muse_input_touch_talk(true);
        break;
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST:
        muse_input_touch_talk(false);
        s_touch_talk_down = false;
        update_tileview_swipe();
        break;
    default:
        break;
    }
}

/* Icons beside the physical buttons, in place of an instruction caption. */
static void build_button_icons(lv_obj_t *face)
{
    if (muse_board->rim_controls) return;   /* dedicated rim hints below */
    const muse_button_hint_t *t = &muse_board->talk_hint, *a = &muse_board->aux_hint;
    s_mic_icon = make_mic(face, s_tall ? 24 : s_small ? 12 : 26);
    lv_obj_align(s_mic_icon, t->align, t->x, t->y);
    set_mic_color(COLOR_DIM);
    if (muse_board->touch_talk) {
        lv_obj_add_flag(s_mic_icon, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(s_mic_icon, 12);
        for (uint32_t i = 0; i < lv_obj_get_child_count(s_mic_icon); i++) {
            lv_obj_remove_flag(lv_obj_get_child(s_mic_icon, i), LV_OBJ_FLAG_CLICKABLE);
        }
        lv_obj_add_event_cb(s_mic_icon, on_touch_mic, LV_EVENT_ALL, NULL);
    }

    /* Without touch the aux button opens the menu rather than sleeping. A board
     * that leaves aux_hint out has no button to put an icon beside. */
    if (a->align == LV_ALIGN_DEFAULT) {
        return;
    }
    s_aux_icon = make_label(face, s_small ? &lv_font_montserrat_14 : &lv_font_montserrat_28, COLOR_DIM);
    lv_label_set_text(s_aux_icon, muse_board->touch ? LV_SYMBOL_POWER : LV_SYMBOL_LIST);
    lv_obj_align(s_aux_icon, a->align, a->x, a->y);
}

static void on_canvas_clicked(lv_event_t *e)
{
    (void)e;
#if CONFIG_MUSE_BOARD_M5STACK_STOPWATCH
    /* dev.18: as on the caption, a tap on Muse while a reply is waited on
     * (or a note is recording) cancels the wait instead of cheering. */
    muse_mode_t mode = muse_state_mode(NULL);
    if (mode == MUSE_MODE_THINKING || mode == MUSE_MODE_LISTENING) {
        muse_voice_tap_cancel();
        return;
    }
    /* dev.14: a tap on the avatar opens its care panel — but only while
     * the face is the avatar's alone. While a reply is up (the caption,
     * the read layout, a kept full reply, a question's buttons) the tap
     * keeps its old meaning and never opens the panel; in clock mode the
     * canvas is hidden and cannot be tapped at all. */
    if (!s_care_open && !s_clock_mode
        && lv_obj_has_flag(s_caption_lbl, LV_OBJ_FLAG_HIDDEN)
        && (!s_reply_lbl || lv_obj_has_flag(s_reply_lbl, LV_OBJ_FLAG_HIDDEN))
        && !s_full_lbl && !muse_state_ask_pending() && !muse_state_hint_pending()) {
        care_open_panel();
        return;
    }
#endif
    muse_state_make_happy();
}

/* A reply stays up after its turn ends (muse_voice keeps it there now);
 * tapping the text puts it away. Only while idle: a tap during a turn is
 * the turn's own business. */
static void on_caption_clicked(lv_event_t *e)
{
    (void)e;
#if CONFIG_MUSE_BOARD_M5STACK_STOPWATCH
    /* dev.18: tapped while Muse's reply is being waited on (or a note is
     * still being recorded): cancel the wait, back to the plain face. */
    muse_mode_t mode = muse_state_mode(NULL);
    if (mode == MUSE_MODE_THINKING || mode == MUSE_MODE_LISTENING) {
        muse_voice_tap_cancel();
        return;
    }
#endif
    if (muse_state_mode(NULL) == MUSE_MODE_IDLE) {
        muse_state_set_caption("%s", "");
    }
}

#if CONFIG_MUSE_BOARD_M5STACK_STOPWATCH
/* The approval buttons: the answer goes back as a typed turn, the same
 * channel the serial console uses (muse_hatch_text_turn takes the string
 * and frees it). Answering clears the question first, so the buttons go
 * even if the turn can't start. */
static void on_ask_ok(lv_event_t *e)
{
    (void)e;
    muse_state_ask_set(false);
    char *text = strdup("承認します");
    if (text) {
        muse_hatch_text_turn(text);
    }
}

static void on_ask_no(lv_event_t *e)
{
    (void)e;
    muse_state_ask_set(false);
    char *text = strdup("やめます");
    if (text) {
        muse_hatch_text_turn(text);
    }
}
#endif

static const lv_font_t *font_pick(const lv_font_t *full, const lv_font_t *compact)
{
    return s_small ? compact : full;
}

#if CONFIG_MUSE_CJK_FONT_NOTO_JP   /* museStopWatch */
LV_FONT_DECLARE(muse_font_cjk_noto_jp_16)
#define CJK_FONT muse_font_cjk_noto_jp_16
#elif CONFIG_MUSE_CJK_FONT_NOTO
LV_FONT_DECLARE(muse_font_cjk_noto_16)
#define CJK_FONT muse_font_cjk_noto_16
#elif CONFIG_MUSE_CJK_FONT
LV_FONT_DECLARE(muse_font_cjk_16)
#define CJK_FONT muse_font_cjk_16
#endif
#if CONFIG_MUSE_LATIN_FONT
LV_FONT_DECLARE(muse_font_latin_16)
#endif

/* unscii-16 for captions and replies; with CONFIG_MUSE_CJK_FONT, a copy that
 * falls back to Unifont's 16x16 CJK, the same cell, for what unscii lacks. */
static const lv_font_t *caption_font(void)
{
#if CONFIG_MUSE_CJK_FONT
    static lv_font_t font;
    if (!font.get_glyph_dsc) {
        font = lv_font_unscii_16;
        font.fallback = &CJK_FONT;
#if CONFIG_MUSE_LATIN_FONT
        static lv_font_t cjk;   /* and a copy of that for accented letters among CJK */
        cjk = CJK_FONT;
        cjk.fallback = &muse_font_latin_16;
        font.fallback = &cjk;
#endif
    }
    return &font;
#else
    return &lv_font_unscii_16;
#endif
}

/* One CJK character's width in the caption font. unscii's 'M' is 8 px, but
 * the CJK fallback draws 16 px cells, so column math off 'M' makes Japanese
 * lines twice as wide as their box and they clip. Measure a kana through
 * the fallback chain instead; without CJK, 'M' is the cell there is. */
static int caption_cjk_cw(void)
{
#if CONFIG_MUSE_CJK_FONT
    int w = lv_font_get_glyph_width(caption_font(), 0x3042, 0);
    if (w > 0) {
        return w;
    }
#endif
    return lv_font_get_glyph_width(caption_font(), 'M', ' ');
}

/* The small screen's caption band: two lines of unscii-8, as replies are paged
 * there, 2 px apart with 2 px above and below. */
static int caption_band_h(void)
{
    return 2 * lv_font_get_line_height(&lv_font_unscii_8) + 2 + 2 * 2;
}

/* Where the small screen's band sits: full width along the bottom, above the
 * navigation dots on touch screens. The SpotPear's band sits higher, under
 * its avatar, and narrower, so every corner stays inside its 240 px circle. */
#if CONFIG_MUSE_BOARD_SPOTPEAR_ESP32_AI_128
#define SMALL_BAND_W 160
#define SMALL_BAND_Y (-34)
#else
#define SMALL_BAND_W s_w
#define SMALL_BAND_Y ((s_tall || s_tv) ? -30 : -3)
#endif

#if CONFIG_MUSE_CJK_FONT || CONFIG_MUSE_LATIN_FONT
/* A caption's font off a small screen's band: caption_font(), or with
 * CONFIG_MUSE_LATIN_FONT, Unifont's 8x16 for the whole of a caption with
 * accented letters and no CJK. */
static const lv_font_t *caption_font_for(const char *text)
{
#if CONFIG_MUSE_LATIN_FONT
#if CONFIG_MUSE_CJK_FONT
    if (muse_text_has_cjk(text)) {
        return caption_font();   /* its CJK falls back to the accented letters */
    }
#endif
    if (muse_text_has_latin(text)) {
        return &muse_font_latin_16;
    }
#endif
    (void)text;
    return caption_font();
}

#if CONFIG_MUSE_CJK_FONT
#define CJK_BAND_PAD 6          /* px each side of a CJK caption on a small screen */
#define CJK_LINE_SPACE 4
static int s_cjk_lines;         /* lines of CJK the small screen's caption band holds */
#endif

/* The small screen's band: two lines of unscii-8, or s_cjk_lines of the 16 px
 * caption font with room between them for CJK, or for accented letters one
 * line of Unifont's 8x16 in the same band as unscii-8's two. */
static void set_caption_font(const char *caption)
{
#if CONFIG_MUSE_SMALL_FULL_REPLY
    if (s_full_reply) {
        return;   /* the reply keeps the screen: set_full_font() once its text is in */
    }
#endif
#if CONFIG_MUSE_CJK_FONT
    if (muse_text_has_cjk(caption)) {
        int lines = s_cjk_lines;
        int line_h = lv_font_get_line_height(caption_font());
        int space = lines > 1 ? CJK_LINE_SPACE : 2;
        lv_obj_set_style_text_font(s_caption_lbl, caption_font(), 0);
        lv_obj_set_style_text_line_space(s_caption_lbl, space, 0);
        lv_obj_set_style_pad_hor(s_caption_lbl, CJK_BAND_PAD, 0);
        lv_obj_set_height(s_caption_lbl, lines * line_h + (lines - 1) * space + 6);
        lv_label_set_long_mode(s_caption_lbl, LV_LABEL_LONG_MODE_DOTS);
        return;
    }
#endif
    const lv_font_t *font = &lv_font_unscii_8;
#if CONFIG_MUSE_LATIN_FONT
    if (muse_text_has_latin(caption)) {
        font = &muse_font_latin_16;
    }
#endif
    lv_obj_set_style_text_font(s_caption_lbl, font, 0);
    lv_obj_set_style_text_line_space(s_caption_lbl, 2, 0);
    lv_obj_set_style_pad_hor(s_caption_lbl, 0, 0);
    lv_obj_set_height(s_caption_lbl, caption_band_h());
    /* Replies are paged to fit; the firmware's own captions are whole, so an
     * accented one longer than the line rolls by rather than ending in dots. */
    lv_label_set_long_mode(s_caption_lbl, font == &lv_font_unscii_8 ? LV_LABEL_LONG_MODE_DOTS
                                                                    : LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
}
#endif

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(l, "");
    return l;
}

static void show_speaker(bool on)
{
    /* Off is the one that stands out, like a lit flashlight button. */
    lv_obj_set_style_bg_color(s_speaker, lv_color_hex(on ? COLOR_METER_OFF : COLOR_LIT), 0);
    lv_obj_set_style_text_color(s_speaker_icon, lv_color_hex(on ? COLOR_DIM : 0x000000), 0);
    lv_label_set_text(s_speaker_icon, on ? LV_SYMBOL_VOLUME_MAX : LV_SYMBOL_MUTE);
    s_shown_speaker = on;
}

static void set_speaker_size(void *obj, int32_t px)
{
    lv_obj_set_size(obj, px, px);
}

/* Swells over the long press, so the toggle lands as it reaches full size. */
static void speaker_grow(bool grow)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_speaker);
    lv_anim_set_exec_cb(&a, set_speaker_size);
    lv_anim_set_values(&a, lv_obj_get_width(s_speaker), SPEAKER_PX + (grow ? SPEAKER_GROW_PX : 0));
    lv_anim_set_duration(&a, grow ? SPEAKER_HOLD_MS : 150);
    lv_anim_start(&a);
}

/* Touch and hold to toggle, like the lock screen's flashlight; a tap only says so. */
static void on_speaker_event(lv_event_t *e)
{
    bool on = muse_settings_speaker_on();
    bool idle = muse_state_mode(NULL) == MUSE_MODE_IDLE;   /* don't cover a reply's captions */
    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED:
        speaker_grow(true);
        break;
    case LV_EVENT_LONG_PRESSED:
        muse_settings_set_speaker_on(!on);
        show_speaker(!on);
        if (idle) {
            muse_state_set_hint(on ? "SPEAKER OFF" : "SPEAKER ON");
        }
        break;
    case LV_EVENT_SHORT_CLICKED:
        if (idle) {
            muse_state_set_hint(on ? "HOLD TO MUTE" : "HOLD TO UNMUTE");
        }
        break;
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST:
        speaker_grow(false);
        break;
    default:
        break;
    }
}

/* Upper left by the status icons, where a thumb finds it without covering the reply. */
static void build_speaker(lv_obj_t *face, int x, int y)
{
    s_speaker = lv_obj_create(face);
    lv_obj_remove_style_all(s_speaker);
    lv_obj_set_size(s_speaker, SPEAKER_PX, SPEAKER_PX);
    lv_obj_set_style_radius(s_speaker, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_speaker, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_speaker, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_ext_click_area(s_speaker, 12);   /* a fingertip is bigger than the circle */
    static const lv_event_code_t EVENTS[] = { LV_EVENT_PRESSED, LV_EVENT_LONG_PRESSED, LV_EVENT_SHORT_CLICKED,
                                              LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST };
    for (size_t i = 0; i < sizeof(EVENTS) / sizeof(EVENTS[0]); i++) {
        lv_obj_add_event_cb(s_speaker, on_speaker_event, EVENTS[i], NULL);
    }
    s_speaker_icon = make_label(s_speaker, &lv_font_montserrat_28, COLOR_DIM);
    lv_obj_center(s_speaker_icon);
    lv_obj_align(s_speaker, LV_ALIGN_CENTER, x, y);
    show_speaker(muse_settings_speaker_on());
}

static void set_canvas_px(int px)
{
    if ((int)s_muse_src.header.w == px) {
        return;
    }
    s_muse_src.header.w = px;
    s_muse_src.header.h = px;
    s_muse_src.header.stride = px * sizeof(uint16_t);
    muse_pixel_set_size(px);
    lv_image_set_src(s_canvas, &s_muse_src);   /* picks up the new size */
}

static void move_muse_t(void *obj, int32_t t)
{
    (void)obj;
    set_canvas_px(s_from_px + (s_to_px - s_from_px) * t / 256);
    s_muse_y = s_from_y + (s_to_y - s_from_y) * t / 256;
    lv_obj_align(s_canvas, LV_ALIGN_CENTER, 0, s_muse_y);
}

/* Eases Muse to `px` centred at `y`, from wherever it is now. */
static void move_muse(int px, int y)
{
    s_from_px = s_muse_src.header.w;
    s_from_y = s_muse_y;
    s_to_px = px;
    s_to_y = y;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_canvas);
    lv_anim_set_exec_cb(&a, move_muse_t);
    lv_anim_set_values(&a, 0, 256);
    lv_anim_set_duration(&a, ANSWER_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

/* Switches to answer layout `which`, or back to the usual one for -1. */
static void set_answer(int which)
{
    const answer_layout_t *was = s_answer >= 0 ? &s_answers[s_answer] : NULL;
    const answer_layout_t *l = which >= 0 ? &s_answers[which] : NULL;
    s_answer = which;
    const size_t n = sizeof(s_answers[0].hides) / sizeof(s_answers[0].hides[0]);
    for (size_t i = 0; was && i < n; i++) {
        if (was->hides[i]) {
            lv_obj_remove_flag(was->hides[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    for (size_t i = 0; l && i < n; i++) {
        if (l->hides[i]) {
            lv_obj_add_flag(l->hides[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (!muse_board->round && s_ring) {
        lv_obj_set_flag(s_ring, LV_OBJ_FLAG_HIDDEN, l != NULL);   /* the reply runs past a rectangle's ring */
    }
    if (l) {
        lv_obj_set_size(s_reply_lbl, l->w, l->h);
        lv_obj_align(s_reply_lbl, LV_ALIGN_CENTER, 0, l->top + l->h / 2);
        lv_obj_set_style_text_align(s_reply_lbl, l->align, 0);
    }
    move_muse(l ? l->px : face_px(), l ? l->y : s_big_y);
}

/* Whether a reply `w` px wide fits across the screen `y` px from the centre. */
static bool fits_across(int w, int y, int ring_in)
{
    if (!muse_board->round) {
        return w <= s_w - 16;
    }
    int r = ring_in - 6;
    return w * w / 4 + y * y <= r * r;
}

/* How far down a reply `w` px wide can reach: inside the ring, above the page dots. */
static int reply_bottom(int w, int ring_in)
{
    int bottom = s_h / 2 - 28;
    if (muse_board->round) {
        int r = ring_in - 6;
        int chord = w / 2 < r ? (int)sqrtf((float)(r * r - w * w / 4)) : 0;
        bottom = chord < bottom ? chord : bottom;
    }
    return bottom;
}

static void set_reply_box(answer_layout_t *l, int cols, int lines, int top, int cw, int pitch)
{
    l->cols = cols;
    l->lines = lines;
    l->w = cols * cw;
    l->h = lines * pitch - CAPTION_LINE_SPACE;
    l->top = top;
}

/* The hint icons a layout's reply would cover go while it's up. */
static void add_hides(answer_layout_t *l, int n)
{
    lv_area_t box = {
        .x1 = s_w / 2 - l->w / 2, .y1 = s_h / 2 + l->top,
        .x2 = s_w / 2 + l->w / 2 - 1, .y2 = s_h / 2 + l->top + l->h - 1,
    };
    lv_obj_t *const hints[] = { s_mic_icon, s_aux_icon };
    for (size_t i = 0; i < 2; i++) {
        if (!hints[i]) {
            continue;   /* no aux icon on this board */
        }
        lv_area_t a;
        lv_obj_get_coords(hints[i], &a);
        if (a.x1 <= box.x2 && a.x2 >= box.x1 && a.y1 <= box.y2 && a.y2 >= box.y1) {
            l->hides[n++] = hints[i];
        }
    }
}

/*
 * The answer layouts, both with Muse centred. Heard: Muse a size smaller,
 * where it was if there's room, over three lines at the bottom. Read: Muse
 * small under the status line, and under it and the speaker button the
 * biggest page of reply text that fits inside the ring.
 */
static void build_answer(lv_obj_t *face, int ring_in)
{
    int spk_r = (SPEAKER_PX + SPEAKER_GROW_PX) / 2;
    int spk_x = -s_w / 2 + 8 + spk_r, spk_y = -s_h / 2 + 8 + spk_r;
    if (muse_board->round) {
        spk_y = -ring_in * 5 / 8;
        int d = ring_in - spk_r - 4;   /* just inside the ring, even when swollen */
        spk_x = -(int)sqrtf((float)(d * d - spk_y * spk_y));
    }
    const lv_font_t *font = caption_font();
    int cw = caption_cjk_cw();
    int pitch = lv_font_get_line_height(font) + CAPTION_LINE_SPACE;

    answer_layout_t *l = &s_answers[ANSWER_HEARD];
    int cell = s_canvas_px / MUSE_PX_W - 1;
    cell = cell > MINI_CELL_PX ? cell : MINI_CELL_PX;
    l->px = MUSE_PX_W * cell;
    l->y = s_big_y;
    l->align = LV_TEXT_ALIGN_CENTER;
    int h = 3 * pitch - CAPTION_LINE_SPACE;
    int art_bottom = l->y + l->px / 2 - ART_BLANK_ROWS * cell;
    set_reply_box(l, CAPTION_W / cw, 3, reply_bottom(CAPTION_W, ring_in) - h, cw, pitch);
    for (int c = 24; c > l->cols; c--) {   /* wider if it still clears Muse */
        int top = reply_bottom(c * cw, ring_in) - h;
        if (top >= art_bottom + 6 && fits_across(c * cw, top, ring_in)) {
            set_reply_box(l, c, 3, top, cw, pitch);
            break;
        }
    }
    if (l->top < art_bottom + 6) {
        l->y -= art_bottom + 6 - l->top;   /* no room under Muse: it moves up */
    }

    l = &s_answers[ANSWER_READ];
    int status_bottom = (muse_board->rim_controls ? 40 : 20 + s_dy) + 16 - s_h / 2;
    l->px = MUSE_PX_W * MINI_CELL_PX;
    l->y = status_bottom + 2 + l->px / 2;   /* its sparkles clear of the status line */
    l->align = LV_TEXT_ALIGN_LEFT;
    art_bottom = l->y + l->px / 2 - ART_BLANK_ROWS * MINI_CELL_PX;
    int top = (art_bottom > spk_y + spk_r ? art_bottom : spk_y + spk_r) + 8;
    set_reply_box(l, muse_board->rim_controls ? 13 : 16, 2, top, cw, pitch);
    /* The widest page isn't the biggest: a round screen narrows towards the bottom. */
    /* Keep read replies in a central column clear of dedicated rim hints. */
    int max_cols = muse_board->rim_controls ? 13 : 24;
    for (int c = 12; c <= max_cols && fits_across(c * cw, top, ring_in); c++) {
        int n = (reply_bottom(c * cw, ring_in) - top + CAPTION_LINE_SPACE) / pitch;
        /* A third of the caption spare for characters wider than a byte. */
        while ((c + 1) * n > MUSE_CAPTION_MAX * 2 / 3) {
            n--;
        }
        if (c * n > l->cols * l->lines) {
            set_reply_box(l, c, n, top, cw, pitch);
        }
    }
    ESP_LOGI(TAG, "reply pages: %d x %d heard, %d x %d read", s_answers[ANSWER_HEARD].cols,
             s_answers[ANSWER_HEARD].lines, l->cols, l->lines);

    s_reply_lbl = make_label(face, font, COLOR_CAPTION);
    lv_obj_set_style_text_line_space(s_reply_lbl, CAPTION_LINE_SPACE, 0);
    /* Pages come wrapped to fit; the transcript while thinking doesn't. */
    lv_label_set_long_mode(s_reply_lbl, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_remove_flag(s_reply_lbl, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_reply_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_reply_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_reply_lbl, on_caption_clicked, LV_EVENT_CLICKED, NULL);
#if CONFIG_MUSE_BOARD_SENSECAP_WATCHER
    s_review_position = make_label(face, &lv_font_unscii_8, COLOR_DIM);
    lv_obj_align(s_review_position, LV_ALIGN_BOTTOM_MID, 0, -30);
    lv_obj_add_flag(s_review_position, LV_OBJ_FLAG_HIDDEN);
#endif

    if (muse_board->touch && !muse_board->rim_controls) {
        build_speaker(face, spk_x, spk_y);
    }

    /* Out of the way while a page is up: the mode name, where Muse was. */
    lv_obj_update_layout(face);
    s_answers[ANSWER_READ].hides[0] = s_state_lbl;
    s_answers[ANSWER_READ].hides[1] = s_name_lbl;   /* the reply takes the top too */
    add_hides(&s_answers[ANSWER_READ], 2);
    add_hides(&s_answers[ANSWER_HEARD], 0);
}

/*
 * A full circle's arc masks every pixel of every row it's asked to draw, hole
 * and corners included, which costs a full-screen redraw (a swipe) several ms.
 * So the ring is drawn a slab of rows at a time, each only as wide as the ring
 * is there: where a slab crosses the hole, a left piece and a right one.
 */
#define RING_SLAB_ROWS 52

static void on_ring_draw(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    const lv_area_t clip = layer->_clip_area;
    lv_area_t c;
    lv_obj_get_coords(s_ring, &c);
    int32_t cx = (c.x1 + c.x2) / 2;
    int32_t cy = (c.y1 + c.y2) / 2;
    int32_t out = lv_area_get_width(&c) / 2 + 1;                                     /* with antialiasing */
    int32_t hole = out - 1 - lv_obj_get_style_arc_width(s_ring, LV_PART_MAIN) - 2;   /* without */
    lv_event_stop_processing(e);
    for (int32_t y = LV_MAX(clip.y1, cy - out); y <= LV_MIN(clip.y2, cy + out); y += RING_SLAB_ROWS) {
        int32_t y2 = LV_MIN(y + RING_SLAB_ROWS - 1, clip.y2);
        int32_t near = y <= cy && cy <= y2 ? 0 : LV_MIN(LV_ABS(y - cy), LV_ABS(y2 - cy));
        int32_t far = LV_MAX(LV_ABS(y - cy), LV_ABS(y2 - cy));
        int32_t ow = near < out ? (int32_t)ceilf(sqrtf((float)(out * out - near * near))) : 0;
        int32_t iw = far < hole ? (int32_t)sqrtf((float)(hole * hole - far * far)) : 0;
        lv_area_t pieces[2] = {
            { cx - ow, y, iw ? cx - iw : cx + ow, y2 },
            { cx + iw, y, cx + ow, y2 },
        };
        for (int i = 0; i < (iw ? 2 : 1); i++) {
            if (lv_area_intersect(&layer->_clip_area, &clip, &pieces[i])) {
                lv_obj_event_base(NULL, e);   /* the arc's own drawing */
            }
        }
    }
    layer->_clip_area = clip;
}

/* The long layout's caption font, for the pager (muse_chat_set_caption_px):
 * LVGL measures a label's lines the same way, and its built-in fonts are
 * read-only, so this is safe off the LVGL task. */
static int long_glyph_px(uint32_t letter, uint32_t next)
{
    return lv_font_get_glyph_width(&lv_font_montserrat_16, letter, next);
}

#if CONFIG_MUSE_CJK_FONT || CONFIG_MUSE_LATIN_FONT
/* The long layout's font for a caption: Montserrat, or as caption_font_for()
 * picks for one with CJK or accented letters, which are paged by characters. */
static const lv_font_t *long_caption_font(const char *text)
{
#if CONFIG_MUSE_CJK_FONT
    if (muse_text_has_cjk(text)) {
        return caption_font();
    }
#endif
#if CONFIG_MUSE_LATIN_FONT
    if (muse_text_has_latin(text)) {
        return &muse_font_latin_16;
    }
#endif
    return &lv_font_montserrat_16;
}
#endif

#if CONFIG_MUSE_SMALL_FULL_REPLY
/* The full-screen reply's font for the page the caption has now: the Text size
 * setting's, or as on the band, the caption font for a page with CJK and
 * Unifont's 8x16 for one with accented letters, the fonts that draw them. */
static void set_full_font(void)
{
#if CONFIG_MUSE_CJK_FONT || CONFIG_MUSE_LATIN_FONT
    const char *text = lv_label_get_text(s_caption_lbl);
#endif
#if CONFIG_MUSE_CJK_FONT
    if (muse_text_has_cjk(text)) {
        lv_obj_set_style_text_font(s_caption_lbl, caption_font(), 0);
        lv_obj_set_style_text_line_space(s_caption_lbl, CJK_LINE_SPACE, 0);
        return;
    }
#endif
    const lv_font_t *font = s_full_font;
#if CONFIG_MUSE_LATIN_FONT
    if (muse_text_has_latin(text)) {
        font = &muse_font_latin_16;
    }
#endif
    lv_obj_set_style_text_font(s_caption_lbl, font, 0);
    lv_obj_set_style_text_line_space(s_caption_lbl, CAPTION_LINE_SPACE, 0);
}

/* The full-screen reply's font, for the pager (muse_chat_set_caption_px), as
 * long_glyph_px() is for the long layout. */
static int full_glyph_px(uint32_t letter, uint32_t next)
{
    return lv_font_get_glyph_width(s_full_font, letter, next);
}

/* Lines of `font` with `space` between them in the full-screen caption. */
static int full_rows(const lv_font_t *font, int space)
{
    return (s_full_reply_h - 4 + space) / (lv_font_get_line_height(font) + space);
}

/* Picks the reply font for the Text size setting and pages replies to fit it:
 * unscii-8's fixed cell (the band's font), or Montserrat 14 or 20. The pager
 * wraps each line to the label's width in pixels as the label measures it
 * (set up in build_screen): a line never wraps again on screen, and every page
 * fits at any size, capitals included. */
static void apply_text_size(void)
{
    static const lv_font_t *const SIZES[] = {
        [MUSE_TEXT_SMALL] = &lv_font_unscii_8,
        [MUSE_TEXT_MEDIUM] = &lv_font_montserrat_14,
        [MUSE_TEXT_LARGE] = &lv_font_montserrat_20,
    };
    int size = muse_settings_text_size();
    if (size == s_full_size) {
        return;
    }
    s_full_size = size;
    s_full_font = SIZES[size];
    int lines = full_rows(s_full_font, CAPTION_LINE_SPACE);
    if (lines < 1) {
        lines = 1;   /* the columns below divide by it */
    }
    int cols;
    if (s_full_font == &lv_font_unscii_8) {
        cols = (s_w - 6) / 8;
        /* A third of the caption spare for characters wider than a byte. */
        while (lines > 1 && (cols + 1) * lines > MUSE_CAPTION_MAX * 2 / 3) {
            lines--;
        }
    } else {
        /* The same spare: this caps a line of narrow letters. */
        cols = MUSE_CAPTION_MAX * 2 / 3 / lines - 1;
    }
    muse_state_set_page(cols, lines);
    /* A reply with CJK or accented letters takes their font's columns, three
     * bytes of the caption a character at most, but its pages without any
     * show in the Text size font, so no more of its rows than that has. */
#if CONFIG_MUSE_CJK_FONT
    int cjk_cols = (s_w - 6) / caption_cjk_cw();
    int cjk_lines = full_rows(caption_font(), CJK_LINE_SPACE);
    cjk_lines = cjk_lines < lines ? cjk_lines : lines;
    while (cjk_lines > 1 && (3 * cjk_cols + 1) * cjk_lines > MUSE_CAPTION_MAX - 1) {
        cjk_lines--;
    }
    muse_state_set_cjk_page(cjk_cols, cjk_lines);
#endif
#if CONFIG_MUSE_LATIN_FONT
    int latin_cols = (s_w - 6) / lv_font_get_glyph_width(&muse_font_latin_16, 'M', ' ');
    int latin_lines = full_rows(&muse_font_latin_16, CAPTION_LINE_SPACE);
    latin_lines = latin_lines < lines ? latin_lines : lines;
    while (latin_lines > 1 && (3 * latin_cols + 1) * latin_lines > MUSE_CAPTION_MAX - 1) {
        latin_lines--;
    }
    muse_state_set_latin_page(latin_cols, latin_lines);
#endif
    if (s_full_reply) {
        set_full_font();
    }
    ESP_LOGI(TAG, "full-screen reply pages: %d x %d", cols, lines);
}
#endif

#if CONFIG_MUSE_BOARD_M5STACK_STOPWATCH
static void on_hist_page(lv_event_t *e)
{
    (void)e;
    muse_review_step(1);   /* a tap on the text turns its page */
}

static void on_hist_older(lv_event_t *e)
{
    (void)e;
    muse_review_entry_step(1);
}

static void on_hist_newer(lv_event_t *e)
{
    (void)e;
    muse_review_entry_step(-1);
}

static void on_hist_chat_prev(lv_event_t *e)
{
    (void)e;
    if (msw_chat_cycle) msw_chat_cycle(-1);
}

static void on_hist_chat_next(lv_event_t *e)
{
    (void)e;
    if (msw_chat_cycle) msw_chat_cycle(1);
}

static lv_obj_t *hist_button(lv_obj_t *parent, const lv_font_t *font, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x241d40), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, 16, 0);
    lv_obj_t *l = make_label(b, font, COLOR_LIT);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

/*
 * The reply history tile (above the face): the chat switcher at the top —
 * the way out of the Settings > Muse > Side chat trip — then the selected
 * reply of the last few, paged like the face's read layout but wider, with
 * buttons to the older and newer replies. update_history() keeps it filled.
 */
static void build_history(lv_obj_t *tile)
{
    const lv_font_t *font = caption_font();
    int cw = caption_cjk_cw();
    int pitch = lv_font_get_line_height(font) + CAPTION_LINE_SPACE;
    int cols = cw > 0 ? 320 / cw : 24;
    int lines = 11;
    /* A page must fit the review's page buffer, as the face's layouts cap theirs. */
    while (lines > 1 && (cols + 1) * lines > MUSE_CAPTION_MAX * 2 / 3) lines--;
    s_hist_cols = cols;
    s_hist_lines = lines;
    /* CJK takes the caption font's wider fallback cell. */
    int cjk_cw = lv_font_get_glyph_width(font, 0x3042, 0);
    int cjk_cols = cjk_cw > 0 ? 320 / cjk_cw : cols;
    int cjk_lines = lines;
    while (cjk_lines > 1 && (3 * cjk_cols + 1) * cjk_lines > MUSE_CAPTION_MAX - 1) cjk_lines--;
    s_hist_cjk_cols = cjk_cols;
    s_hist_cjk_lines = cjk_lines;
#if CONFIG_MUSE_LATIN_FONT
    int latin_cw = lv_font_get_glyph_width(&muse_font_latin_16, 'M', ' ');
    int latin_cols = latin_cw > 0 ? 320 / latin_cw : cols;
    int latin_lines = lines;
    while (latin_lines > 1 && (3 * latin_cols + 1) * latin_lines > MUSE_CAPTION_MAX - 1) latin_lines--;
    s_hist_latin_cols = latin_cols;
    s_hist_latin_lines = latin_lines;
#endif
    int body_h = lines * pitch - CAPTION_LINE_SPACE;

    /* The chat in use, with a button each way: tap to talk somewhere else. */
    lv_obj_t *prev = hist_button(tile, &lv_font_montserrat_20, LV_SYMBOL_LEFT, on_hist_chat_prev);
    lv_obj_set_size(prev, 48, 48);
    lv_obj_set_style_radius(prev, LV_RADIUS_CIRCLE, 0);
    lv_obj_align(prev, LV_ALIGN_TOP_MID, -118, 40);
    lv_obj_t *next = hist_button(tile, &lv_font_montserrat_20, LV_SYMBOL_RIGHT, on_hist_chat_next);
    lv_obj_set_size(next, 48, 48);
    lv_obj_set_style_radius(next, LV_RADIUS_CIRCLE, 0);
    lv_obj_align(next, LV_ALIGN_TOP_MID, 118, 40);
    s_hist_chat = make_label(tile, font, COLOR_LIT);
    lv_obj_set_size(s_hist_chat, 180, LV_SIZE_CONTENT);
    lv_label_set_long_mode(s_hist_chat, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(s_hist_chat, LV_ALIGN_TOP_MID, 0, 54);
    lv_label_set_text(s_hist_chat, msw_chat_name ? msw_chat_name() : "Main chat");

    lv_obj_t *title = make_label(tile, font, COLOR_DIM);
    lv_label_set_text(title, "返信履歴");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 106);

    s_hist_body = make_label(tile, font, COLOR_CAPTION);
    lv_obj_set_size(s_hist_body, 320, body_h);
    lv_obj_set_style_text_align(s_hist_body, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_long_mode(s_hist_body, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_remove_flag(s_hist_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_hist_body, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_hist_body, on_hist_page, LV_EVENT_CLICKED, NULL);
    lv_obj_align(s_hist_body, LV_ALIGN_TOP_MID, 0, 146);

    s_hist_pos = make_label(tile, font, COLOR_DIM);
    lv_obj_align(s_hist_pos, LV_ALIGN_TOP_MID, 0, 146 + body_h + 12);

    lv_obj_t *older = hist_button(tile, font, "まえの返信", on_hist_older);
    lv_obj_set_size(older, 124, 48);
    lv_obj_align(older, LV_ALIGN_BOTTOM_MID, -72, -30);
    lv_obj_t *newer = hist_button(tile, font, "つぎの返信", on_hist_newer);
    lv_obj_set_size(newer, 124, 48);
    lv_obj_align(newer, LV_ALIGN_BOTTOM_MID, 72, -30);
}

/* While the history tile is up: it owns the caption pager (the face sizes it
 * again on the way back, via s_page_restore) and the review selection. */
static void update_history(void)
{
    if (!s_history) return;
    bool on = s_tv && lv_tileview_get_tile_active(s_tv) == s_history;
    static bool was_on;
    if (on && !was_on) {
        muse_review_open();
        muse_state_set_page(s_hist_cols, s_hist_lines);
#if CONFIG_MUSE_CJK_FONT
        muse_state_set_cjk_page(s_hist_cjk_cols, s_hist_cjk_lines);
#endif
#if CONFIG_MUSE_LATIN_FONT
        muse_state_set_latin_page(s_hist_latin_cols, s_hist_latin_lines);
#endif
        s_hist_view.stale = true;
    } else if (!on && was_on) {
        muse_review_close();
#if CONFIG_MUSE_CJK_FONT
        muse_state_set_cjk_page(0, 0);   /* 0: the face's pages fall back as before */
#endif
#if CONFIG_MUSE_LATIN_FONT
        muse_state_set_latin_page(0, 0);
#endif
        s_page_restore = true;
    }
    was_on = on;
    if (!on) return;

    if (msw_chat_name) {
        const char *name = msw_chat_name();
        if (name && strcmp(name, lv_label_get_text(s_hist_chat)) != 0) {
            lv_label_set_text(s_hist_chat, name);
        }
    }
    int at = 0, count = 0;
    muse_review_info(&at, &count);
    bool changed;
    bool active = muse_review_frame(&s_hist_view, s_hist_buf, sizeof(s_hist_buf), &changed);
    if (!active) {
        if (count) {
            /* A new reply landed while reading: it closed the review (as on
             * the Watcher); open it again at the new reply. */
            muse_review_open();
            s_hist_view.stale = true;
        } else if (strcmp(lv_label_get_text(s_hist_body), "まだ返信がありません") != 0) {
            lv_label_set_text(s_hist_body, "まだ返信がありません");
            lv_label_set_text(s_hist_pos, "");
        }
        return;
    }
    if (changed) {
        lv_label_set_text(s_hist_body, s_hist_buf);
    }
    char pos[48];
    snprintf(pos, sizeof(pos), "%d/%d件目・%d/%dページ", at, count, s_hist_view.page, s_hist_view.pages);
    if (strcmp(pos, lv_label_get_text(s_hist_pos)) != 0) {
        lv_label_set_text(s_hist_pos, pos);
    }
}

/*
 * The quick settings tile (above the face, swipe down for it): the two
 * sliders a hand reaches for most — brightness and volume — applied as
 * they move (update_power and the audio path poll the settings), and
 * (dev.14) the three sound-mode buttons. Avatar switching used to live
 * on the tile below the face; it is in Settings now, and the reply
 * history took that tile's place.
 */
static void on_quick_bright(lv_event_t *e)
{
    muse_settings_set_brightness((int)lv_slider_get_value(lv_event_get_target(e)));
}

static void on_quick_vol(lv_event_t *e)
{
    muse_settings_set_volume((int)lv_slider_get_value(lv_event_get_target(e)));
}

/* dev.14: the sound modes moved here from the yellow button (it switches
 * the face now) — the same snd_mode setting Settings and the speaker rim
 * icon read, saved the moment a button is tapped. */
static void on_quick_snd(lv_event_t *e)
{
    muse_settings_set_sound_mode((int)(intptr_t)lv_event_get_user_data(e));
}

/* Settings lives one tile past the dashboard now; this button is its door
 * (the left swipe from the face lands on the dashboard instead). */
static void on_quick_settings(lv_event_t *e)
{
    (void)e;
    if (s_tv && s_settings) {
        lv_tileview_set_tile(s_tv, s_settings, LV_ANIM_ON);
    }
}

/* dev.16: the countdown timer's door on this tile (the launcher's cell
 * and the yellow button's long press are the other two). */
static void on_quick_timer(lv_event_t *e)
{
    (void)e;
    muse_ui_request_timer();
}

/* dev.22: the pomodoro's door on this tile, beside the timer's. */
static void on_quick_pomodoro(lv_event_t *e)
{
    (void)e;
    muse_ui_request_pomodoro();
}

static lv_obj_t *quick_slider(lv_obj_t *tile, const lv_font_t *font, const char *name, int y,
                              int min, int max, int val, lv_event_cb_t cb, lv_obj_t **val_out)
{
    lv_obj_t *l = make_label(tile, font, COLOR_LIT);
    lv_label_set_text(l, name);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_t *sl = lv_slider_create(tile);
    lv_obj_set_size(sl, 280, 12);
    lv_slider_set_range(sl, min, max);
    lv_slider_set_value(sl, val, LV_ANIM_OFF);
    lv_obj_align(sl, LV_ALIGN_TOP_MID, 0, y + 34);
    lv_obj_add_event_cb(sl, cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_t *v = make_label(tile, font, COLOR_DIM);
    lv_obj_align(v, LV_ALIGN_TOP_MID, 0, y + 58);
    *val_out = v;
    return sl;
}

static void build_quick(lv_obj_t *tile)
{
    const lv_font_t *font = caption_font();
    lv_obj_t *title = make_label(tile, font, COLOR_LIT);
    lv_label_set_text(title, "クイック設定");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 36);
    s_quick_bright = quick_slider(tile, font, "明るさ", 82, 10, 100, muse_settings_brightness(),
                                  on_quick_bright, &s_quick_bright_val);
    s_quick_vol = quick_slider(tile, font, "音量", 178, 0, 100, muse_settings_volume(),
                               on_quick_vol, &s_quick_vol_val);
    /* dev.14: the sound modes, three buttons under the sliders — the
     * live one lit (update_quick keeps them honest). */
    lv_obj_t *snd = make_label(tile, font, COLOR_LIT);
    lv_label_set_text(snd, "音モード");
    lv_obj_align(snd, LV_ALIGN_TOP_MID, 0, 262);
    static const char *const SND_NAMES[3] = { "ミュート", "振動のみ", "音あり" };   /* by MUSE_SOUND_* */
    for (int mode = 0; mode < 3; mode++) {
        lv_obj_t *b = lv_btn_create(tile);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, 104, 46);
        lv_obj_set_style_radius(b, 16, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x241d40), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_t *l = make_label(b, font, COLOR_LIT);
        lv_label_set_text(l, SND_NAMES[mode]);
        lv_obj_center(l);
        lv_obj_align(b, LV_ALIGN_TOP_MID, (1 - mode) * 112, 292);
        lv_obj_add_event_cb(b, on_quick_snd, LV_EVENT_CLICKED, (void *)(intptr_t)mode);
        s_quick_snd[mode] = b;
        s_quick_snd_lbl[mode] = l;
    }
    lv_obj_t *settings = hist_button(tile, font, "設定", on_quick_settings);
    lv_obj_set_size(settings, 104, 48);
    lv_obj_align(settings, LV_ALIGN_TOP_MID, -112, 356);
    lv_obj_t *timer = hist_button(tile, font, "タイマー", on_quick_timer);
    lv_obj_set_size(timer, 104, 48);
    lv_obj_align(timer, LV_ALIGN_TOP_MID, 0, 356);
    /* dev.22: the pomodoro joins the row; three across at the sound
     * buttons' width, so 設定 and タイマー narrow to match. */
    lv_obj_t *pomo = hist_button(tile, font, "ポモドーロ", on_quick_pomodoro);
    lv_obj_set_size(pomo, 104, 48);
    lv_obj_align(pomo, LV_ALIGN_TOP_MID, 112, 356);
    lv_obj_t *hint = make_label(tile, font, COLOR_DIM);
    lv_label_set_text(hint, "上にスワイプでもどる");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -30);
}

/* While the quick tile is up, keep its sliders honest (Settings changes
 * the same values) and the sound buttons lit on the live mode (Settings'
 * sound page changes it too). A slider being dragged is left alone: its
 * own event applies the value. */
static void update_quick(void)
{
    if (!s_quick || !s_tv || lv_tileview_get_tile_active(s_tv) != s_quick) return;
    if (!lv_obj_has_state(s_quick_bright, LV_STATE_PRESSED)
        && lv_slider_get_value(s_quick_bright) != muse_settings_brightness()) {
        lv_slider_set_value(s_quick_bright, muse_settings_brightness(), LV_ANIM_OFF);
    }
    if (!lv_obj_has_state(s_quick_vol, LV_STATE_PRESSED)
        && lv_slider_get_value(s_quick_vol) != muse_settings_volume()) {
        lv_slider_set_value(s_quick_vol, muse_settings_volume(), LV_ANIM_OFF);
    }
    int snd = muse_settings_sound_mode();
    if (snd != s_quick_snd_shown) {
        s_quick_snd_shown = snd;
        for (int i = 0; i < 3; i++) {
            bool on = i == snd;
            lv_obj_set_style_bg_color(s_quick_snd[i], lv_color_hex(on ? COLOR_ACCENT : 0x241d40), 0);
            lv_obj_set_style_text_color(s_quick_snd_lbl[i], lv_color_hex(on ? 0x17122b : COLOR_LIT), 0);
        }
    }
    char buf[16];
    snprintf(buf, sizeof(buf), "%d%%", muse_settings_brightness());
    if (strcmp(buf, lv_label_get_text(s_quick_bright_val)) != 0) lv_label_set_text(s_quick_bright_val, buf);
    snprintf(buf, sizeof(buf), "%d%%", muse_settings_volume());
    if (strcmp(buf, lv_label_get_text(s_quick_vol_val)) != 0) lv_label_set_text(s_quick_vol_val, buf);
}

/*
 * dev.14: the care panel and the clock face.
 *
 * Tapping the avatar (while no reply is up — see on_canvas_clicked)
 * opens the care panel: feed or pet みゃおっち, watch its belly/mood
 * meters and its bond. Feeding and petting close the panel and answer
 * with the happy pose; feeding also drops food over the avatar while it
 * eats. The yellow button's clock face is the other half of this
 * section: big time, date, battery, and nothing else — replies still
 * take the face over while they run, exactly as on the avatar.
 */

static void care_refresh(void)
{
    static int belly = -1, mood = -1, bond = -1;
    if ((int)s_care.belly == belly && (int)s_care.mood == mood && (int)s_care.bond == bond) {
        return;   /* nothing moved: leave the objects alone */
    }
    belly = s_care.belly;
    mood = s_care.mood;
    bond = s_care.bond;
    char buf[72];
    int stage = care_stage();
    if (stage >= 2) {
        snprintf(buf, sizeof(buf), "第3段階 ・ 好感度 %u", s_care.bond);
    } else {
        int next = stage == 0 ? CARE_STAGE2_BOND : CARE_STAGE3_BOND;
        snprintf(buf, sizeof(buf), "第%d段階 ・ 好感度 %u（あと%dで進化）",
                 stage + 1, s_care.bond, next - s_care.bond);
    }
    lv_label_set_text(s_care_head, buf);
    lv_obj_set_width(s_care_belly_fill, s_care.belly ? 14 + (170 - 14) * s_care.belly / CARE_FULL : 0);
    lv_obj_set_width(s_care_mood_fill, s_care.mood ? 14 + (170 - 14) * s_care.mood / CARE_FULL : 0);
    snprintf(buf, sizeof(buf), "%d", s_care.belly);
    lv_label_set_text(s_care_belly_val, buf);
    snprintf(buf, sizeof(buf), "%d", s_care.mood);
    lv_label_set_text(s_care_mood_val, buf);
}

static void care_close_panel(void)
{
    s_care_open = false;
    lv_obj_add_flag(s_care_panel, LV_OBJ_FLAG_HIDDEN);
}

static void on_care_close(lv_event_t *e)
{
    (void)e;
    care_close_panel();
}

static void on_care_feed(lv_event_t *e)
{
    (void)e;
    care_settle();
    s_care.belly = s_care.belly > CARE_FULL - 30 ? CARE_FULL : s_care.belly + 30;
    s_care.mood = s_care.mood > CARE_FULL - 5 ? CARE_FULL : s_care.mood + 5;
    if (s_care.bond < 60000) s_care.bond += 5;
    care_save();
    care_refresh();
    care_close_panel();
    muse_state_make_happy();   /* the eating face: the happy pose, held a moment */
    s_care_food_from = (float)esp_timer_get_time() / 1e6f;
    s_care_food_until = s_care_food_from + 1.2f;
    lv_obj_remove_flag(s_care_food, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_care_food);
}

static void on_care_pet(lv_event_t *e)
{
    (void)e;
    care_settle();
    s_care.mood = s_care.mood > CARE_FULL - 15 ? CARE_FULL : s_care.mood + 15;
    if (s_care.bond < 60000) s_care.bond += 2;   /* petting earns less bond than feeding */
    care_save();
    care_refresh();
    care_close_panel();
    muse_state_make_happy();
}

static void care_open_panel(void)
{
    if (!s_care_panel) {
        return;
    }
    care_load();
    care_settle();
    care_save();   /* the settled meters are the new baseline */
    care_refresh();
    s_care_open = true;
    lv_obj_remove_flag(s_care_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_care_panel);
}

/* One meter row: name, a hand-rolled bar (track + fill), the number. */
static void care_meter(lv_obj_t *panel, const lv_font_t *font, const char *name, int y,
                       lv_obj_t **fill_out, lv_obj_t **val_out)
{
    lv_obj_t *l = make_label(panel, font, COLOR_LIT);
    lv_label_set_text(l, name);
    lv_obj_set_pos(l, 28, y);
    lv_obj_t *track = lv_obj_create(panel);
    lv_obj_remove_style_all(track);
    lv_obj_remove_flag(track, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(track, 170, 14);
    lv_obj_set_pos(track, 108, y + 4);
    lv_obj_set_style_bg_color(track, lv_color_hex(COLOR_METER_OFF), 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(track, 7, 0);
    lv_obj_t *fill = lv_obj_create(track);
    lv_obj_remove_style_all(fill);
    lv_obj_remove_flag(fill, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(fill, 0, 14);
    lv_obj_set_style_bg_color(fill, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(fill, 7, 0);
    lv_obj_t *v = make_label(panel, font, COLOR_DIM);
    lv_obj_set_pos(v, 288, y);
    *fill_out = fill;
    *val_out = v;
}

/* A plain disc on the face (an accessory part, a food pellet). */
static lv_obj_t *care_disc(lv_obj_t *parent, uint32_t color)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    return o;
}

static void build_care(lv_obj_t *face)
{
    const lv_font_t *font = caption_font();
    s_care_panel = lv_obj_create(face);
    lv_obj_remove_style_all(s_care_panel);
    lv_obj_remove_flag(s_care_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_care_panel, 344, 356);
    lv_obj_center(s_care_panel);
    lv_obj_set_style_bg_color(s_care_panel, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_bg_opa(s_care_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_care_panel, 24, 0);
    lv_obj_add_flag(s_care_panel, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *title = make_label(s_care_panel, font, COLOR_LIT);
    lv_label_set_text(title, "みゃおっちのお世話");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);
    s_care_head = make_label(s_care_panel, font, COLOR_DIM);
    lv_obj_align(s_care_head, LV_ALIGN_TOP_MID, 0, 52);
    care_meter(s_care_panel, font, "おなか", 100, &s_care_belly_fill, &s_care_belly_val);
    care_meter(s_care_panel, font, "ごきげん", 146, &s_care_mood_fill, &s_care_mood_val);
    lv_obj_t *feed = hist_button(s_care_panel, font, "餌やり", on_care_feed);
    lv_obj_set_size(feed, 128, 54);
    lv_obj_align(feed, LV_ALIGN_TOP_MID, -72, 204);
    lv_obj_t *pet = hist_button(s_care_panel, font, "なでる", on_care_pet);
    lv_obj_set_size(pet, 128, 54);
    lv_obj_align(pet, LV_ALIGN_TOP_MID, 72, 204);
    lv_obj_t *close = hist_button(s_care_panel, font, "閉じる", on_care_close);
    lv_obj_set_size(close, 128, 48);
    lv_obj_align(close, LV_ALIGN_TOP_MID, 0, 278);

    /* The food: three pellets that fall over the avatar while it eats. */
    s_care_food = lv_obj_create(face);
    lv_obj_remove_style_all(s_care_food);
    lv_obj_remove_flag(s_care_food, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_care_food, 54, 18);
    for (int i = 0; i < 3; i++) {
        lv_obj_t *dot = care_disc(s_care_food, 0xd98a4a);
        lv_obj_set_size(dot, 12, 12);
        lv_obj_set_pos(dot, i * 20 + (i == 1 ? 2 : 0), i == 1 ? 4 : 0);
    }
    lv_obj_add_flag(s_care_food, LV_OBJ_FLAG_HIDDEN);

    /* Evolution accessories, drawn over the sprite — the sheet itself is
     * never touched: a gold bell at the neck from stage 2, a pink ribbon
     * at the right ear at stage 3. update_care() places them. */
    s_acc_bell = care_disc(face, 0xf5c542);
    lv_obj_t *dot = care_disc(s_acc_bell, 0x8a6d1f);
    lv_obj_set_size(dot, 5, 5);
    lv_obj_align(dot, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_add_flag(s_acc_bell, LV_OBJ_FLAG_HIDDEN);
    s_acc_ribbon_l = lv_obj_create(face);
    lv_obj_remove_style_all(s_acc_ribbon_l);
    lv_obj_remove_flag(s_acc_ribbon_l, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(s_acc_ribbon_l, lv_color_hex(0xff6b9d), 0);
    lv_obj_set_style_bg_opa(s_acc_ribbon_l, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_acc_ribbon_l, 4, 0);
    lv_obj_set_style_transform_rotation(s_acc_ribbon_l, 450, 0);
    lv_obj_set_style_transform_pivot_x(s_acc_ribbon_l, lv_pct(50), 0);
    lv_obj_set_style_transform_pivot_y(s_acc_ribbon_l, lv_pct(50), 0);
    lv_obj_add_flag(s_acc_ribbon_l, LV_OBJ_FLAG_HIDDEN);
    s_acc_ribbon_r = lv_obj_create(face);
    lv_obj_remove_style_all(s_acc_ribbon_r);
    lv_obj_remove_flag(s_acc_ribbon_r, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(s_acc_ribbon_r, lv_color_hex(0xff6b9d), 0);
    lv_obj_set_style_bg_opa(s_acc_ribbon_r, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_acc_ribbon_r, 4, 0);
    lv_obj_set_style_transform_rotation(s_acc_ribbon_r, -450, 0);
    lv_obj_set_style_transform_pivot_x(s_acc_ribbon_r, lv_pct(50), 0);
    lv_obj_set_style_transform_pivot_y(s_acc_ribbon_r, lv_pct(50), 0);
    lv_obj_add_flag(s_acc_ribbon_r, LV_OBJ_FLAG_HIDDEN);
    s_acc_ribbon_c = care_disc(face, 0xff8fab);
    lv_obj_add_flag(s_acc_ribbon_c, LV_OBJ_FLAG_HIDDEN);
}

static void build_clock(lv_obj_t *face)
{
    s_clock_box = lv_obj_create(face);
    lv_obj_remove_style_all(s_clock_box);
    lv_obj_remove_flag(s_clock_box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_clock_box, 380, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_clock_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_clock_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_clock_box, 12, 0);
    lv_obj_align(s_clock_box, LV_ALIGN_CENTER, 0, -6);
    s_clock_time = make_label(s_clock_box, FONT_CLOCK, 0xffffff);
    lv_label_set_text(s_clock_time, "--:--");
    /* dev.15: Montserrat tops out at 48, so the time is drawn at 200%
     * through a transform (the care ribbon already relies on transforms
     * rendering on this build). The pivot is the label's centre, and
     * symmetric vertical padding reserves the scaled height in the flex
     * column, so the date and battery rows below keep clear of the
     * enlarged digits instead of being overdrawn by them. */
    lv_obj_set_style_transform_scale_x(s_clock_time, 512, 0);
    lv_obj_set_style_transform_scale_y(s_clock_time, 512, 0);
    lv_obj_set_style_transform_pivot_x(s_clock_time, lv_pct(50), 0);
    lv_obj_set_style_transform_pivot_y(s_clock_time, lv_pct(50), 0);
    int clock_pad = lv_font_get_line_height(FONT_CLOCK) / 2;
    lv_obj_set_style_pad_top(s_clock_time, clock_pad, 0);
    lv_obj_set_style_pad_bottom(s_clock_time, clock_pad, 0);
    s_clock_date = make_label(s_clock_box, caption_font(), COLOR_LIT);
    /* dev.16: under the date, the edition's weather and next event in
     * small type, and the running timer's remaining time. No battery
     * row: the face's header chrome already shows it (the user pointed
     * out the duplication), so dev.14's row here is gone. */
    s_clock_wx = make_label(s_clock_box, caption_font(), COLOR_LIT);
    lv_obj_set_size(s_clock_wx, 340, LV_SIZE_CONTENT);
    lv_obj_set_style_text_align(s_clock_wx, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_clock_wx, LV_LABEL_LONG_MODE_DOTS);
    s_clock_next = make_label(s_clock_box, caption_font(), COLOR_DIM);
    lv_obj_set_size(s_clock_next, 340, LV_SIZE_CONTENT);
    lv_obj_set_style_text_align(s_clock_next, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_clock_next, LV_LABEL_LONG_MODE_DOTS);
    s_clock_timer = make_label(s_clock_box, caption_font(), COLOR_ACCENT);
    lv_obj_set_size(s_clock_timer, 340, LV_SIZE_CONTENT);
    lv_obj_set_style_text_align(s_clock_timer, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_clock_timer, LV_LABEL_LONG_MODE_DOTS);
    /* dev.22: under those, the next train — from the timetable the
     * edition carried (trains_line(), by render_dash()). */
    s_clock_train = make_label(s_clock_box, caption_font(), COLOR_DIM);
    lv_obj_set_size(s_clock_train, 340, LV_SIZE_CONTENT);
    lv_obj_set_style_text_align(s_clock_train, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_clock_train, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_add_flag(s_clock_box, LV_OBJ_FLAG_HIDDEN);
}

/* Apply yellow-button presses and keep the clock face honest. The clock
 * yields to everything the avatar face yields to: a turn in progress, a
 * reply on screen, the care panel. */
static void update_face_mode(void)
{
    if (!s_clock_box) {
        return;
    }
    care_load();
    while (s_face_toggles_done != s_face_toggles) {
        s_face_toggles_done++;
        s_clock_mode = !s_clock_mode;
        care_save();   /* the mode survives a reboot */
    }
    bool caption_up = !lv_obj_has_flag(s_caption_lbl, LV_OBJ_FLAG_HIDDEN)
                      || (s_reply_lbl && !lv_obj_has_flag(s_reply_lbl, LV_OBJ_FLAG_HIDDEN))
                      || s_full_lbl || s_ask_shown;
    bool show = s_clock_mode && !s_care_open && !caption_up
                && muse_state_mode(NULL) == MUSE_MODE_IDLE;
    if (show != s_clock_shown) {
        s_clock_shown = show;
        lv_obj_set_flag(s_clock_box, LV_OBJ_FLAG_HIDDEN, !show);
        lv_obj_set_flag(s_canvas, LV_OBJ_FLAG_HIDDEN, show);
        if (show) {
            lv_obj_move_foreground(s_clock_box);
        }
    }
    if (!show) {
        return;
    }
    char buf[48];
    struct tm local;
    bool have_time = msw_time_now && msw_time_now(&local);
    snprintf(buf, sizeof(buf), have_time ? "%02d:%02d" : "--:--",
             have_time ? local.tm_hour : 0, have_time ? local.tm_min : 0);
    if (strcmp(buf, lv_label_get_text(s_clock_time)) != 0) {
        lv_label_set_text(s_clock_time, buf);
    }
    if (have_time) {
        static const char *const WDAY[7] = { "日", "月", "火", "水", "木", "金", "土" };
        snprintf(buf, sizeof(buf), "%d月%d日（%s）", local.tm_mon + 1, local.tm_mday,
                 WDAY[local.tm_wday % 7]);
    } else {
        buf[0] = '\0';
    }
    if (strcmp(buf, lv_label_get_text(s_clock_date)) != 0) {
        lv_label_set_text(s_clock_date, buf);
    }
    /* dev.16: the edition's weather and next event under the date —
     * render_dash() keeps the two strings; they show only while an
     * edition is cached and the last good fetch is under a day old, so
     * a stale edition never dresses itself up as today's. The timer
     * row is update_timer()'s. (dev.14's battery row left with dev.16:
     * the header chrome shows the battery already.) */
    bool fresh = s_dash_info_us && esp_timer_get_time() - s_dash_info_us < 24LL * 3600 * 1000 * 1000;
    const char *wx = fresh ? s_dash_wx : "";
    const char *nx = fresh ? s_dash_next : "";
    if (strcmp(wx, lv_label_get_text(s_clock_wx)) != 0) {
        lv_label_set_text(s_clock_wx, wx);
    }
    lv_obj_set_flag(s_clock_wx, LV_OBJ_FLAG_HIDDEN, !wx[0]);
    if (strcmp(nx, lv_label_get_text(s_clock_next)) != 0) {
        lv_label_set_text(s_clock_next, nx);
    }
    lv_obj_set_flag(s_clock_next, LV_OBJ_FLAG_HIDDEN, !nx[0]);
    /* dev.22: the next train, counted down from the RTC against the
     * cached timetable — no freshness gate like the edition lines
     * above: a timetable stays true until a newer edition replaces
     * it, which is the point of caching it in NVS. */
    char tbuf[64];
    trains_line(tbuf, sizeof(tbuf), &local, have_time);
    if (strcmp(tbuf, lv_label_get_text(s_clock_train)) != 0) {
        lv_label_set_text(s_clock_train, tbuf);
    }
    lv_obj_set_flag(s_clock_train, LV_OBJ_FLAG_HIDDEN, !tbuf[0]);
}

/* Place the evolution accessories over the sprite: the bell at grid
 * (32, 51) — the neck, just above the sprite's blank bottom rows — and
 * the ribbon at grid (47, 8), the right ear. Positions are relative to
 * the face tile, so a tile slide carries them with the avatar for free. */
static void care_accessories(void)
{
    static int last_rx = -1, last_ry, last_w, last_stage;
    static bool last_show;
    int stage = care_stage();
    bool show = s_face_active && !s_clock_shown && !s_care_open;
    lv_area_t c, f;
    lv_obj_get_coords(s_canvas, &c);
    lv_obj_get_coords(s_face, &f);
    int rx = c.x1 - f.x1, ry = c.y1 - f.y1, w = c.x2 - c.x1 + 1;
    if (rx == last_rx && ry == last_ry && w == last_w && stage == last_stage && show == last_show) {
        return;
    }
    last_rx = rx;
    last_ry = ry;
    last_w = w;
    last_stage = stage;
    last_show = show;
    int bs = w * 9 / 64;
    lv_obj_set_size(s_acc_bell, bs, bs);
    lv_obj_set_pos(s_acc_bell, rx + w / 2 - bs / 2, ry + w * 51 / 64 - bs / 2);
    lv_obj_set_flag(s_acc_bell, LV_OBJ_FLAG_HIDDEN, !show || stage < 1);
    int rs = w * 11 / 64;
    int kx = rx + w * 47 / 64, ky = ry + w * 8 / 64;
    lv_obj_set_size(s_acc_ribbon_l, rs, rs);
    lv_obj_set_pos(s_acc_ribbon_l, kx - rs, ky - rs / 2);
    lv_obj_set_size(s_acc_ribbon_r, rs, rs);
    lv_obj_set_pos(s_acc_ribbon_r, kx + rs / 2, ky - rs / 2);
    int cs = w * 6 / 64;
    lv_obj_set_size(s_acc_ribbon_c, cs, cs);
    lv_obj_set_pos(s_acc_ribbon_c, kx - cs / 2, ky - cs / 2);
    bool ribbon = show && stage >= 2;
    lv_obj_set_flag(s_acc_ribbon_l, LV_OBJ_FLAG_HIDDEN, !ribbon);
    lv_obj_set_flag(s_acc_ribbon_r, LV_OBJ_FLAG_HIDDEN, !ribbon);
    lv_obj_set_flag(s_acc_ribbon_c, LV_OBJ_FLAG_HIDDEN, !ribbon);
    if (show) {
        if (stage >= 1) lv_obj_move_foreground(s_acc_bell);
        if (ribbon) {
            lv_obj_move_foreground(s_acc_ribbon_l);
            lv_obj_move_foreground(s_acc_ribbon_r);
            lv_obj_move_foreground(s_acc_ribbon_c);
        }
    }
}

static void update_care(float now)
{
    if (!s_care_panel) {
        return;
    }
    care_load();
    /* Settle about once a minute: enough for the pose effects and the
     * panel, and the settle is exact (whole points only). */
    static float settled_at = -1000.0f;
    if (now - settled_at > 60.0f) {
        settled_at = now;
        care_settle();
    }
    if (s_care_open) {
        care_refresh();
    }
    /* The food falls from above the avatar towards its mouth, then goes. */
    if (s_care_food_until > 0) {
        if (now >= s_care_food_until) {
            s_care_food_until = 0;
            lv_obj_add_flag(s_care_food, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_area_t c, f;
            lv_obj_get_coords(s_canvas, &c);
            lv_obj_get_coords(s_face, &f);
            float k = (now - s_care_food_from) / 0.9f;
            if (k > 1) k = 1;
            int w = c.x2 - c.x1 + 1;
            lv_obj_set_pos(s_care_food, c.x1 - f.x1 + w / 2 - 27,
                           c.y1 - f.y1 + w / 5 + (int)(w / 3 * k));
        }
    }
    care_accessories();
    /* A stage reached while the face is at rest grows the avatar to its
     * new size at once (the reply paths use face_px() too, so a stage
     * reached mid-reply lands at the next restore instead). */
    int stage = care_stage();
    if (s_acc_stage >= 0 && stage != s_acc_stage && s_answer < 0 && !s_full_lbl
        && !s_clock_shown && !s_care_open) {
        move_muse(face_px(), s_big_y);
    }
    s_acc_stage = stage;
}

/*
 * dev.16: the countdown timer.
 *
 * A panel over the face (built beside the care panel): dial a length
 * with the -1/+1/+5/+10 minute buttons, start it, pause or cancel it.
 * It is reached three ways — the quick tile's タイマー button, the
 * launcher's built-in cell, and the yellow button's long press (its
 * default action) — all through muse_ui_request_timer(). The finish
 * is timed by an esp_timer, not by the frame tick, so it lands while
 * the watch sleeps: the callback only flags it and wakes the screen
 * (muse_state_set_asleep(false) ends the Wi-Fi nap too, the way a
 * delivery does), and update_timer() — the first frame back — puts
 * the alert up big on the face and sounds it by the sound mode: chirps
 * (the voice task's, never speech) and motor pulses when sound is on,
 * pulses only in vibrate mode, the screen alone when off. The alert
 * repeats for up to 30 s; a tap on it, or the deadline, ends it. While
 * the timer counts, the clock face carries its remaining time as a
 * small row (update_timer, below).
 */
static void timer_fmt(char *buf, size_t cap, int secs)
{
    snprintf(buf, cap, "%d:%02d", secs / 60, secs % 60);
}

static int timer_left_s(void)
{
    if (s_timer_end_us) {
        int64_t left = (s_timer_end_us - esp_timer_get_time()) / 1000000;
        return left > 0 ? (int)left : 0;
    }
    return s_timer_paused ? s_timer_left_s : -1;
}

static void timer_esp_cb(void *arg)
{
    (void)arg;
    s_timer_fired = true;
    muse_state_set_asleep(false);   /* wake: the alert is the point */
}

static void timer_arm(void)
{
    if (!s_timer_esp) {
        const esp_timer_create_args_t args = { .callback = timer_esp_cb, .name = "timer" };
        if (esp_timer_create(&args, &s_timer_esp) != ESP_OK) {
            s_timer_esp = NULL;
            return;
        }
    }
    esp_timer_stop(s_timer_esp);
    int64_t left = s_timer_end_us - esp_timer_get_time();
    if (left > 0) {
        esp_timer_start_once(s_timer_esp, (uint64_t)left);
    }
}

static void timer_refresh(bool force)
{
    static int shown_s = -1, shown_state = -1;
    int state = s_timer_end_us ? 2 : s_timer_paused ? 1 : 0;   /* set / paused / running */
    int secs = state == 2 ? timer_left_s() : state == 1 ? s_timer_left_s : s_timer_set_s;
    if (!force && secs == shown_s && state == shown_state) {
        return;
    }
    shown_s = secs;
    shown_state = state;
    char buf[16];
    timer_fmt(buf, sizeof(buf), secs);
    lv_label_set_text(s_timer_time, buf);
    lv_label_set_text(s_timer_state, state == 2 ? "計測中" : state == 1 ? "一時停止中" : "セット中");
    lv_label_set_text(s_timer_main_lbl, state == 2 ? "一時停止" : state == 1 ? "再開" : "スタート");
    lv_obj_set_flag(s_timer_cancel_btn, LV_OBJ_FLAG_HIDDEN, state == 0);
}

static void timer_close_panel(void)
{
    s_timer_open = false;
    lv_obj_add_flag(s_timer_panel, LV_OBJ_FLAG_HIDDEN);
}

static void on_timer_close(lv_event_t *e)
{
    (void)e;
    timer_close_panel();
}

static void on_timer_add(lv_event_t *e)
{
    if (s_timer_end_us || s_timer_paused) {
        return;   /* the dial is for a fresh timer */
    }
    int add = (int)(intptr_t)lv_event_get_user_data(e);
    s_timer_set_s += add;
    if (s_timer_set_s < 60) s_timer_set_s = 60;
    if (s_timer_set_s > 5999) s_timer_set_s = 5999;
    timer_refresh(true);
}

static void on_timer_main(lv_event_t *e)
{
    (void)e;
    if (s_timer_end_us) {   /* pause: freeze what is left */
        s_timer_left_s = timer_left_s();
        s_timer_end_us = 0;
        s_timer_paused = true;
        if (s_timer_esp) esp_timer_stop(s_timer_esp);
    } else if (s_timer_paused) {   /* resume */
        s_timer_end_us = esp_timer_get_time() + (int64_t)s_timer_left_s * 1000000;
        s_timer_paused = false;
        timer_arm();
    } else {   /* start */
        if (s_pomo.phase != POMO_IDLE) {
            pomo_end_session();   /* dev.22: the two never run at once */
        }
        s_timer_end_us = esp_timer_get_time() + (int64_t)s_timer_set_s * 1000000;
        timer_arm();
    }
    timer_refresh(true);
}

static void on_timer_cancel(lv_event_t *e)
{
    (void)e;
    s_timer_end_us = 0;
    s_timer_paused = false;
    if (s_timer_esp) esp_timer_stop(s_timer_esp);
    timer_refresh(true);
}

static void on_timer_alert_tap(lv_event_t *e)
{
    (void)e;
    s_timer_alert_until = 0;
    lv_obj_add_flag(s_timer_alert, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *timer_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *ud, lv_obj_t **lbl_out)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x241d40), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, 16, 0);
    lv_obj_t *l = make_label(b, caption_font(), COLOR_LIT);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    if (lbl_out) {
        *lbl_out = l;
    }
    return b;
}

static void build_timer(lv_obj_t *face)
{
    const lv_font_t *font = caption_font();
    s_timer_panel = lv_obj_create(face);
    lv_obj_remove_style_all(s_timer_panel);
    lv_obj_remove_flag(s_timer_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_timer_panel, 344, 372);
    lv_obj_center(s_timer_panel);
    lv_obj_set_style_bg_color(s_timer_panel, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_bg_opa(s_timer_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_timer_panel, 24, 0);
    lv_obj_add_flag(s_timer_panel, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *title = make_label(s_timer_panel, font, COLOR_LIT);
    lv_label_set_text(title, "タイマー");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 18);
    s_timer_time = make_label(s_timer_panel, FONT_CLOCK, 0xffffff);
    lv_label_set_text(s_timer_time, "3:00");
    lv_obj_align(s_timer_time, LV_ALIGN_TOP_MID, 0, 52);
    s_timer_state = make_label(s_timer_panel, font, COLOR_DIM);
    lv_obj_align(s_timer_state, LV_ALIGN_TOP_MID, 0, 128);
    static const struct { const char *t; int add; } ADDS[4] = {
        { "-1分", -60 }, { "+1分", 60 }, { "+5分", 300 }, { "+10分", 600 },
    };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = timer_button(s_timer_panel, ADDS[i].t, on_timer_add,
                                   (void *)(intptr_t)ADDS[i].add, NULL);
        lv_obj_set_size(b, 76, 48);
        lv_obj_align(b, LV_ALIGN_TOP_MID, (i * 2 - 3) * 42, 164);
    }
    lv_obj_t *main_btn = timer_button(s_timer_panel, "スタート", on_timer_main, NULL, &s_timer_main_lbl);
    lv_obj_set_size(main_btn, 150, 54);
    lv_obj_align(main_btn, LV_ALIGN_TOP_MID, -80, 230);
    s_timer_cancel_btn = timer_button(s_timer_panel, "キャンセル", on_timer_cancel, NULL, NULL);
    lv_obj_set_size(s_timer_cancel_btn, 150, 54);
    lv_obj_align(s_timer_cancel_btn, LV_ALIGN_TOP_MID, 80, 230);
    lv_obj_t *close = timer_button(s_timer_panel, "閉じる", on_timer_close, NULL, NULL);
    lv_obj_set_size(close, 128, 48);
    lv_obj_align(close, LV_ALIGN_TOP_MID, 0, 302);
    timer_refresh(true);

    /* The finished alert: the dialled length big, a tap ends it. The
     * title is the 16 px CJK font at 200% through a transform, the
     * clock face's trick (dev.15), with the scaled height reserved. */
    s_timer_alert = lv_obj_create(face);
    lv_obj_remove_style_all(s_timer_alert);
    lv_obj_remove_flag(s_timer_alert, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_timer_alert, 360, 236);
    lv_obj_center(s_timer_alert);
    lv_obj_set_style_bg_color(s_timer_alert, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_bg_opa(s_timer_alert, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_timer_alert, 24, 0);
    lv_obj_set_style_border_width(s_timer_alert, 3, 0);
    lv_obj_set_style_border_color(s_timer_alert, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_add_flag(s_timer_alert, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_timer_alert, on_timer_alert_tap, LV_EVENT_CLICKED, NULL);
    lv_obj_t *done = make_label(s_timer_alert, font, COLOR_LIT);
    lv_label_set_text(done, "タイマー終了");
    lv_obj_set_style_transform_scale_x(done, 512, 0);
    lv_obj_set_style_transform_scale_y(done, 512, 0);
    lv_obj_set_style_transform_pivot_x(done, lv_pct(50), 0);
    lv_obj_set_style_transform_pivot_y(done, lv_pct(50), 0);
    int done_pad = lv_font_get_line_height(font) / 2;
    lv_obj_set_style_pad_top(done, done_pad, 0);
    lv_obj_set_style_pad_bottom(done, done_pad, 0);
    lv_obj_align(done, LV_ALIGN_TOP_MID, 0, 26);
    s_timer_alert_time = make_label(s_timer_alert, FONT_CLOCK, 0xffffff);
    lv_obj_align(s_timer_alert_time, LV_ALIGN_TOP_MID, 0, 96);
    lv_obj_t *hint = make_label(s_timer_alert, font, COLOR_DIM);
    lv_label_set_text(hint, "タップで止める");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -18);
    lv_obj_add_flag(s_timer_alert, LV_OBJ_FLAG_HIDDEN);
}

static void update_timer(float now)
{
    if (!s_timer_panel) {
        return;
    }
    /* Open requests: the face comes forward and the panel opens (the
     * care panel gives way; a running timer keeps running). */
    while (s_timer_open_done != s_timer_open_reqs) {
        s_timer_open_done++;
        if (s_tv && s_face) {
            lv_tileview_set_tile(s_tv, s_face, LV_ANIM_OFF);
        }
        care_close_panel();
        pomo_close_panel();
        s_timer_open = true;
        lv_obj_remove_flag(s_timer_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_timer_panel);
        timer_refresh(true);
    }
    /* The esp_timer fired while the frame tick slept: the alert. */
    if (s_timer_fired) {
        s_timer_fired = false;
        s_timer_end_us = 0;
        s_timer_paused = false;
        if (s_tv && s_face) {
            lv_tileview_set_tile(s_tv, s_face, LV_ANIM_OFF);
        }
        care_close_panel();
        pomo_close_panel();
        timer_close_panel();
        char buf[16];
        timer_fmt(buf, sizeof(buf), s_timer_set_s);
        lv_label_set_text(s_timer_alert_time, buf);
        lv_obj_remove_flag(s_timer_alert, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_timer_alert);
        s_timer_alert_until = now + 30.0f;
        s_timer_alert_last = 0;
        timer_refresh(true);
    }
    if (s_timer_alert_until > 0) {
        if (now >= s_timer_alert_until) {
            s_timer_alert_until = 0;
            lv_obj_add_flag(s_timer_alert, LV_OBJ_FLAG_HIDDEN);
        } else if (now - s_timer_alert_last >= 1.2f) {
            s_timer_alert_last = now;
            int mode = muse_settings_sound_mode();
            if (mode != MUSE_SOUND_OFF) {
                muse_review_pulse();   /* the motor, in the modes that have it */
            }
            if (mode == MUSE_SOUND_ON) {
                muse_voice_request_chirp();   /* a chirp, never speech */
            }
        }
    }
    if (s_timer_open) {
        timer_refresh(false);
    }
    /* The clock face's timer row: what is left, while it counts. */
    if (s_clock_timer) {
        int left = timer_left_s();
        char buf[40] = "";
        if (left >= 0) {
            char t[16];
            timer_fmt(t, sizeof(t), left);
            snprintf(buf, sizeof(buf), "タイマー %s", t);
        }
        if (strcmp(buf, lv_label_get_text(s_clock_timer)) != 0) {
            lv_label_set_text(s_clock_timer, buf);
        }
        lv_obj_set_flag(s_clock_timer, LV_OBJ_FLAG_HIDDEN, !buf[0]);
    }
}

/*
 * dev.22: the pomodoro — the timer's sibling, a chain of phases
 * instead of one countdown: focus (25 min) → break (5 min), and a
 * long break (15 min) after 4 focuses, the lengths and the count
 * tunable in Settings > Pomodoro. It borrows the timer's whole shape:
 * a panel over the face reached three ways (the quick tile's
 * ポモドーロ button, the launcher's built-in cell, the yellow
 * button's long press) through muse_ui_request_pomodoro(); an
 * esp_timer finish that lands while the watch sleeps, its callback
 * only flagging and waking; an alert update_pomodoro() raises on the
 * first frame back, sounded by the sound mode — pulses and chirps,
 * never speech — for up to 30 s. Two things are its own: the alert
 * tells a focus ending from a break ending (a focus ends with two
 * chirps a beat and a 1.2 s beat, a break with one chirp and a 2.4 s
 * beat, and the words 集中おわり / 休憩おわり), and stopping the
 * alert — a tap, or the 30 s running out — starts the next phase at
 * once, so the rhythm survives a missed alert. It and the plain
 * timer are exclusive: starting one cancels the other (one alert
 * path, one clock row). The state persists in the care NVS handle:
 * settings, the phase, its wall-clock end, the round's finished
 * focuses. On reboot a phase whose end is still ahead resumes with
 * what is left; one that ended while the watch was off does NOT
 * sound — the session waits, paused at the next phase's start.
 * While the panel is up, みゃおっち rides in it small (the face's
 * own image, shared) and follows the phase — calm through a focus,
 * happy on a break — by the pose override in the frame tick; the
 * care meters are neither read nor moved for it.
 */
static int pomo_len_s(int phase)
{
    return (phase == POMO_FOCUS ? s_pomo.focus_min
            : phase == POMO_BREAK ? s_pomo.break_min
            : phase == POMO_LONG  ? s_pomo.long_min : 0) * 60;
}

static const char *pomo_name(int phase)
{
    return phase == POMO_FOCUS ? "集中" : phase == POMO_BREAK ? "休憩"
           : phase == POMO_LONG ? "長い休憩" : "";
}

static int pomo_left_s(void)
{
    if (s_pomo_end_us) {
        int64_t left = (s_pomo_end_us - esp_timer_get_time()) / 1000000;
        return left > 0 ? (int)left : 0;
    }
    return s_pomo.paused ? s_pomo.left_s : -1;
}

static void pomo_save(void)
{
    if (!s_care_nvs) {
        return;
    }
    nvs_set_u8(s_care_nvs, "pomo_foc", (uint8_t)s_pomo.focus_min);
    nvs_set_u8(s_care_nvs, "pomo_brk", (uint8_t)s_pomo.break_min);
    nvs_set_u8(s_care_nvs, "pomo_lng", (uint8_t)s_pomo.long_min);
    nvs_set_u8(s_care_nvs, "pomo_nst", (uint8_t)s_pomo.sets);
    nvs_set_u8(s_care_nvs, "pomo_pha", s_pomo.phase);
    nvs_set_u8(s_care_nvs, "pomo_cnt", s_pomo.done);
    nvs_set_u8(s_care_nvs, "pomo_pau", s_pomo.paused ? 1 : 0);
    int left = pomo_left_s();
    nvs_set_u32(s_care_nvs, "pomo_lft", (uint32_t)(left > 0 ? left : 0));
    uint32_t end_s = 0;
    uint32_t now;
    if (s_pomo_end_us && care_now(&now)) {
        int64_t ahead = (s_pomo_end_us - esp_timer_get_time()) / 1000000;
        if (ahead > 0) {
            end_s = now + (uint32_t)ahead;   /* the phase's wall-clock end */
        }
    }
    nvs_set_u32(s_care_nvs, "pomo_end", end_s);
    nvs_commit(s_care_nvs);
}

/* The phase after this one; a finished focus counts towards the
 * long break, a finished long break opens a fresh round. */
static void pomo_advance(void)
{
    if (s_pomo.phase == POMO_FOCUS) {
        s_pomo.done++;
        s_pomo.phase = s_pomo.done >= s_pomo.sets ? POMO_LONG : POMO_BREAK;
    } else if (s_pomo.phase == POMO_BREAK) {
        s_pomo.phase = POMO_FOCUS;
    } else {
        s_pomo.done = 0;
        s_pomo.phase = POMO_FOCUS;
    }
}

static void pomo_load(void)
{
    if (s_pomo.loaded) {
        return;
    }
    s_pomo.loaded = true;
    if (!s_care_nvs) {
        return;   /* RAM only this boot */
    }
    uint8_t b;
    uint32_t u;
    if (nvs_get_u8(s_care_nvs, "pomo_foc", &b) == ESP_OK && b >= 1 && b <= 180) s_pomo.focus_min = b;
    if (nvs_get_u8(s_care_nvs, "pomo_brk", &b) == ESP_OK && b >= 1 && b <= 60) s_pomo.break_min = b;
    if (nvs_get_u8(s_care_nvs, "pomo_lng", &b) == ESP_OK && b >= 1 && b <= 90) s_pomo.long_min = b;
    if (nvs_get_u8(s_care_nvs, "pomo_nst", &b) == ESP_OK && b >= 2 && b <= 8) s_pomo.sets = b;
    if (nvs_get_u8(s_care_nvs, "pomo_pha", &b) == ESP_OK && b <= POMO_IDLE) s_pomo.phase = b;
    if (nvs_get_u8(s_care_nvs, "pomo_cnt", &b) == ESP_OK && b <= 8) s_pomo.done = b;
    if (s_pomo.phase == POMO_IDLE) {
        return;
    }
    uint32_t lft = 0, end_s = 0;
    nvs_get_u32(s_care_nvs, "pomo_lft", &lft);
    nvs_get_u32(s_care_nvs, "pomo_end", &end_s);
    bool paused = nvs_get_u8(s_care_nvs, "pomo_pau", &b) == ESP_OK && b;
    uint32_t now;
    if (paused) {
        s_pomo.paused = true;
        s_pomo.left_s = lft > 0 ? (int)lft : pomo_len_s(s_pomo.phase);
    } else if (end_s && care_now(&now)) {
        int64_t left = (int64_t)end_s - (int64_t)now;
        if (left > 0) {
            /* Still running: re-arm against the monotonic clock in
             * update_pomodoro(), once the panel side exists. */
            s_pomo_end_us = esp_timer_get_time() + left * 1000000;
            s_pomo.arm_pending = true;
        } else {
            /* The phase ended while the watch was off: no alert —
             * the next phase waits at its start, paused. */
            pomo_advance();
            s_pomo.paused = true;
            s_pomo.left_s = pomo_len_s(s_pomo.phase);
        }
    } else {
        /* No wall clock to reckon the end by: hold what was left. */
        s_pomo.paused = true;
        s_pomo.left_s = lft > 0 ? (int)lft : pomo_len_s(s_pomo.phase);
    }
}

static void pomo_esp_cb(void *arg)
{
    (void)arg;
    s_pomo_fired = true;
    muse_state_set_asleep(false);   /* wake: the alert is the point */
}

static void pomo_arm(void)
{
    if (!s_pomo_esp) {
        const esp_timer_create_args_t args = { .callback = pomo_esp_cb, .name = "pomo" };
        if (esp_timer_create(&args, &s_pomo_esp) != ESP_OK) {
            s_pomo_esp = NULL;
            return;
        }
    }
    esp_timer_stop(s_pomo_esp);
    int64_t left = s_pomo_end_us - esp_timer_get_time();
    if (left > 0) {
        esp_timer_start_once(s_pomo_esp, (uint64_t)left);
    }
}

static void pomo_start_phase(void)
{
    s_pomo_end_us = esp_timer_get_time() + (int64_t)pomo_len_s(s_pomo.phase) * 1000000;
    s_pomo.paused = false;
    pomo_arm();
    pomo_save();
}

static void pomo_close_panel(void)
{
    s_pomo_open = false;
    if (s_pomo_panel) {
        lv_obj_add_flag(s_pomo_panel, LV_OBJ_FLAG_HIDDEN);
    }
}

static void pomo_end_session(void)
{
    s_pomo.phase = POMO_IDLE;
    s_pomo.done = 0;
    s_pomo_end_us = 0;
    s_pomo.paused = false;
    if (s_pomo_esp) {
        esp_timer_stop(s_pomo_esp);
    }
    s_pomo_alert_until = 0;
    if (s_pomo_alert) {
        lv_obj_add_flag(s_pomo_alert, LV_OBJ_FLAG_HIDDEN);
    }
    pomo_save();
}

/* The alert is over (a tap, or its 30 s): the next phase starts. */
static void pomo_stop_alert(void)
{
    if (!s_pomo_alert_until) {
        return;
    }
    s_pomo_alert_until = 0;
    if (s_pomo_alert) {
        lv_obj_add_flag(s_pomo_alert, LV_OBJ_FLAG_HIDDEN);
    }
    pomo_start_phase();
}

static void pomo_refresh(bool force)
{
    static int shown_s = -1, shown_state = -1;
    /* set / running / paused-at-start / paused / alert-pending */
    int state = s_pomo.phase == POMO_IDLE ? 0
                : s_pomo_alert_until > 0 ? 4
                : s_pomo_end_us ? 1
                : s_pomo.left_s >= pomo_len_s(s_pomo.phase) ? 2 : 3;
    int secs = state == 0 ? s_pomo.focus_min * 60
               : state == 1 ? pomo_left_s()
               : s_pomo.left_s;
    if (state == 4) {
        secs = pomo_len_s(s_pomo.phase);   /* the next phase, whole */
    }
    if (!force && secs == shown_s && state == shown_state) {
        return;
    }
    shown_s = secs;
    shown_state = state;
    char buf[40];
    timer_fmt(buf, sizeof(buf), secs);
    lv_label_set_text(s_pomo_time, buf);
    if (state == 0) {
        lv_label_set_text(s_pomo_state, "セット中");
    } else if (state == 4) {
        snprintf(buf, sizeof(buf), "%s 開始待ち", pomo_name(s_pomo.phase));
        lv_label_set_text(s_pomo_state, buf);
    } else {
        snprintf(buf, sizeof(buf), "%s %s", pomo_name(s_pomo.phase),
                 state == 1 ? "計測中" : state == 2 ? "開始待ち" : "一時停止中");
        lv_label_set_text(s_pomo_state, buf);
    }
    /* The round's dots: one per focus in the round, filled as they land. */
    char dots[28];
    size_t dn = 0;
    for (int i = 0; i < s_pomo.sets && dn + 4 < sizeof(dots); i++) {
        const char *d = i < s_pomo.done ? "●" : "○";
        memcpy(dots + dn, d, 3);
        dn += 3;
    }
    dots[dn] = '\0';
    lv_label_set_text(s_pomo_dots, dots);
    lv_label_set_text(s_pomo_main_lbl, state == 1 ? "一時停止" : state == 3 ? "再開" : "スタート");
    lv_obj_set_flag(s_pomo_skip_btn, LV_OBJ_FLAG_HIDDEN, state == 0);
    lv_obj_set_flag(s_pomo_end_btn, LV_OBJ_FLAG_HIDDEN, state == 0);
}

static void on_pomo_close(lv_event_t *e)
{
    (void)e;
    pomo_close_panel();
}

static void on_pomo_main(lv_event_t *e)
{
    (void)e;
    if (s_pomo_alert_until > 0) {   /* the panel reopened over the alert */
        pomo_stop_alert();
    } else if (s_pomo_end_us) {   /* pause: freeze what is left */
        s_pomo.left_s = pomo_left_s();
        s_pomo_end_us = 0;
        s_pomo.paused = true;
        if (s_pomo_esp) esp_timer_stop(s_pomo_esp);
        pomo_save();
    } else if (s_pomo.phase == POMO_IDLE) {   /* a fresh session */
        if (s_timer_end_us || s_timer_paused) {   /* the timer gives way */
            s_timer_end_us = 0;
            s_timer_paused = false;
            if (s_timer_esp) esp_timer_stop(s_timer_esp);
            timer_refresh(true);
        }
        s_pomo.phase = POMO_FOCUS;
        s_pomo.done = 0;
        pomo_start_phase();
    } else if (s_pomo.left_s >= pomo_len_s(s_pomo.phase)) {   /* at a phase's start */
        pomo_start_phase();
    } else {   /* resume */
        s_pomo_end_us = esp_timer_get_time() + (int64_t)s_pomo.left_s * 1000000;
        s_pomo.paused = false;
        pomo_arm();
        pomo_save();
    }
    pomo_refresh(true);
}

static void on_pomo_skip(lv_event_t *e)
{
    (void)e;
    if (s_pomo_alert_until > 0) {
        pomo_stop_alert();
    } else if (s_pomo.phase != POMO_IDLE) {   /* on to the next phase, counted */
        pomo_advance();
        pomo_start_phase();
    }
    pomo_refresh(true);
}

static void on_pomo_end(lv_event_t *e)
{
    (void)e;
    pomo_end_session();
    pomo_refresh(true);
}

static void on_pomo_alert_tap(lv_event_t *e)
{
    (void)e;
    pomo_stop_alert();
}

static void build_pomodoro(lv_obj_t *face)
{
    const lv_font_t *font = caption_font();
    s_pomo_panel = lv_obj_create(face);
    lv_obj_remove_style_all(s_pomo_panel);
    lv_obj_remove_flag(s_pomo_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_pomo_panel, 344, 372);
    lv_obj_center(s_pomo_panel);
    lv_obj_set_style_bg_color(s_pomo_panel, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_bg_opa(s_pomo_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_pomo_panel, 24, 0);
    lv_obj_add_flag(s_pomo_panel, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *title = make_label(s_pomo_panel, font, COLOR_LIT);
    lv_label_set_text(title, "ポモドーロ");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 14);
    /* みゃおっち rides along small: the face's own rendered image,
     * shared and scaled down — its expression follows the phase
     * through the frame tick's pose override, nothing of its own. */
    lv_obj_t *mini = lv_image_create(s_pomo_panel);
    lv_image_set_src(mini, &s_muse_src);
    lv_image_set_scale(mini, 72);
    lv_obj_align(mini, LV_ALIGN_TOP_MID, 0, 40);
    s_pomo_time = make_label(s_pomo_panel, FONT_CLOCK, 0xffffff);
    lv_label_set_text(s_pomo_time, "25:00");
    lv_obj_align(s_pomo_time, LV_ALIGN_TOP_MID, 0, 146);
    s_pomo_state = make_label(s_pomo_panel, font, COLOR_DIM);
    lv_obj_align(s_pomo_state, LV_ALIGN_TOP_MID, 0, 222);
    s_pomo_dots = make_label(s_pomo_panel, font, COLOR_ACCENT);
    lv_obj_align(s_pomo_dots, LV_ALIGN_TOP_MID, 0, 248);
    lv_obj_t *main_btn = timer_button(s_pomo_panel, "スタート", on_pomo_main, NULL, &s_pomo_main_lbl);
    lv_obj_set_size(main_btn, 150, 50);
    lv_obj_align(main_btn, LV_ALIGN_TOP_MID, -80, 272);
    s_pomo_skip_btn = timer_button(s_pomo_panel, "スキップ", on_pomo_skip, NULL, NULL);
    lv_obj_set_size(s_pomo_skip_btn, 150, 50);
    lv_obj_align(s_pomo_skip_btn, LV_ALIGN_TOP_MID, 80, 272);
    s_pomo_end_btn = timer_button(s_pomo_panel, "終了", on_pomo_end, NULL, NULL);
    lv_obj_set_size(s_pomo_end_btn, 104, 36);
    lv_obj_align(s_pomo_end_btn, LV_ALIGN_TOP_MID, -56, 330);
    lv_obj_t *close = timer_button(s_pomo_panel, "閉じる", on_pomo_close, NULL, NULL);
    lv_obj_set_size(close, 104, 36);
    lv_obj_align(close, LV_ALIGN_TOP_MID, 56, 330);
    pomo_refresh(true);

    /* The phase-end alert, the timer alert's shape: the ended phase
     * named big, what comes next under it, a tap starts that next
     * phase (pomo_stop_alert). */
    s_pomo_alert = lv_obj_create(face);
    lv_obj_remove_style_all(s_pomo_alert);
    lv_obj_remove_flag(s_pomo_alert, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_pomo_alert, 360, 236);
    lv_obj_center(s_pomo_alert);
    lv_obj_set_style_bg_color(s_pomo_alert, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_bg_opa(s_pomo_alert, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_pomo_alert, 24, 0);
    lv_obj_set_style_border_width(s_pomo_alert, 3, 0);
    lv_obj_set_style_border_color(s_pomo_alert, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_add_flag(s_pomo_alert, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_pomo_alert, on_pomo_alert_tap, LV_EVENT_CLICKED, NULL);
    s_pomo_alert_title = make_label(s_pomo_alert, font, COLOR_LIT);
    lv_obj_set_style_transform_scale_x(s_pomo_alert_title, 512, 0);
    lv_obj_set_style_transform_scale_y(s_pomo_alert_title, 512, 0);
    lv_obj_set_style_transform_pivot_x(s_pomo_alert_title, lv_pct(50), 0);
    lv_obj_set_style_transform_pivot_y(s_pomo_alert_title, lv_pct(50), 0);
    int title_pad = lv_font_get_line_height(font) / 2;
    lv_obj_set_style_pad_top(s_pomo_alert_title, title_pad, 0);
    lv_obj_set_style_pad_bottom(s_pomo_alert_title, title_pad, 0);
    lv_obj_align(s_pomo_alert_title, LV_ALIGN_TOP_MID, 0, 26);
    s_pomo_alert_sub = make_label(s_pomo_alert, font, COLOR_LIT);
    lv_obj_set_size(s_pomo_alert_sub, 340, LV_SIZE_CONTENT);
    lv_obj_set_style_text_align(s_pomo_alert_sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_pomo_alert_sub, LV_ALIGN_TOP_MID, 0, 110);
    lv_obj_t *hint = make_label(s_pomo_alert, font, COLOR_DIM);
    lv_label_set_text(hint, "タップで次へ");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -18);
    lv_obj_add_flag(s_pomo_alert, LV_OBJ_FLAG_HIDDEN);
}

static void update_pomodoro(float now)
{
    if (!s_pomo_panel) {
        return;
    }
    if (s_pomo.arm_pending) {   /* a session restored at load re-arms here */
        s_pomo.arm_pending = false;
        if (s_pomo_end_us) {
            pomo_arm();
        }
    }
    /* Open requests: the face comes forward and the panel opens. */
    while (s_pomo_open_done != s_pomo_open_reqs) {
        s_pomo_open_done++;
        if (s_tv && s_face) {
            lv_tileview_set_tile(s_tv, s_face, LV_ANIM_OFF);
        }
        care_close_panel();
        timer_close_panel();
        s_pomo_open = true;
        lv_obj_remove_flag(s_pomo_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_pomo_panel);
        pomo_refresh(true);
    }
    /* The esp_timer fired while the frame tick slept: the alert. The
     * phase has already advanced on paper; it starts when the alert
     * is stopped (pomo_stop_alert), by tap or by its deadline. */
    if (s_pomo_fired) {
        s_pomo_fired = false;
        int ended = s_pomo.phase;
        pomo_advance();
        s_pomo.end_us = 0;
        s_pomo.paused = false;
        s_pomo_alert_ended = ended;
        if (s_tv && s_face) {
            lv_tileview_set_tile(s_tv, s_face, LV_ANIM_OFF);
        }
        care_close_panel();
        timer_close_panel();
        pomo_close_panel();
        lv_label_set_text(s_pomo_alert_title,
                          ended == POMO_FOCUS ? "集中おわり"
                          : ended == POMO_LONG ? "長い休憩おわり" : "休憩おわり");
        char sub[80];
        if (ended == POMO_FOCUS) {
            snprintf(sub, sizeof(sub), "%dセット目おわり　次は%s %d分",
                     s_pomo.done, pomo_name(s_pomo.phase), pomo_len_s(s_pomo.phase) / 60);
        } else {
            snprintf(sub, sizeof(sub), "次は集中 %d分（%dセット目）",
                     s_pomo.focus_min, s_pomo.done + 1);
        }
        lv_label_set_text(s_pomo_alert_sub, sub);
        lv_obj_remove_flag(s_pomo_alert, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_pomo_alert);
        s_pomo_alert_until = now + 30.0f;
        s_pomo_alert_last = 0;
        pomo_save();
    }
    if (s_pomo_alert_until > 0) {
        if (now >= s_pomo_alert_until) {
            pomo_stop_alert();   /* unstopped alerts still advance the chain */
        } else {
            /* A focus ending insists (two chirps, a 1.2 s beat); a
             * break ending is gentler (one chirp, a 2.4 s beat). */
            float beat = s_pomo_alert_ended == POMO_FOCUS ? 1.2f : 2.4f;
            if (now - s_pomo_alert_last >= beat) {
                s_pomo_alert_last = now;
                int mode = muse_settings_sound_mode();
                if (mode != MUSE_SOUND_OFF) {
                    muse_review_pulse();   /* the motor, in the modes that have it */
                }
                if (mode == MUSE_SOUND_ON) {
                    muse_voice_request_chirp();   /* a chirp, never speech */
                    if (s_pomo_alert_ended == POMO_FOCUS) {
                        muse_voice_request_chirp();
                    }
                }
            }
        }
    }
    if (s_pomo_open) {
        pomo_refresh(false);
    }
    /* The clock face's remaining-time row, shared with the timer:
     * update_timer() runs first and owns the row while it counts, so
     * the pomodoro writes only when the timer is idle. */
    if (s_clock_timer && timer_left_s() < 0) {
        char buf[48] = "";
        if (s_pomo_alert_until > 0) {
            char t[16];
            timer_fmt(t, sizeof(t), pomo_len_s(s_pomo.phase));
            snprintf(buf, sizeof(buf), "%s %s", pomo_name(s_pomo.phase), t);
        } else {
            int left = pomo_left_s();
            if (left >= 0) {
                char t[16];
                timer_fmt(t, sizeof(t), left);
                snprintf(buf, sizeof(buf), "%s %s", pomo_name(s_pomo.phase), t);
            }
        }
        if (strcmp(buf, lv_label_get_text(s_clock_timer)) != 0) {
            lv_label_set_text(s_clock_timer, buf);
        }
        lv_obj_set_flag(s_clock_timer, LV_OBJ_FLAG_HIDDEN, !buf[0]);
    }
}

/* Settings > Pomodoro (msw_pomodoro_ui.inc) reads and writes the
 * lengths through here; a change mid-session lands at the next
 * phase, whose length is read when it starts. */
void muse_ui_pomodoro_config(muse_pomo_cfg_t *out)
{
    care_load();
    out->focus_min = s_pomo.focus_min;
    out->break_min = s_pomo.break_min;
    out->long_min = s_pomo.long_min;
    out->sets = s_pomo.sets;
}

void muse_ui_set_pomodoro_config(const muse_pomo_cfg_t *cfg)
{
    care_load();
    s_pomo.focus_min = cfg->focus_min < 1 ? 1 : cfg->focus_min > 180 ? 180 : cfg->focus_min;
    s_pomo.break_min = cfg->break_min < 1 ? 1 : cfg->break_min > 60 ? 60 : cfg->break_min;
    s_pomo.long_min = cfg->long_min < 1 ? 1 : cfg->long_min > 90 ? 90 : cfg->long_min;
    s_pomo.sets = cfg->sets < 2 ? 2 : cfg->sets > 8 ? 8 : cfg->sets;
    if (s_pomo.done > s_pomo.sets) {
        s_pomo.done = s_pomo.sets;
    }
    pomo_save();
    if (s_pomo_panel) {
        pomo_refresh(true);
    }
}

/*
 * The dashboard tile (right of the face): the morning/evening "edition"
 * Muse writes to watch_edition.json on the VM. The tile fetches it on its
 * own task — the request callback runs on the session task, so it only
 * copies frames into a buffer and hands over; the task below does the
 * waiting, the parsing and the caching. The last good edition is cached in
 * PSRAM and shown as-is when a fetch fails. Display only: nothing on the
 * tile sends anything back. dev.8: the whole fetch runs against one
 * deadline (DASH_FETCH_DEADLINE_US) and logs its stages (TAG "muse_ui",
 * "dash: …"), so a stall resolves and says why. dev.10: the same stages
 * also land in s_dash_err, and the tile shows the last one ("取得失敗: …")
 * — the log is not readable off the device, the tile is.
 * dev.11: POST /fs/read of the bare home path drew a 403 on the real
 * device (dev.10's tile said so), so the fetch now tries GET /fs/raw/<path>
 * first — the route image_fetch.c reads VM files through, whose reply body
 * is the file itself, no envelope — and the edition lives under
 * workspace/ (DASH_PATH), the home-relative shape the VM's file rules
 * expect. /fs/read with the same new path stays as the fallback, and
 * s_dash_err names both routes' stages ("raw …/read …") when both fail.
 */
#define DASH_PATH "workspace/watch_edition.json"
#define DASH_CAP (8 * 1024)        /* the whole edition, at most */
#define DASH_CHUNK 1024            /* one /fs/read: len <= 1024 (AGENTS.md) */
#define DASH_ENV_CAP (12 * 1024)   /* one reply envelope: the frame bound */
#define DASH_STALE_US (10LL * 60 * 1000 * 1000)
#define DASH_CHUNK_WAIT_MS 8000    /* one chunk's reply, at most */
#define DASH_FETCH_DEADLINE_US (30LL * 1000 * 1000)   /* the whole fetch */

static lv_obj_t *s_dash_sub, *s_dash_state, *s_dash_box;
static char *s_dash_json;          /* PSRAM: the cached edition (raw JSON) */
static char *s_dash_new;           /* PSRAM: the edition being fetched */
static char *s_dash_env;           /* PSRAM: one reply envelope, assembled */
static char *s_dash_scratch;       /* PSRAM: a section's lines, joined */
static size_t s_dash_env_len;
static bool s_dash_env_over;
static int s_dash_status;          /* the reply's HTTP status (first frame) */
static uint32_t s_dash_gen;        /* request generation: stale frames drop */
static SemaphoreHandle_t s_dash_go;    /* a fetch was requested */
static SemaphoreHandle_t s_dash_done;  /* the current request has ended */
static TaskHandle_t s_dash_task;
static volatile bool s_dash_busy;
static volatile bool s_dash_dirty;     /* the cache changed: re-render */
static volatile bool s_dash_wake;      /* the screen woke: fetch (update_sleep) */
static int64_t s_dash_ok_us;           /* the last successful fetch */
static char s_dash_err[48];            /* dev.10: the last fetch's failure stage, "" if none */
static char s_dash_raw_err[32];        /* dev.11: the /fs/raw stage, "" if it landed */
static char s_dash_read_err[32];       /* dev.11: the /fs/read stage, "" if it landed */

/* On the session task: copy the frame out and hand over, nothing more. */
static void dash_on_frame(void *ctx, int status, const uint8_t *data, size_t len, bool end)
{
    if ((uint32_t)(uintptr_t)ctx != s_dash_gen) {
        return;   /* a frame of a request already given up on */
    }
    if (status != 0) {
        s_dash_status = status;   /* the first frame carries the HTTP status;
                                   * a failure ends the request with a negative */
    }
    if (data && len) {
        if (s_dash_env_len + len < DASH_ENV_CAP) {
            memcpy(s_dash_env + s_dash_env_len, data, len);
            s_dash_env_len += len;
        } else {
            s_dash_env_over = true;
        }
    }
    /* The stream is done after end OR a negative status (noise_control.h);
     * image_fetch's vm_on_frame treats it the same way. Signalling only on
     * end would leave the fetch task waiting out its timeout on a stream
     * the daemon reset. */
    if (end || status < 0) {
        xSemaphoreGive(s_dash_done);
    }
}

/* dev.11 PRIMARY: one GET /fs/raw/<path> for the whole edition — the reply
 * body is the file itself, accumulated by dash_on_frame like an envelope
 * but never parsed as one. False: s_dash_raw_err says why. */
static bool dash_raw_fetch(size_t *total, int64_t deadline)
{
    if (esp_timer_get_time() >= deadline) {
        ESP_LOGW(TAG, "dash: /fs/raw skipped, fetch already over its deadline");
        snprintf(s_dash_raw_err, sizeof(s_dash_raw_err), "deadline");
        return false;
    }
    /* The same identity headers the /fs/read chunks carry (dev.9). */
    char req_id[40];
    snprintf(req_id, sizeof(req_id), "muse-%08" PRIx32 "-%08" PRIx32, esp_random(), esp_random());
    const char *headers[] = { "x-request-id", req_id, "x-app-id", "hatch-web",
                              "Accept", "application/json", NULL };
    s_dash_gen++;
    s_dash_env_len = 0;
    s_dash_env_over = false;
    s_dash_status = 0;
    xSemaphoreTake(s_dash_done, 0);   /* drain a stale give */
    int64_t id = muse_link_req_open("GET", "/fs/raw/" DASH_PATH, headers, true, dash_on_frame,
                                    (void *)(uintptr_t)s_dash_gen);
    if (!id) {
        ESP_LOGW(TAG, "dash: /fs/raw open refused (session down or busy)");
        snprintf(s_dash_raw_err, sizeof(s_dash_raw_err), "open refused");
        return false;
    }
    /* One chunk's wait, capped by what is left of the fetch deadline. */
    int64_t left_ms = (deadline - esp_timer_get_time()) / 1000;
    int wait_ms = left_ms > DASH_CHUNK_WAIT_MS ? DASH_CHUNK_WAIT_MS : (int)left_ms;
    if (wait_ms <= 0 ||
        xSemaphoreTake(s_dash_done, pdMS_TO_TICKS(wait_ms)) != pdTRUE) {
        ESP_LOGW(TAG, "dash: /fs/raw no reply in %d ms", wait_ms);
        snprintf(s_dash_raw_err, sizeof(s_dash_raw_err), "no reply");
        muse_link_req_cancel(id);
        s_dash_gen++;   /* a late frame of this request drops */
        return false;
    }
    if (s_dash_status != 200 || s_dash_env_over || s_dash_env_len > DASH_CAP) {
        bool over = s_dash_env_over || s_dash_env_len > DASH_CAP;
        ESP_LOGW(TAG, "dash: /fs/raw status %d%s (%u bytes)", s_dash_status,
                 over ? " (reply too large)" : "", (unsigned)s_dash_env_len);
        if (over) {
            snprintf(s_dash_raw_err, sizeof(s_dash_raw_err), "reply too large");
        } else {
            snprintf(s_dash_raw_err, sizeof(s_dash_raw_err), "status %d", s_dash_status);
        }
        return false;
    }
    memcpy(s_dash_new, s_dash_env, s_dash_env_len);
    s_dash_new[s_dash_env_len] = '\0';
    /* Landed means the edition itself: the bytes must parse as JSON. */
    cJSON *probe = cJSON_Parse(s_dash_new);
    if (!probe) {
        ESP_LOGW(TAG, "dash: /fs/raw reply is %u bytes but not JSON",
                 (unsigned)s_dash_env_len);
        snprintf(s_dash_raw_err, sizeof(s_dash_raw_err), "not json");
        return false;
    }
    cJSON_Delete(probe);
    *total = s_dash_env_len;
    ESP_LOGI(TAG, "dash: edition fetched via /fs/raw, %u bytes", (unsigned)*total);
    return true;
}

/* One /fs/read chunk of the edition into s_dash_new at *total. False: stop. */
static bool dash_read_chunk(size_t offset, size_t *total, bool *eof)
{
    char body[128];
    snprintf(body, sizeof(body), "{\"path\":\"" DASH_PATH "\",\"offset\":%u,\"len\":%d}",
             (unsigned)offset, DASH_CHUNK);
    /* The same headers Muse's own chat requests carry (muse_chat_link.c):
     * dev.8 sent Content-Type alone and the daemon never answered, so the
     * tile stayed on 「まだ届いていません」. */
    char req_id[40];
    snprintf(req_id, sizeof(req_id), "muse-%08" PRIx32 "-%08" PRIx32, esp_random(), esp_random());
    const char *headers[] = { "x-request-id", req_id, "x-app-id", "hatch-web",
                              "Content-Type", "application/json",
                              "Accept", "application/json", NULL };
    s_dash_gen++;
    s_dash_env_len = 0;
    s_dash_env_over = false;
    s_dash_status = 0;
    xSemaphoreTake(s_dash_done, 0);   /* drain a stale give */
    int64_t id = muse_link_req_open("POST", "/fs/read", headers, false, dash_on_frame,
                                    (void *)(uintptr_t)s_dash_gen);
    if (!id) {
        ESP_LOGW(TAG, "dash: /fs/read open refused (session down or busy)");
        snprintf(s_dash_read_err, sizeof(s_dash_read_err), "open refused");
        return false;
    }
    if (!muse_link_req_send(id, body, strlen(body), true, 2000)) {
        ESP_LOGW(TAG, "dash: /fs/read send failed at offset %u", (unsigned)offset);
        snprintf(s_dash_read_err, sizeof(s_dash_read_err), "send failed");
        muse_link_req_cancel(id);
        return false;
    }
    if (xSemaphoreTake(s_dash_done, pdMS_TO_TICKS(DASH_CHUNK_WAIT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "dash: /fs/read no reply in %d ms at offset %u",
                 DASH_CHUNK_WAIT_MS, (unsigned)offset);
        snprintf(s_dash_read_err, sizeof(s_dash_read_err), "no reply");
        muse_link_req_cancel(id);
        s_dash_gen++;   /* a late frame of this request drops, not lands in
                         * the next chunk's envelope */
        return false;
    }
    if (s_dash_status != 200 || s_dash_env_over) {
        ESP_LOGW(TAG, "dash: /fs/read status %d%s at offset %u", s_dash_status,
                 s_dash_env_over ? " (reply too large)" : "", (unsigned)offset);
        if (s_dash_env_over) {
            snprintf(s_dash_read_err, sizeof(s_dash_read_err), "reply too large");
        } else {
            snprintf(s_dash_read_err, sizeof(s_dash_read_err), "status %d", s_dash_status);
        }
        return false;
    }
    s_dash_env[s_dash_env_len] = '\0';
    /* The reply is an {ok, result} envelope: result carries the chunk as
     * UTF-8 text (or data_base64) and eof once the file is done. */
    bool ok = false;
    *eof = false;
    cJSON *root = cJSON_Parse(s_dash_env);
    if (root) {
        cJSON *res = cJSON_GetObjectItemCaseSensitive(root, "result");
        cJSON *okj = cJSON_GetObjectItemCaseSensitive(root, "ok");
        if (res && (!okj || cJSON_IsTrue(okj))) {
            *eof = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(res, "eof"));
            cJSON *text = cJSON_GetObjectItemCaseSensitive(res, "text");
            cJSON *b64 = cJSON_GetObjectItemCaseSensitive(res, "data_base64");
            if (cJSON_IsString(text) && text->valuestring) {
                size_t n = strlen(text->valuestring);
                if (*total + n <= DASH_CAP) {
                    memcpy(s_dash_new + *total, text->valuestring, n);
                    *total += n;
                    ok = true;
                }
            } else if (cJSON_IsString(b64) && b64->valuestring) {
                size_t out = 0;
                if (mbedtls_base64_decode((unsigned char *)s_dash_new + *total, DASH_CAP - *total,
                                          &out, (const unsigned char *)b64->valuestring,
                                          strlen(b64->valuestring)) == 0) {
                    *total += out;
                    ok = true;
                }
            }
        }
        cJSON_Delete(root);
    }
    if (ok) {
        ESP_LOGI(TAG, "dash: chunk at %u -> %u bytes, eof %d",
                 (unsigned)offset, (unsigned)*total, *eof ? 1 : 0);
    } else {
        ESP_LOGW(TAG, "dash: /fs/read reply at %u is not a usable envelope (%u bytes)",
                 (unsigned)offset, (unsigned)s_dash_env_len);
        snprintf(s_dash_read_err, sizeof(s_dash_read_err), "bad envelope");
    }
    return ok;
}

static void dash_fetch_task(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_dash_go, portMAX_DELAY);
        /* One deadline for the whole fetch — the link wait and every chunk
         * together — so "更新中…" always resolves: a success replaces the
         * cache, anything else keeps it and frees the tile. */
        int64_t deadline = esp_timer_get_time() + DASH_FETCH_DEADLINE_US;
        /* On a wake the link may still be coming up: give it a while. */
        while (!muse_link_req_ready() && esp_timer_get_time() < deadline) {
            vTaskDelay(pdMS_TO_TICKS(250));
        }
        size_t total = 0;
        bool ok = muse_link_req_ready();
        ESP_LOGI(TAG, "dash: fetch start (link %s)", ok ? "ready" : "down");
        s_dash_raw_err[0] = '\0';
        s_dash_read_err[0] = '\0';
        if (!ok) {
            snprintf(s_dash_err, sizeof(s_dash_err), "link down");
        }
        /* dev.11: GET /fs/raw first; POST /fs/read is the fallback. got =
         * s_dash_new holds a parseable edition of total bytes. */
        bool got = false;
        if (ok && dash_raw_fetch(&total, deadline)) {
            got = true;
        } else if (ok) {
            total = 0;
            bool eof = false, rd_ok = true;
            for (int i = 0; rd_ok && !eof && i < 16; i++) {
                if (esp_timer_get_time() >= deadline) {
                    ESP_LOGW(TAG, "dash: fetch over its deadline at %u bytes", (unsigned)total);
                    snprintf(s_dash_read_err, sizeof(s_dash_read_err), "deadline");
                    rd_ok = false;
                    break;
                }
                size_t before = total;
                rd_ok = dash_read_chunk(total, &total, &eof);
                if (rd_ok && !eof && total == before) {
                    rd_ok = false;   /* no progress and not eof: do not spin */
                }
            }
            if (rd_ok && eof && total > 0) {
                s_dash_new[total] = '\0';
                /* Only a parseable edition replaces the cache. */
                cJSON *probe = cJSON_Parse(s_dash_new);
                if (probe) {
                    cJSON_Delete(probe);
                    got = true;
                } else {
                    ESP_LOGW(TAG, "dash: fetched %u bytes but not JSON; cache kept",
                             (unsigned)total);
                    snprintf(s_dash_read_err, sizeof(s_dash_read_err), "not json");
                }
            }
        }
        bool cached = false;
        if (got && total > 0) {
            memcpy(s_dash_json, s_dash_new, total + 1);
            s_dash_ok_us = esp_timer_get_time();
            s_dash_err[0] = '\0';   /* a full fetch landed: no failure to show */
            s_dash_dirty = true;
            cached = true;
            ESP_LOGI(TAG, "dash: edition fetched, %u bytes", (unsigned)total);
        }
        if (!cached) {
            /* Name both routes when both were tried and failed. */
            if (s_dash_raw_err[0] && s_dash_read_err[0]) {
                snprintf(s_dash_err, sizeof(s_dash_err), "raw %s/read %s",
                         s_dash_raw_err, s_dash_read_err);
            } else if (s_dash_raw_err[0]) {
                snprintf(s_dash_err, sizeof(s_dash_err), "raw %s", s_dash_raw_err);
            } else if (s_dash_read_err[0]) {
                snprintf(s_dash_err, sizeof(s_dash_err), "read %s", s_dash_read_err);
            }
            ESP_LOGW(TAG, "dash: fetch failed; showing the cached edition");
        }
        s_dash_busy = false;
    }
}

static void dash_request(void)
{
    if (!s_dash_task || s_dash_busy) {
        return;
    }
    s_dash_busy = true;
    xSemaphoreGive(s_dash_go);
}

/* From update_sleep(), on the wake edge. Consumed by update_dash(). */
static void dash_note_wake(void)
{
    s_dash_wake = true;
}

static void on_dash_update(lv_event_t *e)
{
    (void)e;
    dash_request();
}

/* Rebuild the tile's words from the cached edition. LVGL task only. */
static void render_dash(void)
{
    char sub[96] = "";
    cJSON *root = s_dash_json && s_dash_json[0] ? cJSON_Parse(s_dash_json) : NULL;
    if (root) {
        cJSON *kind = cJSON_GetObjectItemCaseSensitive(root, "kind");
        cJSON *updated = cJSON_GetObjectItemCaseSensitive(root, "updated");
        const char *k = cJSON_IsString(kind) ? kind->valuestring : "";
        const char *kn = !strcmp(k, "morning") ? "朝版" : !strcmp(k, "evening") ? "夕版" : "";
        const char *up = cJSON_IsString(updated) ? updated->valuestring : "";
        snprintf(sub, sizeof(sub), "%s%s%s", kn, kn[0] && up[0] ? "  " : "", up);
    }
    /* dev.16: the clock face's two small lines come from these same
     * sections — the weather section's first line, and the schedule
     * section's first line as the next event. update_face_mode() shows
     * them only while the fetch behind them is fresh. */
    s_dash_wx[0] = '\0';
    s_dash_next[0] = '\0';
    s_dash_info_us = root ? s_dash_ok_us : 0;
    cJSON *secs0 = root ? cJSON_GetObjectItemCaseSensitive(root, "sections") : NULL;
    if (cJSON_IsArray(secs0)) {
        cJSON *sec;
        cJSON_ArrayForEach(sec, secs0) {
            cJSON *t = cJSON_GetObjectItemCaseSensitive(sec, "t");
            cJSON *lines = cJSON_GetObjectItemCaseSensitive(sec, "lines");
            const char *title = cJSON_IsString(t) ? t->valuestring : "";
            cJSON *first = cJSON_IsArray(lines) && cJSON_GetArraySize(lines) > 0
                               ? cJSON_GetArrayItem(lines, 0) : NULL;
            const char *line = cJSON_IsString(first) ? first->valuestring : "";
            if (!line[0]) {
                continue;
            }
            if (!s_dash_wx[0] && strstr(title, "天気")) {
                snprintf(s_dash_wx, sizeof(s_dash_wx), "%s", line);
            } else if (!s_dash_next[0] && strstr(title, "予定")) {
                snprintf(s_dash_next, sizeof(s_dash_next), "次: %s", line);
            }
        }
    }
    if (strcmp(sub, lv_label_get_text(s_dash_sub)) != 0) {
        lv_label_set_text(s_dash_sub, sub);
    }
    const lv_font_t *font = caption_font();
    lv_obj_clean(s_dash_box);
    int y = 0;
    bool shown = false;
    cJSON *sections = root ? cJSON_GetObjectItemCaseSensitive(root, "sections") : NULL;
    if (cJSON_IsArray(sections)) {
        cJSON *sec;
        cJSON_ArrayForEach(sec, sections) {
            cJSON *t = cJSON_GetObjectItemCaseSensitive(sec, "t");
            cJSON *lines = cJSON_GetObjectItemCaseSensitive(sec, "lines");
            const char *title = cJSON_IsString(t) ? t->valuestring : "";
            /* Join the section's lines, each prefixed with "・" unless it
             * already carries a bullet of its own. */
            size_t used = 0;
            s_dash_scratch[0] = '\0';
            if (cJSON_IsArray(lines)) {
                cJSON *line;
                cJSON_ArrayForEach(line, lines) {
                    if (!cJSON_IsString(line) || !line->valuestring[0]) {
                        continue;
                    }
                    const char *s = line->valuestring;
                    bool bullet = !strncmp(s, "・", 3) || !strncmp(s, "•", 3);
                    size_t n = strlen(s);
                    if (used + (bullet ? 0 : 3) + n + 2 > DASH_CAP) {
                        break;
                    }
                    if (!bullet) {
                        memcpy(s_dash_scratch + used, "・", 3);
                        used += 3;
                    }
                    memcpy(s_dash_scratch + used, s, n);
                    used += n;
                    s_dash_scratch[used++] = '\n';
                }
                if (used) {
                    s_dash_scratch[used - 1] = '\0';   /* no trailing newline */
                }
            }
            if (!title[0] && !used) {
                continue;   /* an empty section draws no card */
            }
            /* One card per section: rounded, a step above the tile's black,
             * an accent bar beside the title, the lines underneath. */
            lv_obj_t *card = lv_obj_create(s_dash_box);
            lv_obj_remove_style_all(card);
            lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_CARD_BG), 0);
            lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(card, 14, 0);
            lv_obj_set_pos(card, 0, y);
            lv_obj_set_width(card, 340);
            int cy = 12;   /* top padding */
            if (title[0]) {
                lv_obj_t *h = make_label(card, font, COLOR_LIT);
                lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_LEFT, 0);
                lv_label_set_text(h, title);
                lv_obj_set_pos(h, 12 + 4 + 10, cy);
                lv_obj_update_layout(h);
                lv_obj_t *bar = lv_obj_create(card);
                lv_obj_remove_style_all(bar);
                lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
                lv_obj_set_size(bar, 4, lv_obj_get_height(h));
                lv_obj_set_style_radius(bar, 2, 0);
                lv_obj_set_style_bg_color(bar, lv_color_hex(COLOR_CARD_BAR), 0);
                lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
                lv_obj_set_pos(bar, 12, cy);
                cy += lv_obj_get_height(h) + 6;
            }
            if (used) {
                lv_obj_t *b = make_label(card, font, COLOR_CAPTION);
                lv_obj_set_style_text_align(b, LV_TEXT_ALIGN_LEFT, 0);
                lv_obj_set_width(b, 302);
                lv_label_set_long_mode(b, LV_LABEL_LONG_MODE_WRAP);
                lv_obj_set_style_text_line_space(b, 6, 0);
                lv_label_set_text(b, s_dash_scratch);
                lv_obj_set_pos(b, 12 + 4 + 10, cy);
                lv_obj_update_layout(b);
                cy += lv_obj_get_height(b);
            }
            lv_obj_set_height(card, cy + 10);   /* bottom padding */
            y += cy + 10 + 10;                  /* the card, then the gap */
            shown = true;
        }
    }
    if (!shown) {
        lv_obj_t *none = make_label(s_dash_box, font, COLOR_DIM);
        lv_label_set_text(none, root ? "表示できる項目がありません" : "まだ届いていません");
        lv_obj_set_pos(none, 10, 0);
    }
    if (root) {
        trains_consider(root);   /* dev.22: cache a new timetable, if this edition carries one */
        cJSON_Delete(root);
    }
}

/*
 * dev.22: the next train — the watch side. The edition may carry a
 * "trains" block (the schema is in work/development-status.md's
 * dev.22 entry): one station, one direction, three day-type tables
 * of departure minutes, the walk minutes to the platform, and the
 * day types the edition asserts for today and tomorrow (holidays are
 * Muse's to know; the watch only knows weekdays). render_dash()
 * hands each new edition here; a block that validates replaces the
 * NVS cache (in the care handle, beside the pomodoro), and from then
 * on the countdown is pure RTC arithmetic in trains_line() — no
 * fetch, no polling, and an edition without the block (or no edition
 * at all yet) simply leaves the cache standing. The railway day runs
 * from 03:00: earlier than that, the small hours still belong to
 * yesterday's table, whose entries run past 1440 to 25:59.
 */
typedef struct {
    uint8_t v, walk, dt_today, dt_tom;
    char date[11];
    uint8_t n[3];
    uint16_t mins[3][TRAINS_MAX];
} trains_blob_t;

static uint32_t trains_hash(const trains_blob_t *b)
{
    const uint8_t *p = (const uint8_t *)b;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < sizeof(*b); i++) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}

static void trains_load(void)
{
    if (s_trains.loaded) {
        return;
    }
    s_trains.loaded = true;
    care_load();   /* opens the shared NVS handle */
    if (!s_care_nvs) {
        return;
    }
    trains_blob_t blob;
    size_t len = sizeof(blob);
    if (nvs_get_blob(s_care_nvs, "trains", &blob, &len) != ESP_OK || len != sizeof(blob)
        || blob.v != 1) {
        return;
    }
    for (int t = 0; t < 3; t++) {
        if (blob.n[t] > TRAINS_MAX) {
            return;
        }
    }
    s_trains.present = true;
    s_trains.walk = blob.walk <= 30 ? blob.walk : 0;
    snprintf(s_trains.date, sizeof(s_trains.date), "%s", blob.date);
    s_trains.dt_today = blob.dt_today <= 2 ? blob.dt_today : 0;
    s_trains.dt_tom = blob.dt_tom <= 2 ? blob.dt_tom : 0;
    for (int t = 0; t < 3; t++) {
        s_trains.n[t] = blob.n[t];
        memcpy(s_trains.mins[t], blob.mins[t], sizeof(blob.mins[t]));
    }
    s_trains.hash = trains_hash(&blob);
}

static void trains_consider(cJSON *root)
{
    cJSON *tr = cJSON_GetObjectItemCaseSensitive(root, "trains");
    if (!cJSON_IsObject(tr)) {
        return;   /* an edition without the block leaves the cache standing */
    }
    cJSON *v = cJSON_GetObjectItemCaseSensitive(tr, "v");
    if (!cJSON_IsNumber(v) || v->valueint != 1) {
        return;
    }
    trains_blob_t blob;
    memset(&blob, 0, sizeof(blob));
    blob.v = 1;
    cJSON *walk = cJSON_GetObjectItemCaseSensitive(tr, "walk");
    blob.walk = cJSON_IsNumber(walk) && walk->valueint >= 0 && walk->valueint <= 30
                    ? (uint8_t)walk->valueint : 0;
    static const char *const KEYS[3] = { "wd", "sat", "hol" };
    for (int t = 0; t < 3; t++) {
        cJSON *arr = cJSON_GetObjectItemCaseSensitive(tr, KEYS[t]);
        if (!cJSON_IsArray(arr)) {
            return;   /* all three tables, or the block is not trusted */
        }
        int n = cJSON_GetArraySize(arr);
        if (n > TRAINS_MAX) {
            n = TRAINS_MAX;
        }
        for (int i = 0; i < n; i++) {
            cJSON *m = cJSON_GetArrayItem(arr, i);
            if (!cJSON_IsNumber(m) || m->valueint < 0 || m->valueint > 1500) {
                return;
            }
            blob.mins[t][i] = (uint16_t)m->valueint;
        }
        blob.n[t] = (uint8_t)n;
    }
    cJSON *days = cJSON_GetObjectItemCaseSensitive(tr, "days");
    if (cJSON_IsObject(days)) {
        cJSON *date = cJSON_GetObjectItemCaseSensitive(days, "date");
        cJSON *today = cJSON_GetObjectItemCaseSensitive(days, "today");
        cJSON *tom = cJSON_GetObjectItemCaseSensitive(days, "tomorrow");
        if (cJSON_IsString(date) && strlen(date->valuestring) == 10
            && cJSON_IsNumber(today) && today->valueint >= 0 && today->valueint <= 2
            && cJSON_IsNumber(tom) && tom->valueint >= 0 && tom->valueint <= 2) {
            snprintf(blob.date, sizeof(blob.date), "%s", date->valuestring);
            blob.dt_today = (uint8_t)today->valueint;
            blob.dt_tom = (uint8_t)tom->valueint;
        }
    }
    trains_load();
    uint32_t h = trains_hash(&blob);
    if (s_trains.present && h == s_trains.hash) {
        return;   /* the same timetable again: no NVS wear */
    }
    s_trains.present = true;
    s_trains.walk = blob.walk;
    snprintf(s_trains.date, sizeof(s_trains.date), "%s", blob.date);
    s_trains.dt_today = blob.dt_today;
    s_trains.dt_tom = blob.dt_tom;
    for (int t = 0; t < 3; t++) {
        s_trains.n[t] = blob.n[t];
        memcpy(s_trains.mins[t], blob.mins[t], sizeof(blob.mins[t]));
    }
    s_trains.hash = h;
    if (s_care_nvs) {
        nvs_set_blob(s_care_nvs, "trains", &blob, sizeof(blob));
        nvs_commit(s_care_nvs);
    }
}

/* The day type for one date: the edition's flags when they name the
 * date (or the day after their date), else the weekday's own type —
 * the fallback for a stale edition, which still gets Saturdays and
 * Sundays right and only misses holidays. */
static int trains_type_for(const struct tm *d)
{
    if (s_trains.date[0]) {
        char dwant[16];
        snprintf(dwant, sizeof(dwant), "%04d-%02d-%02d",
                 d->tm_year + 1900, d->tm_mon + 1, d->tm_mday);
        if (!strcmp(dwant, s_trains.date)) {
            return s_trains.dt_today;
        }
        /* The flags' tomorrow: the day after their date. */
        struct tm base = { 0 };
        int y, mo, da;
        if (sscanf(s_trains.date, "%d-%d-%d", &y, &mo, &da) == 3) {
            base.tm_year = y - 1900;
            base.tm_mon = mo - 1;
            base.tm_mday = da + 1;   /* mktime normalises the overflow */
            base.tm_isdst = -1;
            struct tm next;
            time_t tt = mktime(&base);
            if (tt != (time_t)-1 && localtime_r(&tt, &next)) {
                char nwant[16];
                snprintf(nwant, sizeof(nwant), "%04d-%02d-%02d",
                         next.tm_year + 1900, next.tm_mon + 1, next.tm_mday);
                if (!strcmp(dwant, nwant)) {
                    return s_trains.dt_tom;
                }
            }
        }
    }
    return d->tm_wday == 0 ? 2 : d->tm_wday == 6 ? 1 : 0;
}

/* The clock face's train line: "電車 あと9分 7:42発 次7:53", or, once
 * the railway day's last train has gone, "電車 本日終了 始発 5:12".
 * The countdown is to leaving home (departure minus the walk) when
 * the edition set walk minutes; the 発 time is always the train's. */
static void trains_line(char *buf, size_t cap, const struct tm *local, bool have_time)
{
    buf[0] = '\0';
    trains_load();
    if (!have_time || !s_trains.present) {
        return;
    }
    struct tm d = *local;
    int now_min = local->tm_hour * 60 + local->tm_min;
    int rel = now_min;
    if (now_min < 180) {   /* before 03:00: still yesterday's railway day */
        time_t tt = mktime(&d);
        if (tt == (time_t)-1) {
            return;
        }
        tt -= 86400;
        if (!localtime_r(&tt, &d)) {
            return;
        }
        rel = now_min + 1440;
    }
    int type = trains_type_for(&d);
    const uint16_t *tab = s_trains.mins[type];
    int n = s_trains.n[type];
    int first = -1, second = -1;
    for (int i = 0; i < n; i++) {
        if (tab[i] > rel) {
            first = tab[i];
            if (i + 1 < n) {
                second = tab[i + 1];
            }
            break;
        }
    }
    bool next_day = false;
    if (first < 0) {   /* the day's trains are done: tomorrow's first */
        struct tm d2 = d;
        time_t tt = mktime(&d2);
        if (tt == (time_t)-1) {
            return;
        }
        tt += 86400;
        if (!localtime_r(&tt, &d2)) {
            return;
        }
        int type2 = trains_type_for(&d2);
        if (s_trains.n[type2] > 0) {
            first = s_trains.mins[type2][0] + 1440;
            next_day = true;
        }
    }
    if (first < 0) {
        return;   /* no timetable at all */
    }
    int dep_h = (first / 60) % 24, dep_m = first % 60;
    if (next_day) {
        snprintf(buf, cap, "電車 本日終了 始発 %d:%02d", dep_h, dep_m);
        return;
    }
    int away = first - rel - s_trains.walk;
    if (away < 0) {
        away = 0;
    }
    if (second >= 0) {
        snprintf(buf, cap, "電車 あと%d分 %d:%02d発 次%d:%02d",
                 away, dep_h, dep_m, (second / 60) % 24, second % 60);
    } else {
        snprintf(buf, cap, "電車 あと%d分 %d:%02d発", away, dep_h, dep_m);
    }
}

static void build_dash(lv_obj_t *tile)
{
    const lv_font_t *font = caption_font();
    lv_obj_t *title = make_label(tile, font, COLOR_LIT);
    lv_label_set_text(title, "ダッシュボード");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 40);
    s_dash_sub = make_label(tile, font, COLOR_DIM);
    lv_obj_align(s_dash_sub, LV_ALIGN_TOP_MID, 0, 74);
    s_dash_state = make_label(tile, font, COLOR_DIM);
    lv_obj_align(s_dash_state, LV_ALIGN_TOP_MID, 0, 100);
    /* The edition, scrollable like a Settings page inside its tile. */
    s_dash_box = lv_obj_create(tile);
    lv_obj_remove_style_all(s_dash_box);
    lv_obj_set_size(s_dash_box, 340, 246);
    lv_obj_align(s_dash_box, LV_ALIGN_TOP_MID, 0, 128);
    lv_obj_add_flag(s_dash_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_dash_box, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_dash_box, LV_SCROLLBAR_MODE_OFF);
    lv_obj_t *upd = hist_button(tile, font, "更新", on_dash_update);
    lv_obj_set_size(upd, 124, 48);
    lv_obj_align(upd, LV_ALIGN_BOTTOM_MID, 0, -30);

    s_dash_json = heap_caps_malloc(DASH_CAP + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_dash_new = heap_caps_malloc(DASH_CAP + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_dash_env = heap_caps_malloc(DASH_ENV_CAP + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_dash_scratch = heap_caps_malloc(DASH_CAP + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_dash_go = xSemaphoreCreateBinary();
    s_dash_done = xSemaphoreCreateBinary();
    if (s_dash_json) {
        s_dash_json[0] = '\0';   /* no edition yet, whatever else failed */
    }
    if (s_dash_json && s_dash_new && s_dash_env && s_dash_scratch && s_dash_go && s_dash_done) {
        if (msw_task_create(dash_fetch_task, "dash", 8192, NULL, tskIDLE_PRIORITY + 2, &s_dash_task) != pdPASS) {
            s_dash_task = NULL;
        }
    }
    render_dash();   /* the placeholder, until the first edition lands */
}

static void update_dash(void)
{
    if (!s_dash) return;
    if (s_dash_wake) {
        s_dash_wake = false;
        dash_request();
    }
    bool on = s_tv && lv_tileview_get_tile_active(s_tv) == s_dash;
    static bool was_on;
    if (on && !was_on && (!s_dash_ok_us || esp_timer_get_time() - s_dash_ok_us > DASH_STALE_US)) {
        dash_request();
    }
    was_on = on;
    static bool was_busy;
    if (s_dash_busy != was_busy) {
        was_busy = s_dash_busy;
        if (s_dash_busy) {
            lv_label_set_text(s_dash_state, "更新中…");
        } else if (s_dash_err[0]) {
            /* dev.10: name the stage the fetch died at, on the tile itself —
             * the "dash: …" log lines cannot be read off the device. */
            char buf[64];
            snprintf(buf, sizeof(buf), "取得失敗: %s", s_dash_err);
            lv_label_set_text(s_dash_state, buf);
        } else {
            lv_label_set_text(s_dash_state, "");
        }
    }
    if (s_dash_dirty) {
        s_dash_dirty = false;
        render_dash();
    }
    /* dev.17: a charge start (or anything else) that left history points
     * unsent gets its upload at the next wake; the request throttles. */
    muse_batt_hist_upload_request();
}

/*
 * dev.17: the battery history upload, the dash fetch's mirror image. The
 * points muse_battery keeps (muse_battery.h) go to a BATT_HIST_PATHS
 * path on the VM by POST /fs/write: the first piece overwrites, the rest
 * append, each piece's text under the body bound (AGENTS.md). Muse reads
 * the file when asked to analyse the battery. Triggers: Settings >
 * Battery asks on open, update_dash() asks on wake; batt_hist_due() keeps both polite (an hour
 * between uploads unless 4 or more points are owed, 5 minutes between
 * attempts). Failures are silent and retried at the next ask. Never beside
 * a dash fetch: the link allows 3 requests at once and the fetch matters
 * more, so an upload that finds one running waits for the next ask.
 *
 * dev.21: as shipped, the upload never landed once — and nothing on the
 * device could say so: failures went only to the log, and a piece counted
 * as written on HTTP 200 alone, without the {ok, result} envelope check
 * the /fs/read side has always done. Now a piece must also carry an
 * envelope whose ok is not false, the last attempt is kept in
 * s_batt_hist_last for the Battery page's note (the dash tile's s_dash_err
 * is the precedent), and a refusal (403, or ok:false) retries the whole
 * upload under the next path form in BATT_HIST_PATHS — the bare home path
 * drew a 403 for /fs/read in dev.10, so the daemon's answer may depend on
 * the form. A form that lands sticks for the next attempts.
 */
static const char *const BATT_HIST_PATHS[] = {
    "workspace/battery_history.json",
    "/workspace/battery_history.json",
    "battery_history.json",
};
#define BATT_HIST_PATH_COUNT (int)(sizeof(BATT_HIST_PATHS) / sizeof(BATT_HIST_PATHS[0]))
static int s_batt_hist_path_idx;   /* the form to try first; a winner sticks */
#define BATT_HIST_CAP (16 * 1024)      /* the whole file text, at most */
#define BATT_HIST_PIECE 6000           /* one /fs/write's text piece */
#define BATT_HIST_ENV_CAP (BATT_HIST_PIECE * 2 + 256)   /* the piece, JSON-escaped, plus the envelope */
#define BATT_HIST_FRESH_US (3600LL * 1000 * 1000)       /* an upload an hour old is fresh */
#define BATT_HIST_RETRY_US (5LL * 60 * 1000 * 1000)     /* between attempts */

static char *s_batt_hist_text;         /* PSRAM: the file being written */
static char *s_batt_hist_env;          /* PSRAM: one request envelope */
static char s_batt_hist_reply[1024];   /* one reply envelope (dev.21: its ok is read too) */
static size_t s_batt_hist_env_len;
static bool s_batt_hist_env_over;
static int s_batt_hist_status;
static uint32_t s_batt_hist_gen;
static SemaphoreHandle_t s_batt_hist_go, s_batt_hist_done;
static TaskHandle_t s_batt_hist_task;
static volatile bool s_batt_hist_busy;
static int64_t s_batt_hist_ok_us;      /* the last successful upload */
static int64_t s_batt_hist_try_us;     /* the last attempt */
/* dev.21: the piece in flight's outcome, filled by batt_hist_write_piece()
 * for batt_hist_task() and the page's note. */
static int s_batt_hist_piece_no;       /* the piece being written (0-based) */
static int s_batt_hist_piece_http;     /* its HTTP status, 0 if no reply arrived */
static int s_batt_hist_piece_ok;       /* its envelope ok: 1 true, 0 false, -1 absent/unread */
static bool s_batt_hist_piece_refused; /* 403 or ok:false: the daemon refused the write */
static char s_batt_hist_piece_reply[49];   /* the failing reply's first bytes, "" if none */
static muse_batt_hist_up_t s_batt_hist_last;   /* the last attempt (Battery page) */

/* On the session task: note the status, hand over, nothing more. */
static void batt_hist_on_frame(void *ctx, int status, const uint8_t *data, size_t len, bool end)
{
    if ((uint32_t)(uintptr_t)ctx != s_batt_hist_gen) {
        return;
    }
    if (status != 0) {
        s_batt_hist_status = status;
    }
    if (data && len) {
        if (s_batt_hist_env_len + len < sizeof(s_batt_hist_reply)) {
            memcpy(s_batt_hist_reply + s_batt_hist_env_len, data, len);
            s_batt_hist_env_len += len;
        } else {
            s_batt_hist_env_over = true;
        }
    }
    if (end || status < 0) {
        xSemaphoreGive(s_batt_hist_done);
    }
}

/* The file: the wrapper (version, firmware, upload time) around
 * muse_battery_hist_json()'s points and run. False: nothing worth sending. */
static bool batt_hist_build(void)
{
    int head = snprintf(s_batt_hist_text, BATT_HIST_CAP, "{\"v\":1,\"fw\":\"%s\",\"uploaded\":%lld,",
                        esp_app_get_description()->version, (long long)time(NULL));
    if (head < 0 || head >= BATT_HIST_CAP - 2) {
        return false;
    }
    int n = muse_battery_hist_json(s_batt_hist_text + head, BATT_HIST_CAP - (size_t)head - 1);
    if (n <= 0 || head + n >= BATT_HIST_CAP - 1) {
        return false;
    }
    s_batt_hist_text[head + n] = '}';
    s_batt_hist_text[head + n + 1] = '\0';
    return true;
}

/* dev.21: the reply's envelope ok, read as dash_read_chunk() reads it:
 * 1 true, 0 false, -1 the envelope names no ok, -2 unparseable. The reply
 * is NUL-terminated here (the frame handler's copy leaves room for it). */
static int batt_hist_reply_ok(void)
{
    s_batt_hist_reply[s_batt_hist_env_len] = '\0';
    cJSON *root = cJSON_Parse(s_batt_hist_reply);
    if (!root) {
        return -2;
    }
    cJSON *okj = cJSON_GetObjectItemCaseSensitive(root, "ok");
    int v = !okj ? -1 : (cJSON_IsTrue(okj) ? 1 : 0);
    cJSON_Delete(root);
    return v;
}

/* dev.21: keep the failing reply's first bytes for the Battery page. */
static void batt_hist_note_reply(void)
{
    size_t n = s_batt_hist_env_len < 48 ? s_batt_hist_env_len : 48;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s_batt_hist_reply[i];
        s_batt_hist_piece_reply[i] = c < 0x20 || c == 0x7f ? ' ' : (char)c;
    }
    s_batt_hist_piece_reply[n] = '\0';
}

/* One /fs/write of text[0..n) to path: the first piece overwrites, the
 * rest append. The text is escaped into the envelope (the history is
 * ASCII, but the run's lock names are text like any other). dev.21: the
 * piece counts as written only on HTTP 200 with an envelope whose ok is
 * not false (dash_read_chunk()'s rule); the outcome lands in the
 * s_batt_hist_piece_* statics either way. */
static bool batt_hist_write_piece(const char *path, const char *text, size_t n, bool first, int64_t deadline)
{
    s_batt_hist_piece_http = 0;
    s_batt_hist_piece_ok = -1;
    s_batt_hist_piece_refused = false;
    s_batt_hist_piece_reply[0] = '\0';
    char *e = s_batt_hist_env;
    size_t cap = BATT_HIST_ENV_CAP, len = 0;
    len += snprintf(e + len, cap - len, "{\"path\":\"%s\",\"text\":\"", path);
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)text[i];
        const char *esc = c == '"' ? "\\\"" : c == '\\' ? "\\\\" : NULL;
        if (esc) {
            if (len + 2 >= cap) {
                return false;
            }
            e[len++] = esc[0];
            e[len++] = esc[1];
        } else if (c < 0x20) {
            if (len + 6 >= cap) {
                return false;
            }
            len += snprintf(e + len, cap - len, "\\u%04x", c);
        } else {
            if (len + 1 >= cap) {
                return false;
            }
            e[len++] = (char)c;
        }
    }
    len += snprintf(e + len, cap - len, "\",\"overwrite\":%s,\"append\":%s,\"create_parent\":true}",
                    first ? "true" : "false", first ? "false" : "true");
    if (len >= cap) {
        return false;
    }
    char req_id[40];
    snprintf(req_id, sizeof(req_id), "muse-%08" PRIx32 "-%08" PRIx32, esp_random(), esp_random());
    const char *headers[] = { "x-request-id", req_id, "x-app-id", "hatch-web",
                              "Content-Type", "application/json",
                              "Accept", "application/json", NULL };
    s_batt_hist_gen++;
    s_batt_hist_env_len = 0;
    s_batt_hist_env_over = false;
    s_batt_hist_status = 0;
    xSemaphoreTake(s_batt_hist_done, 0);   /* drain a stale give */
    int64_t id = muse_link_req_open("POST", "/fs/write", headers, false, batt_hist_on_frame,
                                    (void *)(uintptr_t)s_batt_hist_gen);
    if (!id) {
        ESP_LOGW(TAG, "batt hist: /fs/write open refused (session down or busy)");
        return false;
    }
    if (!muse_link_req_send(id, e, len, true, 2000)) {
        ESP_LOGW(TAG, "batt hist: /fs/write send failed");
        muse_link_req_cancel(id);
        return false;
    }
    int64_t left_ms = (deadline - esp_timer_get_time()) / 1000;
    int wait_ms = left_ms > DASH_CHUNK_WAIT_MS ? DASH_CHUNK_WAIT_MS : (int)left_ms;
    if (wait_ms <= 0 || xSemaphoreTake(s_batt_hist_done, pdMS_TO_TICKS(wait_ms)) != pdTRUE) {
        ESP_LOGW(TAG, "batt hist: /fs/write no reply in %d ms", wait_ms);
        muse_link_req_cancel(id);
        s_batt_hist_gen++;
        return false;
    }
    s_batt_hist_piece_http = s_batt_hist_status;
    if (s_batt_hist_status != 200 || s_batt_hist_env_over) {
        ESP_LOGW(TAG, "batt hist: /fs/write status %d%s", s_batt_hist_status,
                 s_batt_hist_env_over ? " (reply too large)" : "");
        s_batt_hist_piece_refused = s_batt_hist_status == 403;
        if (s_batt_hist_env_len) {
            batt_hist_note_reply();
        }
        return false;
    }
    /* dev.21: HTTP 200 is not enough — the daemon answers {ok, result}
     * envelopes, and an ok:false write wrote nothing. */
    int env_ok = batt_hist_reply_ok();
    s_batt_hist_piece_ok = env_ok == -2 ? -1 : env_ok;
    if (env_ok == 0 || env_ok == -2) {
        ESP_LOGW(TAG, "batt hist: /fs/write envelope %s", env_ok == 0 ? "ok:false" : "unparseable");
        s_batt_hist_piece_refused = env_ok == 0;
        batt_hist_note_reply();
        return false;
    }
    return true;
}

static void batt_hist_task(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_batt_hist_go, portMAX_DELAY);
        s_batt_hist_try_us = esp_timer_get_time();
        int64_t deadline = esp_timer_get_time() + DASH_FETCH_DEADLINE_US;
        while (!muse_link_req_ready() && esp_timer_get_time() < deadline) {
            vTaskDelay(pdMS_TO_TICKS(250));
        }
        bool ok = false;
        /* dev.21: the attempt's outcome, kept for the Battery page. */
        muse_batt_hist_up_t up = { .stage = MUSE_BATT_HIST_UP_NONE, .ok = -1 };
        if (!muse_link_req_ready()) {
            up.stage = MUSE_BATT_HIST_UP_WAIT_LINK;
        } else if (s_dash_busy) {
            up.stage = MUSE_BATT_HIST_UP_DASH_BUSY;
        } else if (!batt_hist_build()) {
            up.stage = MUSE_BATT_HIST_UP_BUILD;
        } else {
            size_t total = strlen(s_batt_hist_text);
            /* dev.21: the path forms in order, starting from the one that
             * landed last. A refusal (403 / ok:false) advances to the next
             * form and retries the whole upload; any other failure ends
             * the attempt, as does the deadline. */
            for (int form = s_batt_hist_path_idx; form < BATT_HIST_PATH_COUNT && !ok; form++) {
                ok = total > 0;
                for (size_t off = 0; off < total && ok; off += BATT_HIST_PIECE) {
                    size_t n = total - off > BATT_HIST_PIECE ? BATT_HIST_PIECE : total - off;
                    s_batt_hist_piece_no = (int)(off / BATT_HIST_PIECE);
                    ok = batt_hist_write_piece(BATT_HIST_PATHS[form], s_batt_hist_text + off, n,
                                               off == 0, deadline);
                }
                up.stage = ok ? MUSE_BATT_HIST_UP_DONE : MUSE_BATT_HIST_UP_WRITE;
                up.path = form;
                up.piece = s_batt_hist_piece_no;
                up.http = s_batt_hist_piece_http;
                up.ok = s_batt_hist_piece_ok;
                strlcpy(up.reply, s_batt_hist_piece_reply, sizeof(up.reply));
                if (ok) {
                    s_batt_hist_path_idx = form;   /* the winner sticks */
                } else if (!s_batt_hist_piece_refused || esp_timer_get_time() >= deadline) {
                    break;
                }
            }
        }
        up.at = time(NULL);
        s_batt_hist_last = up;
        if (ok) {
            s_batt_hist_ok_us = esp_timer_get_time();
            muse_battery_hist_mark_sent();
            ESP_LOGI(TAG, "batt hist: history uploaded");
        } else {
            ESP_LOGW(TAG, "batt hist: upload failed; the next ask retries");
        }
        s_batt_hist_busy = false;
    }
}

static bool batt_hist_due(void)
{
    int64_t now = esp_timer_get_time();
    if (s_batt_hist_busy || now - s_batt_hist_try_us < BATT_HIST_RETRY_US) {
        return false;
    }
    int unsent = muse_battery_hist_unsent();
    if (!unsent) {
        return false;
    }
    return !s_batt_hist_ok_us || now - s_batt_hist_ok_us >= BATT_HIST_FRESH_US || unsent >= 4;
}

void muse_batt_hist_upload_request(void)
{
    if (!batt_hist_due()) {
        return;
    }
    if (!s_batt_hist_task) {
        s_batt_hist_text = heap_caps_malloc(BATT_HIST_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_batt_hist_env = heap_caps_malloc(BATT_HIST_ENV_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_batt_hist_go = xSemaphoreCreateBinary();
        s_batt_hist_done = xSemaphoreCreateBinary();
        if (!s_batt_hist_text || !s_batt_hist_env || !s_batt_hist_go || !s_batt_hist_done ||
            msw_task_create(batt_hist_task, "batthist", 8192, NULL, tskIDLE_PRIORITY + 2,
                            &s_batt_hist_task) != pdPASS) {
            s_batt_hist_task = NULL;
            return;
        }
    }
    s_batt_hist_busy = true;
    xSemaphoreGive(s_batt_hist_go);
}

void muse_batt_hist_upload_last(muse_batt_hist_up_t *out)
{
    *out = s_batt_hist_last;
}
#endif

static void build_screen(void)
{
#if CONFIG_MUSE_LATIN_FONT
    muse_text_keep_latin();   /* as muse_app_run() has: the fonts picked below depend on it */
#endif
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *face = scr;
    if (muse_board->touch) {
        /* Swipe left from Muse for settings (museStopWatch: for the
         * dashboard; Settings is one tile further, from quick settings). */
        s_tv = lv_tileview_create(scr);
        lv_obj_set_style_bg_color(s_tv, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_tv, LV_OPA_COVER, 0);
        lv_obj_set_scrollbar_mode(s_tv, LV_SCROLLBAR_MODE_OFF);
        int col = 0;
#if CONFIG_MUSE_BOARD_M5STACK_STOPWATCH
        /* museStopWatch: quick settings sit above the face and the reply
         * history below it, so the face's row is 1. */
        const int row = 1;
#else
        const int row = 0;
#endif
        if (msw_launcher_build) {   /* museStopWatch: swipe right from Muse for the launcher */
            /* LV_DIR_HOR, not only RIGHT: a swipe past the end (to the right
             * here) then bounces (elastic) instead of finding no scroll and
             * becoming a tap or long press on what is under the finger. */
            s_launcher = lv_tileview_add_tile(s_tv, col++, row, LV_DIR_HOR);
            lv_obj_set_scrollbar_mode(s_launcher, LV_SCROLLBAR_MODE_OFF);
            msw_launcher_build(s_launcher);
        }
        int face_col = col;
        s_face = lv_tileview_add_tile(s_tv, col++, row, (s_launcher ? LV_DIR_LEFT | LV_DIR_RIGHT : LV_DIR_RIGHT)
#if CONFIG_MUSE_BOARD_M5STACK_STOPWATCH
                                                       | LV_DIR_TOP | LV_DIR_BOTTOM   /* down to quick settings, up to the history */
#endif
                                                       );
        /* It never scrolls, but LVGL would size its scrollbars from all its
         * children every time it draws any part of it. */
        lv_obj_set_scrollbar_mode(s_face, LV_SCROLLBAR_MODE_OFF);
        /* Match the avatar's black canvas; the tile's theme background otherwise
         * shows as a different-coloured square around the character. */
        lv_obj_set_style_bg_color(s_face, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_face, LV_OPA_COVER, 0);
#if CONFIG_MUSE_BOARD_M5STACK_STOPWATCH
        /* museStopWatch: the dashboard takes the settings tile's old place,
         * right of the face. A tile's dir names where the view may move FROM
         * that tile (lv_tileview makes the active tile's dir the tileview's
         * scroll dir): the face is to its LEFT, so LV_DIR_LEFT only — the
         * view can slide back to the face, and cannot slide on into
         * Settings. Settings opens only from the quick tile's 設定 button
         * (lv_tileview_set_tile is not bound by the dir mask). dev.7 had
         * LV_DIR_RIGHT here, which did the exact opposite: no way back to
         * the face, and a clear run into Settings. */
        s_dash = lv_tileview_add_tile(s_tv, col++, row, LV_DIR_LEFT);
        lv_obj_set_scrollbar_mode(s_dash, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_style_bg_color(s_dash, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_dash, LV_OPA_COVER, 0);
        build_dash(s_dash);
#endif
        /* museStopWatch: LV_DIR_HOR as the launcher, so a swipe past the end is no tap. */
        s_settings = lv_tileview_add_tile(s_tv, col, row, s_launcher ? LV_DIR_HOR : LV_DIR_LEFT);
#if CONFIG_MUSE_BOARD_M5STACK_STOPWATCH
        /* Match the face: the tiles' theme background otherwise shows as a
         * different-coloured square. */
        s_quick = lv_tileview_add_tile(s_tv, face_col, 0, LV_DIR_BOTTOM);
        lv_obj_set_scrollbar_mode(s_quick, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_style_bg_color(s_quick, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_quick, LV_OPA_COVER, 0);
        build_quick(s_quick);
        s_history = lv_tileview_add_tile(s_tv, face_col, row + 1, LV_DIR_TOP);
        lv_obj_set_scrollbar_mode(s_history, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_style_bg_color(s_history, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_history, LV_OPA_COVER, 0);
        build_history(s_history);
#endif
        lv_tileview_set_tile(s_tv, s_face, LV_ANIM_OFF);   /* start on Muse */
        lv_obj_add_event_cb(s_tv, tile_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
        face = s_face;
    }

    if (!s_small && muse_board->no_ring) {
        /* A square panel: progress along the bottom edge, as the compact
         * layout has it, rather than a circle around Muse. */
        s_bar = lv_obj_create(face);
        lv_obj_remove_style_all(s_bar);
        lv_obj_set_size(s_bar, 0, 6);
        lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, 0);
        lv_obj_align(s_bar, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    } else if (!s_small) {
        /* Progress ring around the bezel. */
        int d = (s_w < s_h ? s_w : s_h) - 8;
        s_ring = lv_arc_create(face);
        lv_obj_set_size(s_ring, d, d);
        lv_obj_center(s_ring);
        lv_arc_set_bg_angles(s_ring, 0, 360);
        lv_arc_set_rotation(s_ring, 270);
        lv_arc_set_range(s_ring, 0, RING_RANGE);
        lv_arc_set_value(s_ring, 0);
        lv_obj_remove_style(s_ring, NULL, LV_PART_KNOB);
        lv_obj_remove_flag(s_ring, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_arc_width(s_ring, 6, LV_PART_MAIN);
        lv_obj_set_style_arc_color(s_ring, lv_color_hex(COLOR_RING_BG), LV_PART_MAIN);
        lv_obj_set_style_arc_width(s_ring, 6, LV_PART_INDICATOR);
        lv_obj_set_style_arc_rounded(s_ring, false, LV_PART_INDICATOR);
        lv_obj_add_event_cb(s_ring, on_ring_draw, LV_EVENT_DRAW_MAIN | LV_EVENT_PREPROCESS, NULL);
    }

    /*
     * Full layout, from the bottom up (offsets from the centre): two caption
     * lines as low as the ring leaves them room, the level meter, then Muse,
     * whose blank bottom rows can tuck in behind the meter.
     */
    int ring_in = (s_w < s_h ? s_w : s_h) / 2 - 10;   /* the ring's inner edge */
    int cap_h = 2 * lv_font_get_line_height(&lv_font_unscii_16) + CAPTION_LINE_SPACE;
    int cap_bottom = 179;                              /* a 466 px circle's; fine for rectangles */
    if (muse_board->round) {
        cap_bottom = (int)sqrtf((float)(ring_in * ring_in - CAPTION_W * CAPTION_W / 4)) - 3;
    }
    int cap_top = cap_bottom - cap_h;
    int meter_y = cap_top - 6 - METER_SEG_PX / 2;
    int art_bottom = meter_y - METER_SEG_PX / 2 - 4;
    s_big_y = s_small ? 0 : art_bottom - (s_canvas_px / 2 - ART_BLANK_ROWS * (s_canvas_px / MUSE_PX_W));
#if CONFIG_MUSE_BOARD_SPOTPEAR_ESP32_AI_128
    /* The 128 px avatar ends at y=159; Japanese captions start at y=164. */
    s_big_y = -25;
#endif

    /* The character. */
    muse_image_init();
    s_canvas = lv_image_create(face);
    lv_image_set_src(s_canvas, &s_muse_src);
    lv_obj_align(s_canvas, LV_ALIGN_CENTER, 0, s_big_y);
    s_muse_y = s_big_y;
    lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_canvas, on_canvas_clicked, LV_EVENT_CLICKED, NULL);
    if (s_ring) {
        /* The canvas's black corners reach the bezel; keep the ring on top. */
        lv_obj_move_foreground(s_ring);
    }
    build_button_icons(face);

    /* Status line: connectivity icons + power. */
    lv_obj_t *status = lv_obj_create(face);
    lv_obj_remove_style_all(status);
    lv_obj_remove_flag(status, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(status, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(status, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(status, s_small ? 4 : 8, 0);
    lv_obj_align(status, LV_ALIGN_TOP_MID, 0, muse_board->rim_controls ? 40 : s_small ? 1 : 20 + s_dy);
    s_wifi_icon = make_label(status, &lv_font_montserrat_14, COLOR_DIM);
    s_ble_icon = make_label(status, &lv_font_montserrat_14, COLOR_DIM);
    s_power_lbl = make_label(status, &lv_font_unscii_8, COLOR_DIM);

    /* The compact layout leaves the state to the avatar and the caption,
     * unless the screen is tall enough to fit it in small type above Muse. */
    s_state_lbl = make_label(face, s_small ? &lv_font_unscii_8 : &lv_font_unscii_16, 0xffffff);
    lv_obj_set_style_text_letter_space(s_state_lbl, s_small ? 1 : 2, 0);
    lv_obj_align(s_state_lbl, LV_ALIGN_TOP_MID, 0, muse_board->rim_controls ? 64 : s_small ? 22 : 40 + s_dy);
    lv_obj_set_flag(s_state_lbl, LV_OBJ_FLAG_HIDDEN, s_small && !s_tall && s_h < 200);

    /* This gadget's own name, dim under the state while it's unpaired: with
     * more than one on the bench, the screen says which one to pick in the
     * Muse app. update_chrome() fills it in, shortens it to the hex tail on a
     * screen too narrow for the whole thing, and empties it once paired. */
    s_name_lbl = make_label(face, s_small ? &lv_font_unscii_8 : &lv_font_unscii_16, COLOR_DIM);
    /* Under the state, which a small screen draws in unscii-8 or, with accented
     * letters, the caption font's 16 px (update_status). */
    int state_h = lv_font_get_line_height(muse_lang_label_font(muse_lang_status("READY"), &lv_font_unscii_8));
    lv_obj_align(s_name_lbl, LV_ALIGN_TOP_MID, 0, muse_board->rim_controls ? 88 : s_small ? 24 + state_h : 60 + s_dy);
    if (muse_board->rim_controls) {
        /* Safe chord below the round panel's top, not the full 466 px width. */
        lv_obj_set_size(s_state_lbl, 280, lv_font_get_line_height(&lv_font_unscii_16));
        lv_obj_set_size(s_name_lbl, 280, lv_font_get_line_height(&lv_font_unscii_16));
        lv_label_set_long_mode(s_state_lbl, LV_LABEL_LONG_MODE_DOTS);
        lv_label_set_long_mode(s_name_lbl, LV_LABEL_LONG_MODE_DOTS);
    }
    /* Same rule as the state label: a square 128 px screen centres Muse over
     * these rows, so there's nowhere to put this without covering the face. */
    lv_obj_set_flag(s_name_lbl, LV_OBJ_FLAG_HIDDEN, s_small && !s_tall && s_h < 200);

    /* museStopWatch: whose voice voice.play plays, dim near the bottom (the
     * reply takes the top), on the round panel's chord there. */
    s_cv_lbl = make_label(face, caption_font(), COLOR_DIM);
    lv_obj_set_size(s_cv_lbl, 240, LV_SIZE_CONTENT);
    lv_obj_set_style_text_align(s_cv_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_cv_lbl, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(s_cv_lbl, LV_ALIGN_BOTTOM_MID, 0, -40);
    lv_obj_add_flag(s_cv_lbl, LV_OBJ_FLAG_HIDDEN);

    s_caption_lbl = make_label(face, font_pick(caption_font(), &lv_font_unscii_8), COLOR_CAPTION);
    lv_obj_add_flag(s_caption_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_caption_lbl, on_caption_clicked, LV_EVENT_CLICKED, NULL);
    if (s_small) {
        /* Two lines over the bottom of the face, on a dark band so they stay
         * legible. A tall screen has room to keep them above the mic icon. */
        lv_obj_set_size(s_caption_lbl, SMALL_BAND_W, caption_band_h());
        lv_obj_set_style_pad_ver(s_caption_lbl, 2, 0);
        lv_obj_set_style_text_line_space(s_caption_lbl, 2, 0);
        lv_obj_set_style_bg_color(s_caption_lbl, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_caption_lbl, LV_OPA_70, 0);
        lv_label_set_long_mode(s_caption_lbl, LV_LABEL_LONG_MODE_DOTS);
        /* Touch screens need the caption above the navigation dots too. */
        lv_obj_align(s_caption_lbl, LV_ALIGN_BOTTOM_MID, 0, SMALL_BAND_Y);
#if CONFIG_MUSE_CJK_FONT
        /* CJK doesn't fit unscii-8's cell, so a reply with CJK in it fills the
         * band in the 16 px caption font instead: one line at a time, or two
         * on a landscape screen with the height for them, such as the
         * BOX-3's 320x240 (set_caption_font). */
        s_cjk_lines = !s_tall && s_h >= 200 ? 2 : 1;
        muse_state_set_cjk_page((SMALL_BAND_W - 2 * CJK_BAND_PAD) / caption_cjk_cw(),
                                s_cjk_lines);
#endif
#if CONFIG_MUSE_LATIN_FONT
        /* Accented letters take one line of Unifont's 8x16, clear of the
         * button icons in the band's corners (8 px in, up to 14 px wide) and
         * of a case's bezel. Bigger screens page them as their other replies,
         * Unifont being the narrower. So those pages fill half their width; a
         * page of their own there needs MUSE_CAPTION_MAX to allow for 3-byte
         * letters. */
        muse_state_set_latin_page((SMALL_BAND_W - 2 * LATIN_BAND_MARGIN) / lv_font_get_glyph_width(&muse_font_latin_16, 'M', ' '), 1);
#endif
#if CONFIG_MUSE_SMALL_FULL_REPLY
        if (!s_long) {   /* the long layout has its own page of the reply */
            s_full_reply_h = s_h - FULL_REPLY_TOP - 6;
            muse_chat_set_caption_px(s_w - 6, full_glyph_px);
            apply_text_size();
        }
#endif
        if (s_long) {
            /* Not a band over Muse but a page of the reply under it. Montserrat's
             * letters differ in width, so the pager wraps each line to the
             * label's width in pixels as the label measures it: a line never
             * wraps again on screen, and every page fits. */
            const lv_font_t *font = &lv_font_montserrat_16;
            int top = LONG_MUSE_TOP + s_canvas_px - ART_BLANK_ROWS * (s_canvas_px / MUSE_PX_W) + 14;
            int pitch = lv_font_get_line_height(font) + LONG_LINE_SPACE;
            int lines = (s_h - LONG_HINTS_H - top + LONG_LINE_SPACE) / pitch;
            if (lines < 1) {
                lines = 1;   /* the columns below divide by it */
            }
            int w = s_w - 24;
            /* A third of the caption spare for characters wider than a byte:
             * this caps a line of narrow letters. */
            int cols = MUSE_CAPTION_MAX * 2 / 3 / lines - 1;
            muse_chat_set_caption_px(w, long_glyph_px);
            lv_obj_set_style_text_font(s_caption_lbl, font, 0);
            lv_obj_set_style_text_line_space(s_caption_lbl, LONG_LINE_SPACE, 0);
            lv_obj_set_style_pad_all(s_caption_lbl, 0, 0);
            lv_obj_set_style_bg_opa(s_caption_lbl, LV_OPA_TRANSP, 0);
            lv_obj_set_style_text_align(s_caption_lbl, LV_TEXT_ALIGN_LEFT, 0);
            lv_obj_set_size(s_caption_lbl, w, lines * pitch - LONG_LINE_SPACE);
            lv_obj_align(s_caption_lbl, LV_ALIGN_TOP_MID, 0, top);
            muse_state_set_page(cols, lines);
            /* A reply with CJK or accented letters takes their font's columns,
             * but its pages without any show in Montserrat, so every page keeps
             * Montserrat's rows, and the pager its pixel widths too. */
#if CONFIG_MUSE_CJK_FONT
            muse_state_set_cjk_page(w / caption_cjk_cw(), lines);
#endif
#if CONFIG_MUSE_LATIN_FONT
            muse_state_set_latin_page(w / lv_font_get_glyph_width(&muse_font_latin_16, 'M', ' '), lines);
#endif
            if (muse_board->touch) {
                build_speaker(face, s_w / 2 - 96, s_h / 2 - 75);   /* in the bottom strip, left of the key hints */
            }
        }

        s_bar = lv_obj_create(face);
        lv_obj_remove_style_all(s_bar);
        lv_obj_set_size(s_bar, 0, 3);
        lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, 0);
        lv_obj_align(s_bar, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        return;
    }
    /* Fixed height: a longer caption ends in dots rather than growing into the ring. */
    lv_obj_set_size(s_caption_lbl, CAPTION_W, cap_h);
    lv_obj_set_style_text_line_space(s_caption_lbl, CAPTION_LINE_SPACE, 0);
    lv_label_set_long_mode(s_caption_lbl, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(s_caption_lbl, LV_ALIGN_CENTER, 0, cap_top + cap_h / 2);
    s_cap_h = cap_h;
    s_cap_y = cap_top + cap_h / 2;

    /* Chunky level meter. */
    int span = METER_SEGS * (METER_SEG_PX + METER_GAP_PX) - METER_GAP_PX;
    for (int i = 0; i < METER_SEGS; i++) {
        lv_obj_t *seg = lv_obj_create(face);
        lv_obj_remove_style_all(seg);
        lv_obj_remove_flag(seg, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(seg, METER_SEG_PX, METER_SEG_PX);
        lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(seg, lv_color_hex(COLOR_METER_OFF), 0);
        int x = -span / 2 + i * (METER_SEG_PX + METER_GAP_PX) + METER_SEG_PX / 2;
        lv_obj_align(seg, LV_ALIGN_CENTER, x, meter_y);
        s_meter[i] = seg;
    }

#if CONFIG_MUSE_BOARD_M5STACK_STOPWATCH
    /* Approval buttons, over the bottom of the face: hidden until a
     * question from Claude waits with its kept reply (update_status). */
    s_ask_ok = hist_button(face, caption_font(), "承認", on_ask_ok);
    lv_obj_set_size(s_ask_ok, 124, 52);
    lv_obj_align(s_ask_ok, LV_ALIGN_BOTTOM_MID, -72, -24);
    lv_obj_add_flag(s_ask_ok, LV_OBJ_FLAG_HIDDEN);
    s_ask_no = hist_button(face, caption_font(), "やめる", on_ask_no);
    lv_obj_set_size(s_ask_no, 124, 52);
    lv_obj_align(s_ask_no, LV_ALIGN_BOTTOM_MID, 72, -24);
    lv_obj_add_flag(s_ask_no, LV_OBJ_FLAG_HIDDEN);
#endif

    build_answer(face, ring_in);
#if CONFIG_MUSE_BOARD_M5STACK_STOPWATCH
    build_care(face);    /* dev.14: the care panel, food and accessories */
    build_clock(face);   /* dev.14: the yellow button's clock face */
    build_timer(face);   /* dev.16: the countdown timer panel and its alert */
    build_pomodoro(face);   /* dev.22: the pomodoro panel and its alert */
#endif
}


static void on_cover_event(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
        /* Wake on touch; keep the (now clear) cover until release so the tap
         * doesn't land on whatever is underneath. */
        muse_state_set_asleep(false);
        lv_obj_set_style_bg_opa(s_cover, LV_OPA_TRANSP, 0);
    } else if (!muse_state_asleep()) {
        lv_obj_add_flag(s_cover, LV_OBJ_FLAG_HIDDEN);
    }
}

/* With the display lock held. */
static void image_hide_locked(void)
{
    xSemaphoreTake(s_image_mutex, portMAX_DELAY);
    if (s_image_buf) {
        lv_obj_add_flag(s_image, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_src(s_image, NULL);
        heap_caps_free(s_image_buf);
        s_image_buf = NULL;
        s_image_dsc.data = NULL;
        s_image_dirty = false;
    }
    xSemaphoreGive(s_image_mutex);
}

/* Each frame: shows a new image and redraws what the download changed. */
static void image_sync(void)
{
    xSemaphoreTake(s_image_mutex, portMAX_DELAY);
    if (s_image_buf && !s_image_dsc.data) {
        s_image_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        s_image_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        s_image_dsc.header.w = s_w;
        s_image_dsc.header.h = s_h;
        s_image_dsc.header.stride = s_w * sizeof(uint16_t);
        s_image_dsc.data_size = (size_t)s_w * s_h * sizeof(uint16_t);
        s_image_dsc.data = (const uint8_t *)s_image_buf;
        muse_menu_close();
        lv_image_set_src(s_image, &s_image_dsc);
        lv_obj_remove_flag(s_image, LV_OBJ_FLAG_HIDDEN);
        muse_state_set_asleep(false);
    }
    if (s_image_dirty) {
        lv_obj_invalidate_area(s_image, &s_image_area);
        s_image_dirty = false;
    }
    xSemaphoreGive(s_image_mutex);
}

static void on_image_clicked(lv_event_t *e)
{
    (void)e;
#if CONFIG_MUSE_WATCHER_CAMERA
    if (watcher_camera_preview_active()) {
        watcher_camera_preview_toggle();
        return;
    }
#endif
    image_hide_locked();
}

#if CONFIG_MUSE_WATCHER_CAMERA
static void on_camera_hint_clicked(lv_event_t *e)
{
    (void)e;
    if (watcher_camera_preview_active()) watcher_camera_preview_toggle();
}
#endif

static void on_any_press(lv_event_t *e)
{
    (void)e;
    if (muse_board->touch_talk && muse_link_state() == MUSE_LINK_CONFIRM) {
        muse_input_touch_talk(true);
        muse_input_touch_talk(false);
    }
    muse_state_poke();
}

/* Icons only beside the physical controls, including before pairing.
 * dev.15: the hints are never created any more (see build_overlays), so
 * on this fork's board s_ptt_hint stays NULL and this is a no-op; it is
 * kept as the single guard every caller already goes through. */
static void update_control_hints(muse_mode_t mode)
{
    if (!s_ptt_hint) return;
    /* Don't obscure settings rows/keyboards with floating physical icons.
     * museStopWatch: nor the app launcher (msw_launcher_build). */
    lv_obj_t *tile = s_tv ? lv_tileview_get_tile_active(s_tv) : NULL;
    bool settings = tile && (tile == s_settings || tile == s_launcher || tile == s_quick || tile == s_history
                             || tile == s_dash);
    lv_obj_set_flag(s_ptt_hint, LV_OBJ_FLAG_HIDDEN, settings);
    lv_obj_set_flag(s_spk_hint, LV_OBJ_FLAG_HIDDEN, settings);
    lv_obj_set_flag(s_pwr_hint, LV_OBJ_FLAG_HIDDEN, settings);
    static int last_mode = -1;
    if ((int)mode != last_mode) {
        color_mic(s_ptt_hint, mode == MUSE_MODE_LISTENING ? 0x64d4ff : 0x80bfff);
        last_mode = (int)mode;
    }
    bool speaker_on = muse_settings_speaker_on();
    const char *speaker = speaker_on ? LV_SYMBOL_VOLUME_MID : LV_SYMBOL_MUTE;
    if (strcmp(speaker, lv_label_get_text(s_spk_hint))) {
        lv_label_set_text(s_spk_hint, speaker);
        lv_obj_set_style_text_color(s_spk_hint, lv_color_hex(speaker_on ? 0xffd060 : 0x8b7845), 0);
    }
}

static void build_overlays(void)
{
    lv_obj_t *scr = lv_screen_active();

    /* Page dots. */
    int dots = s_launcher ? TILES : 2;
    for (int i = 0; i < dots && s_tv; i++) {
        lv_obj_t *d = lv_obj_create(scr);
        lv_obj_remove_style_all(d);
        lv_obj_set_size(d, 8, 8);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(d, lv_color_hex(COLOR_DOT_OFF), 0);
        lv_obj_remove_flag(d, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(d, LV_ALIGN_BOTTOM_MID, 16 * i - 8 * (dots - 1), -14);
        s_dots[i] = d;
    }

    /* A downloaded image: over everything on the screen (and in snapshots),
     * under the pairing code and sleep cover on the top layer. */
    s_image = lv_image_create(scr);
    lv_obj_set_pos(s_image, 0, 0);
    lv_obj_add_flag(s_image, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_image, on_image_clicked, LV_EVENT_CLICKED, NULL);
#if CONFIG_MUSE_WATCHER_CAMERA
    s_camera_hint = lv_btn_create(scr);
    lv_obj_set_size(s_camera_hint, 244, 46);
    lv_obj_align(s_camera_hint, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_bg_color(s_camera_hint, lv_color_hex(0x201a35), 0);
    lv_obj_set_style_bg_opa(s_camera_hint, LV_OPA_90, 0);
    lv_obj_set_style_border_color(s_camera_hint, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_set_style_border_width(s_camera_hint, 2, 0);
    lv_obj_set_style_radius(s_camera_hint, 18, 0);
    lv_obj_add_flag(s_camera_hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *hint_text = lv_label_create(s_camera_hint);
    lv_label_set_text(hint_text, "TAP TO TAKE PHOTO");
    lv_obj_center(hint_text);
    lv_obj_add_event_cb(s_camera_hint, on_camera_hint_clicked, LV_EVENT_CLICKED, NULL);
#endif

    if (muse_board->rim_controls) {
        /* dev.15: the dedicated rim icons that named the three physical
         * controls (mic beside the talk button, speaker beside the mute
         * button, power beside the power button) are gone at the user's
         * request: the face shows the avatar and the real status chrome
         * only. s_ptt_hint/s_spk_hint/s_pwr_hint are never created, so
         * update_control_hints() no-ops. Live information is untouched:
         * the header's Wi-Fi/BLE glyphs and state label, the transient
         * SPEAKER ON/OFF caption on a mute toggle, and the quick tile's
         * sound-mode buttons all stay. */
#if CONFIG_MUSE_BOARD_M5STACK_STOPWATCH && CONFIG_MUSE_REALTIME_VOICE
        /* During a push-to-talk call: what yellow does now, and that blue
         * hangs up. Under each button's hint position, pushed out towards
         * the rim, clear of the avatar and its sparkles (CALL_HINT_OUT). */
        const muse_button_hint_t *t = &muse_board->talk_hint, *k = &muse_board->speaker_hint;
        s_talk_hint_lbl = make_label(scr, &lv_font_montserrat_14, 0xffdc55);
        s_end_hint_lbl = make_label(scr, &lv_font_montserrat_14, 0x70baff);
        lv_obj_set_style_text_align(s_talk_hint_lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_align(s_end_hint_lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(s_talk_hint_lbl, t->align, t->x + (t->x < 0 ? -CALL_HINT_OUT : CALL_HINT_OUT), t->y + 36);
        lv_obj_align(s_end_hint_lbl, k->align, k->x + (k->x < 0 ? -CALL_HINT_OUT : CALL_HINT_OUT), k->y + 36);
        lv_label_set_text(s_end_hint_lbl, "END CALL");
        lv_obj_add_flag(s_talk_hint_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_end_hint_lbl, LV_OBJ_FLAG_HIDDEN);
#endif
        update_control_hints(MUSE_MODE_BOOT);
    }

    /* BLE pairing code, or the Muse app's ask for the talk button: a centred
     * column in one typeface, the code large. The small card grows with the hint. */
    s_pair = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_pair);
    lv_obj_set_size(s_pair, s_small ? s_w - 8 : 300, s_small ? LV_SIZE_CONTENT : 150);
    lv_obj_center(s_pair);
    lv_obj_set_flex_flow(s_pair, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_pair, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(s_pair, s_small ? 10 : 18, 0);
    lv_obj_set_style_pad_hor(s_pair, s_small ? 6 : 16, 0);
    lv_obj_set_style_pad_row(s_pair, s_small ? 4 : 10, 0);
    lv_obj_set_style_radius(s_pair, s_small ? 10 : 24, 0);
    lv_obj_set_style_bg_opa(s_pair, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_pair, lv_color_hex(0x1a1530), 0);
    lv_obj_set_style_border_color(s_pair, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_set_style_border_width(s_pair, 2, 0);
    lv_obj_remove_flag(s_pair, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_pair, LV_OBJ_FLAG_HIDDEN);
    s_pair_title = make_label(s_pair, muse_lang_montserrat(font_pick(&lv_font_montserrat_20, FONT_COMPACT)), COLOR_LIT);
    lv_label_set_text(s_pair_title, muse_lang_menu("Pairing code"));
    s_pair_code = make_label(s_pair, muse_lang_montserrat(font_pick(&lv_font_montserrat_28, &lv_font_montserrat_20)),
                             COLOR_ACCENT);
    lv_obj_set_style_text_letter_space(s_pair_code, s_small ? 2 : 6, 0);
    s_pair_hint = make_label(s_pair, muse_lang_montserrat(font_pick(&lv_font_montserrat_14, FONT_COMPACT)), COLOR_DIM);
    lv_label_set_text(s_pair_hint, muse_lang_menu(s_small ? "Enter on phone" : "Enter it on your phone"));
    /* Wraps: "bottom right button" is wider than the AIPI's card. */
    lv_obj_set_width(s_pair_hint, lv_pct(100));
    lv_label_set_long_mode(s_pair_hint, LV_LABEL_LONG_MODE_WRAP);

    /* Sleep cover: swallows the waking touch. */
    s_cover = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_cover);
    lv_obj_set_size(s_cover, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_cover, lv_color_black(), 0);
    lv_obj_add_flag(s_cover, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_cover, on_cover_event, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_cover, on_cover_event, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_cover, on_cover_event, LV_EVENT_PRESS_LOST, NULL);

    if (s_indev) {
        lv_indev_add_event_cb(s_indev, on_any_press, LV_EVENT_PRESSED, NULL);
    }
}

static void apply_brightness(int pct)
{
    if (pct != s_brightness) {
        muse_board->set_brightness(pct);
        s_brightness = pct;
    }
}

bool muse_ui_dark(void)
{
    return s_dark;
}

/* Returns true while asleep (skip rendering). */
static bool update_sleep(void)
{
    bool asleep = muse_state_asleep();
    if (asleep && !s_dark) {
        muse_menu_close();
        apply_brightness(0);
        if (muse_board->panel_sleep) {
            muse_board->panel_sleep(true);
        }
        lv_obj_set_style_bg_opa(s_cover, LV_OPA_COVER, 0);
        lv_obj_remove_flag(s_cover, LV_OBJ_FLAG_HIDDEN);
        s_dark = true;
    } else if (!asleep && s_dark) {
        s_dark = false;
#if CONFIG_MUSE_BOARD_M5STACK_STOPWATCH
        dash_note_wake();   /* the dashboard fetches a fresh edition on wake */
#endif
        if (muse_board->panel_sleep) {
            muse_board->panel_sleep(false);
        }
        /* Woken by a button: drop the cover now. Woken by touch: on release. */
        if (lv_obj_get_style_bg_opa(s_cover, 0) == LV_OPA_COVER) {
            lv_obj_add_flag(s_cover, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (!s_dark) {
        int target = muse_settings_brightness();
        if (s_preview_brightness >= 0) {
            if (s_preview_brightness == target) {
                s_preview_brightness = -1;
            } else {
                target = s_preview_brightness;
            }
        }
        apply_brightness(target);
    }
    return s_dark;
}

/* Idle's label: whether a press reaches Hatch now or waits for Wi-Fi. */
static const char *idle_name(muse_wifi_state_t wifi)
{
    static bool joined;   /* since boot: from then on, a drop is reconnecting */
    switch (wifi) {
    case MUSE_WIFI_CONNECTED:
        joined = true;
        return MODE_NAMES[MUSE_MODE_IDLE];
    case MUSE_WIFI_OFF:
        return "WI-FI OFF";
    case MUSE_WIFI_NO_NETWORK:
        return "SET UP WI-FI";
    case MUSE_WIFI_NOT_NEARBY:
        return "NO WI-FI";   /* none of the saved networks is in range */
    default:
        return joined ? "RECONNECTING" : "CONNECTING";
    }
}

static void update_chrome(float now)
{
    if (s_cv_lbl) {
        const char *cv = msw_voice_credit ? msw_voice_credit() : NULL;
        if (cv) {
            char t[64];
            snprintf(t, sizeof(t), "CV: %s", cv);
            if (strcmp(t, lv_label_get_text(s_cv_lbl)) != 0) {
                lv_label_set_text(s_cv_lbl, t);
            }
        }
        lv_obj_set_flag(s_cv_lbl, LV_OBJ_FLAG_HIDDEN, !cv);
    }
    if (now < s_next_settings_tick) {
        return;
    }
    s_next_settings_tick = now + SETTINGS_TICK_S;

    if (s_tv) {
        int tile = tile_page();
        bool settings = lv_tileview_get_tile_active(s_tv) == s_settings;
        int page = s_launcher ? tile : settings;   /* the lit dot */
        bool subpage = muse_settings_ui_in_subpage();
        s_swipe_enabled = !settings || !subpage;
        update_tileview_swipe();
        int shown = tile * 2 + subpage;
        if (shown != s_shown_page) {
            for (int i = 0; i < TILES && s_dots[i]; i++) {
                lv_obj_set_style_bg_color(s_dots[i], lv_color_hex(i == page ? COLOR_ACCENT : COLOR_DOT_OFF), 0);
                lv_obj_set_flag(s_dots[i], LV_OBJ_FLAG_HIDDEN, settings && subpage);
            }
            s_shown_page = shown;
        }
#if CONFIG_MUSE_BOARD_M5STACK_STOPWATCH
        /* The dashboard sits between the face and Settings: Settings counts
         * as visible only while its own tile is up, not whenever the view
         * is scrolled right of the face. */
        muse_settings_ui_tick(settings);
#else
        muse_settings_ui_tick(lv_obj_get_scroll_x(s_tv) > face_scroll_x());
#endif
    }

    /* Joining, the icon blinks: the compact layout has no state label. */
    muse_wifi_status_t w;
    muse_wifi_status(&w);
    bool joining = w.state == MUSE_WIFI_CONNECTING || w.state == MUSE_WIFI_FAILED;
    const char *wifi = w.state == MUSE_WIFI_CONNECTED ||
                       (joining && (muse_board->epaper || (int)(now * 2) % 2 == 0)) ? LV_SYMBOL_WIFI : "";
    if (strcmp(wifi, lv_label_get_text(s_wifi_icon)) != 0) {
        lv_label_set_text(s_wifi_icon, wifi);
    }
    s_idle_name = idle_name(w.state);
    muse_ble_status_t b;
    muse_ble_status(&b);
    const char *ble = b.state != MUSE_BLE_OFF ? LV_SYMBOL_BLUETOOTH : "";
    if (strcmp(ble, lv_label_get_text(s_ble_icon)) != 0) {
        lv_label_set_text(s_ble_icon, ble);
        lv_obj_set_style_text_color(s_ble_icon, lv_color_hex(b.state == MUSE_BLE_CONNECTED ? COLOR_ACCENT : COLOR_DIM), 0);
    }

    /* Paired, the name has done its job (picking this one out in the Muse
     * app) and the speaker button has replies to mute. */
    muse_hatch_status_t h;
    muse_hatch_status(&h);
    bool paired = h.state != MUSE_HATCH_NOT_SET;

    /* The gadget's name, until it's paired. Emptied rather than hidden: the
     * read layout unhides it on the way out. A narrow screen gets the hex tail
     * on its own, which is the part that differs between two of them, rather
     * than a head that ends in dots before it gets there. */
    const lv_font_t *name_font = s_small ? &lv_font_unscii_8 : &lv_font_unscii_16;
    int name_cw = lv_font_get_glyph_width(name_font, 'M', ' ');
    const char *shown = paired ? "" : b.name;
#if LV_USE_SNAPSHOT
    if (!paired && s_bench_demo) shown = "MuseGadget-DEMO";
#endif
    if (name_cw > 0 && (int)strlen(shown) * name_cw > (muse_board->rim_controls ? 280 : s_w)) {
        const char *tail = strrchr(shown, '-');
        if (tail && tail[1]) {
            shown = tail + 1;
        }
    }
    if (strcmp(shown, lv_label_get_text(s_name_lbl)) != 0) {
        lv_label_set_text(s_name_lbl, shown);
    }

    /* The same card asks for the talk button when the Muse app pairs. */
    bool confirm = !b.passkey && muse_link_state() == MUSE_LINK_CONFIRM;
    if (b.passkey || confirm) {
        char code[24], hint[40];
        if (confirm) {
            if (muse_board->touch_talk || !muse_board->audio_init) {
                strlcpy(code, "Tap screen", sizeof(code));
                strlcpy(hint, "Tap to confirm pairing", sizeof(hint));
            } else {
                strlcpy(code, s_small ? "Press" : "Press button", sizeof(code));
                snprintf(hint, sizeof(hint), muse_lang_menu(s_small ? "%s button" : "Press the %s button"),
                         muse_board->talk_button);
            }
        } else {
            snprintf(code, sizeof(code), "%06lu", (unsigned long)b.passkey);
            strlcpy(hint, s_small ? "Enter on phone" : "Enter it on your phone", sizeof(hint));
        }
        const char *title = confirm ? (s_small ? "Muse app" : "Pair with Muse app") : "Pairing code";
        const char *shown = muse_lang_menu(code);
        if (strcmp(shown, lv_label_get_text(s_pair_code)) != 0) {
            lv_label_set_text(s_pair_code, shown);
            lv_label_set_text(s_pair_title, muse_lang_menu(title));
            lv_label_set_text(s_pair_hint, muse_lang_menu(hint));
        }
    }
    lv_obj_set_flag(s_pair, LV_OBJ_FLAG_HIDDEN, !b.passkey && !confirm);

    bool speaker = muse_settings_speaker_on();   /* also set from settings, the phone and serial */
    if (s_speaker && (int)speaker != s_shown_speaker) {
        show_speaker(speaker);
    }
    if (s_speaker && (paired && muse_board->audio_init) == lv_obj_has_flag(s_speaker, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_set_flag(s_speaker, LV_OBJ_FLAG_HIDDEN, !paired || !muse_board->audio_init);
    }
    /* Unpaired, a press only says "SET UP MUSE FIRST", so the mic goes too.
     * While a reply's layout is up it decides; that's only ever paired. A
     * microphone without a speaker (mic_init) still talks. */
    bool mic = paired && (muse_board->audio_init || muse_board->mic_init);
    if (s_mic_icon && !muse_board->rim_controls && s_answer < 0 && mic == lv_obj_has_flag(s_mic_icon, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_set_flag(s_mic_icon, LV_OBJ_FLAG_HIDDEN, !mic);
    }
}

/* One area for the whole meter before its segments change: LVGL won't join
 * squares with gaps between them, and each area it keeps costs a walk of the
 * widget tree. The segments' own areas then fall inside this one. */
static void invalidate_meter(void)
{
    lv_area_t a, last;
    lv_obj_get_coords(s_meter[0], &a);
    lv_obj_get_coords(s_meter[METER_SEGS - 1], &last);
    a.x2 = last.x2;
    lv_obj_invalidate_area(lv_obj_get_parent(s_meter[0]), &a);
}

static void set_meter_visible(bool visible)
{
    if (s_small || visible == s_meter_visible) {
        return;
    }
    invalidate_meter();
    for (int i = 0; i < METER_SEGS; i++) {
        if (visible) {
            lv_obj_remove_flag(s_meter[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_meter[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    s_meter_visible = visible;
    s_shown_lit = -1;
}

static void update_power(float now)
{
    if (now < s_next_power_update) {
        return;
    }
    s_next_power_update = now + 1.0f;

    muse_power_t p = muse_state_power();
    char buf[32];
    if (p.battery_pct < 0) {
        strlcpy(buf, p.usb ? (s_small ? "USB" : muse_lang_status("USB POWER")) : "", sizeof(buf));
    } else if (s_small) {
        snprintf(buf, sizeof(buf), "%s%d%%", p.charging ? "+" : "", p.battery_pct);
    } else if (p.charging) {
        snprintf(buf, sizeof(buf), muse_lang_status("CHARGING %d%%"), p.battery_pct);
    } else {
        snprintf(buf, sizeof(buf), muse_lang_status("BATTERY %d%%"), p.battery_pct);
    }
    if (strcmp(buf, lv_label_get_text(s_power_lbl)) != 0) {
        lv_obj_set_style_text_font(s_power_lbl, muse_lang_label_font(buf, &lv_font_unscii_8), 0);
        lv_label_set_text(s_power_lbl, buf);
    }
}

/* An avatar without a working pose keeps THINKING's accent. */
__attribute__((weak)) uint32_t muse_pixel_working_accent(void)
{
    return 0;
}

#if CONFIG_MUSE_SMALL_FULL_REPLY
/* While Muse thinks or answers, the caption takes the screen and Muse steps
 * aside; otherwise it's the usual two-line band over the bottom of the face. */
static void set_full_reply(bool on)
{
    if (on == s_full_reply) {
        return;
    }
    s_full_reply = on;
    lv_obj_set_flag(s_canvas, LV_OBJ_FLAG_HIDDEN, on);
    if (on) {
        lv_obj_set_size(s_caption_lbl, s_w, s_full_reply_h);
        lv_obj_set_style_pad_hor(s_caption_lbl, 3, 0);
        lv_obj_set_style_bg_opa(s_caption_lbl, LV_OPA_COVER, 0);
        lv_label_set_long_mode(s_caption_lbl, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_align(s_caption_lbl, LV_ALIGN_TOP_MID, 0, FULL_REPLY_TOP);
        set_full_font();
    } else {
        lv_obj_set_size(s_caption_lbl, SMALL_BAND_W, caption_band_h());
        lv_obj_set_style_bg_opa(s_caption_lbl, LV_OPA_70, 0);
        lv_label_set_long_mode(s_caption_lbl, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(s_caption_lbl, LV_ALIGN_BOTTOM_MID, 0, SMALL_BAND_Y);
#if CONFIG_MUSE_CJK_FONT || CONFIG_MUSE_LATIN_FONT
        set_caption_font(lv_label_get_text(s_caption_lbl));   /* the band's font, padding, height and long mode */
#else
        lv_obj_set_style_text_font(s_caption_lbl, &lv_font_unscii_8, 0);
        lv_obj_set_style_pad_hor(s_caption_lbl, 0, 0);
#endif
    }
}
#endif

static void update_status(muse_mode_t mode, float now)
{
    uint32_t accent = muse_pixel_accent(mode);
    const char *name = mode == MUSE_MODE_IDLE ? s_idle_name : MODE_NAMES[mode];
    if (mode == MUSE_MODE_THINKING && muse_voice_call_view() == MUSE_VOICE_VIEW_WORKING) {
        name = "WORKING";
        uint32_t working = muse_pixel_working_accent();
        if (working) accent = working;   /* the ring and label match the working avatar */
    }
#if CONFIG_MUSE_BOARD_M5STACK_STOPWATCH && CONFIG_MUSE_REALTIME_VOICE
    const char *call_name = muse_voice_call_label();
    const char *hint = muse_voice_call_hint();
    if (call_name) name = call_name;
    if (s_talk_hint_lbl) {
        /* Hidden over the settings rows, as the rim icons are, and once the
         * call is ending (an empty hint). */
        bool show = hint && hint[0] && !(s_tv && lv_tileview_get_tile_active(s_tv) == s_settings);
        lv_obj_set_flag(s_talk_hint_lbl, LV_OBJ_FLAG_HIDDEN, !show);
        lv_obj_set_flag(s_end_hint_lbl, LV_OBJ_FLAG_HIDDEN, !show);
        if (show && strcmp(lv_label_get_text(s_talk_hint_lbl), hint)) {
            lv_label_set_text(s_talk_hint_lbl, hint);
        }
    }
#endif

    if (name != s_shown_name) {
        const char *shown = muse_lang_status(name);
        lv_obj_set_style_text_font(s_state_lbl, muse_lang_label_font(shown, s_small ? &lv_font_unscii_8 : &lv_font_unscii_16), 0);
        lv_label_set_text(s_state_lbl, shown);
        s_shown_name = name;
    }
    if ((int)mode != s_shown_state || accent != s_shown_status_accent) {
        lv_obj_set_style_text_color(s_state_lbl, lv_color_hex(accent), 0);
        if (s_ring) {
            lv_obj_set_style_arc_color(s_ring, lv_color_hex(accent), LV_PART_INDICATOR);
        } else {
            lv_obj_set_style_bg_color(s_bar, lv_color_hex(accent), 0);
        }
        set_mic_color(mode == MUSE_MODE_LISTENING ? accent : COLOR_DIM);   /* legacy face icon */
        s_shown_state = (int)mode;
        s_shown_status_accent = accent;
        s_shown_lit = -1;
    }

    /* Ring: phase progress while listening/speaking, a spinner while thinking. */
    int ring = 0;
    if (mode == MUSE_MODE_LISTENING || mode == MUSE_MODE_SPEAKING) {
        ring = (int)(muse_state_progress() * RING_RANGE);
    } else if (mode == MUSE_MODE_THINKING) {
        ring = RING_RANGE / 6;
    }
    if (s_ring && mode == MUSE_MODE_THINKING) {
        /* Move the indicator rather than rotate the arc: a rotation redraws
         * the whole ring, and with it the whole screen. */
        int start = (int)(now * 300.0f) % 360;
        lv_arc_set_angles(s_ring, start, start + ring * 360 / RING_RANGE);
        s_ring_value = -1;
    } else if (s_ring) {
        if (s_ring_value < 0) {
            lv_arc_set_bg_start_angle(s_ring, 0);   /* unchanged, but puts the value's angles back */
        }
        if (ring != s_ring_value) {
            lv_arc_set_value(s_ring, ring);
            s_ring_value = ring;
        }
    } else {
        /* Bar along the bottom edge; a sliding segment while thinking. */
        int x = mode == MUSE_MODE_THINKING ? (int)(now * s_w) % s_w : 0;
        int w = ring * s_w / RING_RANGE;
        if (ring != s_ring_value || x) {
            lv_obj_set_width(s_bar, w);
            lv_obj_align(s_bar, LV_ALIGN_BOTTOM_LEFT, x, 0);
            s_ring_value = ring;
        }
    }

    /* The mic's meter; a reply's page has the room while answering. */
    bool meter = mode == MUSE_MODE_LISTENING;
    set_meter_visible(meter);
    if (meter && !s_small) {
        int lit = (int)lroundf(s_level * METER_SEGS);
        if (lit != s_shown_lit || accent != s_shown_accent) {
            invalidate_meter();
            for (int i = 0; i < METER_SEGS; i++) {
                /* Outer segments light last, like a centred VU. */
                int rank = abs(2 * i - (METER_SEGS - 1)) / 2;
                bool on = rank < (lit + 1) / 2;
                lv_obj_set_style_bg_color(s_meter[i], lv_color_hex(on ? accent : COLOR_METER_OFF), 0);
            }
            s_shown_lit = lit;
            s_shown_accent = accent;
        }
    }

    static char caption[MUSE_CAPTION_MAX];
    bool fresh = muse_state_caption(caption, sizeof(caption), &s_caption_version);
    const char *shown = caption;
    int answer = -1;
#if CONFIG_MUSE_BOARD_SENSECAP_WATCHER
    /* The review page has its own buffer, so the live caption is still there to
     * go back to when review ends, including any that arrived meanwhile. */
    static char review[MUSE_CAPTION_MAX];
    static muse_review_view_t view;
    bool reviewing = muse_review_active();
#endif
    if (s_reply_lbl) {
        /* The speaker picks the layout, even mid-reply: the voice task pages to fit. */
        int layout = muse_board->audio_init && muse_settings_speaker_on() ? ANSWER_HEARD : ANSWER_READ;
#if CONFIG_MUSE_BOARD_SENSECAP_WATCHER
        if (reviewing) layout = ANSWER_READ;
#endif
        if (layout != s_page_for || s_page_restore) {
            muse_state_set_page(s_answers[layout].cols, s_answers[layout].lines);
            s_page_for = layout;
            s_page_restore = false;
#if CONFIG_MUSE_BOARD_SENSECAP_WATCHER
            view.stale = true;   /* a page wrapped for the old layout is wrapped again */
#endif
        }
        if (mode == MUSE_MODE_THINKING || mode == MUSE_MODE_SPEAKING) {
            answer = layout;
        }
#if CONFIG_MUSE_BOARD_SENSECAP_WATCHER
        bool review_changed;
        reviewing = muse_review_frame(&view, review, sizeof(review), &review_changed);
        if (reviewing) {
            answer = ANSWER_READ;
            shown = review;
        }
        if (review_changed) {
            fresh = true;
            if (reviewing) {
                char position[24];
                snprintf(position, sizeof(position), "%d / %d", view.page, view.pages);
                lv_label_set_text(s_review_position, position);
            }
            lv_obj_set_flag(s_review_position, LV_OBJ_FLAG_HIDDEN, !reviewing);
        }
#endif
    }
    if (answer != s_answer) {
        set_answer(answer);
        fresh = true;   /* the caption moves between labels */
    }
    if (fresh) {
        lv_obj_t *lbl = answer >= 0 ? s_reply_lbl : s_caption_lbl;
#if CONFIG_MUSE_CJK_FONT || CONFIG_MUSE_LATIN_FONT
        if (s_long) {
            lv_obj_set_style_text_font(s_caption_lbl, long_caption_font(shown), 0);
        } else if (s_small) {
            set_caption_font(shown);
        }
        if (!s_small || lbl != s_caption_lbl) {
            lv_obj_set_style_text_font(lbl, caption_font_for(shown), 0);
        }
#endif
        lv_label_set_text(lbl, shown);
#if CONFIG_MUSE_BOARD_M5STACK_STOPWATCH
        {
            /* While a reply is up — already while it is being read out,
             * and after the turn as the kept caption — it gets most of
             * the face, and Muse shrinks to the bottom instead of
             * vanishing. Hints keep the band. The kept reply clears
             * itself a few seconds after the turn ends (below) and a
             * tap still dismisses it at once; the history tile keeps
             * every reply either way. */
            bool full = shown[0] && !muse_state_hint_pending()
                        && ((lbl == s_caption_lbl && mode == MUSE_MODE_IDLE)
                            || (lbl == s_reply_lbl && answer >= 0));
            if (s_full_lbl && (s_full_lbl != lbl || !full)) {
                if (s_full_lbl == s_caption_lbl) {
                    lv_obj_set_size(s_caption_lbl, CAPTION_W, s_cap_h);
                    lv_obj_align(s_caption_lbl, LV_ALIGN_CENTER, 0, s_cap_y);
                    lv_label_set_long_mode(s_caption_lbl, LV_LABEL_LONG_MODE_DOTS);
                }
                if (s_full_lbl == s_reply_lbl && s_answer >= 0) {
                    /* dev.18: give the answer box its layout alignment back
                     * (the full display centres the reply, below). */
                    lv_obj_set_style_text_align(s_reply_lbl, s_answers[s_answer].align, 0);
                }
                lv_obj_remove_flag(s_state_lbl, LV_OBJ_FLAG_HIDDEN);
                lv_obj_remove_flag(s_name_lbl, LV_OBJ_FLAG_HIDDEN);
                if (answer < 0) {
                    move_muse(face_px(), s_big_y);
                }
                s_full_lbl = NULL;
                s_full_since = 0;
            }
            if (full && !s_full_lbl) {
                s_full_lbl = lbl;
                lv_obj_set_size(lbl, s_w - 32, s_h - 200);
                lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 56);
                lv_label_set_long_mode(lbl, LV_LABEL_LONG_MODE_WRAP);
                /* dev.18: the read layout's pages are left-aligned for its
                 * narrow centred box (208 px at x=129..337, inside the
                 * circle at every row). Up here the label spans x=16..450,
                 * but the round screen's visible chord at the top rows is
                 * only ~300 px wide (x≈81..385 at y=56), so a left-aligned
                 * line starts off the visible screen and its first
                 * characters are lost. Centred, a paged line — never wider
                 * than its layout's box, 288 px at most — stays inside the
                 * chord at every row this label covers. */
                if (lbl == s_reply_lbl) {
                    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
                }
                lv_obj_add_flag(s_state_lbl, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(s_name_lbl, LV_OBJ_FLAG_HIDDEN);
                move_muse(face_px() * 2 / 5, 168);
                if (lbl == s_caption_lbl) {
                    s_full_since = now;
                }
            }
        }
#endif
#if CONFIG_MUSE_SMALL_FULL_REPLY
        if (s_full_reply) {
            set_full_font();
        }
#endif
        if (s_long && s_muse_up != (shown[0] != '\0')) {
            s_muse_up = shown[0] != '\0';
            move_muse(s_canvas_px, s_muse_up ? LONG_MUSE_TOP + s_canvas_px / 2 - s_h / 2 : s_big_y);
        }
        lv_obj_set_flag(lbl, LV_OBJ_FLAG_HIDDEN, !shown[0]);
        if (s_reply_lbl) {
            lv_obj_add_flag(answer >= 0 ? s_caption_lbl : s_reply_lbl, LV_OBJ_FLAG_HIDDEN);
        }
#if CONFIG_MUSE_SMALL_FULL_REPLY
        s_caption_shown = shown[0] != '\0';
#endif
    }
#if CONFIG_MUSE_BOARD_M5STACK_STOPWATCH
    /* While a question waits, its buttons are up over the kept reply and
     * the reply stays: the auto-clear below must not take the question
     * away. Once it is answered (or times out), the usual few seconds run
     * from then. */
    {
        bool ask = muse_state_ask_pending() && mode == MUSE_MODE_IDLE;
        if (ask != s_ask_shown) {
            s_ask_shown = ask;
            lv_obj_set_flag(s_ask_ok, LV_OBJ_FLAG_HIDDEN, !ask);
            lv_obj_set_flag(s_ask_no, LV_OBJ_FLAG_HIDDEN, !ask);
            if (!ask && s_full_lbl == s_caption_lbl) {
                s_full_since = now;
            }
        }
        if (muse_state_ask_pending()) {
            s_full_since = 0;
        }
    }
    /* The kept reply has been read by now: clear it and give Muse the
     * face back. It stays in the history tile. */
    if (s_full_lbl == s_caption_lbl && s_full_since > 0 && mode == MUSE_MODE_IDLE
        && now - s_full_since > 4.0f) {
        s_full_since = 0;
        muse_state_set_caption("%s", "");
    }
#endif
#if CONFIG_MUSE_SMALL_FULL_REPLY
    if (s_small && !s_long) {
        apply_text_size();
        set_full_reply(s_caption_shown && (mode == MUSE_MODE_THINKING || mode == MUSE_MODE_SPEAKING));
    }
#endif
    update_power(now);
}

static volatile bool s_snapshot;
#if LV_USE_SNAPSHOT
static volatile int s_bench_page = -1;
void muse_ui_bench_page(bool settings)
{
    s_bench_page = settings ? 1 : 0;   /* applied in the LVGL task */
}
#endif

/* Streams the screen over the USB cable as base64 RGB565 (bench testing; needs
 * LV_USE_SNAPSHOT, which devices/sdkconfig.muse-bench turns on). */
static void send_snapshot(void)
{
#if LV_USE_SNAPSHOT
    lv_draw_buf_t *buf = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
    if (!buf) {
        return;
    }
    enum { RAW = 144 };   /* lines must fit the driver's 256-byte TX ring */
    char hdr[48];
    int n = snprintf(hdr, sizeof(hdr), "\nSNAP BEGIN %d %d %d\n", (int)buf->header.w, (int)buf->header.h, RAW);
    if (!muse_console_write(hdr, n)) {
        goto done;
    }
    static unsigned char b64[4 * RAW / 3 + 4];
    for (uint32_t y = 0; y < buf->header.h; y++) {
        const unsigned char *p = buf->data + y * buf->header.stride;
        size_t left = buf->header.w * 2;
        while (left) {
            size_t chunk = left > RAW ? RAW : left, olen;
            mbedtls_base64_encode(b64, sizeof(b64), &olen, p, chunk);
            b64[olen++] = '\n';
            if (!muse_console_write(b64, olen)) {
                goto done;
            }
            p += chunk;
            left -= chunk;
        }
    }
    muse_console_write("SNAP END\n", 9);
done:
    lv_draw_buf_destroy(buf);
#else
    muse_console_write("\nSNAP OFF\n", 10);   /* so snap.py can say why */
#endif
}

void muse_ui_request_snapshot(void)
{
    s_snapshot = true;
}

static void frame_tick(lv_timer_t *timer)
{
#if LV_USE_SNAPSHOT
    if (s_bench_page >= 0) {
        bool settings = s_bench_page != 0;
        s_bench_page = -1;
        if (s_tv) lv_tileview_set_tile(s_tv, settings ? s_settings : s_face, LV_ANIM_OFF);
    }
#endif
    update_control_hints(muse_state_mode(NULL));
    if (s_snapshot) {
        s_snapshot = false;
        send_snapshot();
    }
    (void)timer;
    image_sync();
    float mode_t;
    muse_mode_t mode = muse_state_mode(&mode_t);
    float now = (float)esp_timer_get_time() / 1e6f;

    if (mode != s_last_mode) {
        if (muse_voice_in_call()) s_level = 0;   /* do not smooth speaker energy into the mic meter */
        if (mode == MUSE_MODE_LISTENING) {
            image_hide_locked();
            muse_ui_show_face();
        }
#if CONFIG_MUSE_VOICE_NOTE_TOGGLE
        s_note_listening = mode == MUSE_MODE_LISTENING;
        update_tileview_swipe();
#endif
        s_last_mode = mode;
    }
    if (update_sleep()) {
        return;
    }
#if CONFIG_MUSE_VIDEO
    if (muse_video_tick()) {
        muse_menu_close();
        return;   /* video covers everything */
    }
#endif
    update_chrome(now);
    if (muse_menu_tick(now)) {
        image_hide_locked();
        return;   /* the menu covers the face */
    }
    if (s_image_dsc.data) {
        return;   /* the image covers the face */
    }
#if CONFIG_MUSE_BOARD_M5STACK_STOPWATCH
    /* The tile updates run before the off-face early return below, not at
     * the bottom of the tick: the history and quick tiles share the face's
     * column, so they passed that check all along, but the dashboard sits
     * in its own column — at the bottom it never saw a tick while it was
     * on screen, so it could neither fetch on open nor repaint, and its
     * state line froze on "更新中…" (dev.7 bug). Each update gates on its
     * own tile being active, as update_chrome() above already runs for
     * every tile. */
    update_history();
    update_quick();
    update_dash();
    update_face_mode();   /* dev.14: yellow-button presses, the clock face */
    update_care(now);     /* dev.14: care meters, food, accessories, growth */
    update_timer(now);    /* dev.16: timer opens, the fired alert, the clock row */
    update_pomodoro(now);   /* dev.22: pomodoro opens, the fired alert, the shared clock row */
#endif
    if (s_tv && lv_obj_get_scroll_x(s_tv) != face_scroll_x()) {
        /* Off screen, or sliding to or from settings: hold still so the
         * slide gets the whole frame time. */
        return;
    }

    /* Fast attack, slow release keeps the mouth and meter lively but readable. */
    float level = muse_state_level();
    /* The voice task can switch sources after this frame read its mode. */
    if (muse_voice_in_call() &&
        ((mode == MUSE_MODE_LISTENING) != (muse_voice_call_view() == MUSE_VOICE_VIEW_LISTENING))) level = 0;
    s_level += (level - s_level) * (level > s_level ? 0.6f : 0.2f);

    muse_pose_t pose = {
        .mode = mode,
        .t = now,
        .mode_t = mode_t,
        .level = s_level,
        .happy = muse_state_happiness(),
        .working = muse_voice_call_view() == MUSE_VOICE_VIEW_WORKING ? 1.0f : 0,
    };
#if CONFIG_MUSE_BOARD_M5STACK_STOPWATCH
    /* dev.14 care, in the pose's own terms: a content avatar (mood 60+)
     * breaks into its happy face for a moment every few seconds while
     * idle; a neglected one (mood under 30) dozes instead — the OFF
     * pose's closed eyes at part brightness — a few seconds at a time.
     * Neither while the care panel or the clock owns the face. */
    if (mode == MUSE_MODE_IDLE && !s_care_open && !s_clock_shown) {
        if (s_care.mood >= 60) {
            if (fmodf(now, 7.0f) < 1.4f && pose.happy < 0.7f) {
                pose.happy = 0.7f;
            }
        } else if (s_care.mood < 30 && fmodf(now, 11.0f) < 3.0f) {
            pose.mode = MUSE_MODE_OFF;
            pose.mode_t = 0.7f;
        }
    }
    /* dev.22: while the pomodoro panel is up, みゃおっち follows the
     * phase instead — calm (no happy mixed in) through a focus, fully
     * happy on a break. Expression only: the care meters above are
     * neither consulted nor changed for it. */
    if (mode == MUSE_MODE_IDLE && s_pomo_open && s_pomo.phase != POMO_IDLE) {
        pose.happy = s_pomo.phase == POMO_FOCUS ? 0.0f : 1.0f;
    }
#endif
    bool render = !muse_board->epaper || mode != s_last_epaper_pose;
#if CONFIG_MUSE_BOARD_M5STACK_STOPWATCH
    /* Battery: the avatar doesn't need redrawing 25 times a second —
     * a quarter of that (about 6 fps) reads fine and costs far less,
     * so the pose renders at a quarter rate all the time. A quiet idle
     * face — nothing said, nothing to read — drops further after
     * ~10 s, to a sixteenth (nearly still); anything new wakes it up. */
    static unsigned s_quiet_ticks;
    static unsigned s_frame_ticks;
    bool quiet = mode == MUSE_MODE_IDLE && s_face_active
                 && lv_obj_has_flag(s_caption_lbl, LV_OBJ_FLAG_HIDDEN)
                 && (!s_reply_lbl || lv_obj_has_flag(s_reply_lbl, LV_OBJ_FLAG_HIDDEN));
    if (quiet) {
        s_quiet_ticks++;
    } else {
        s_quiet_ticks = 0;
    }
    if (++s_frame_ticks % (s_quiet_ticks > 250 ? 16 : 4) != 0) render = false;
#endif
    if (render) {
        muse_pixel_render(&pose);
        invalidate_muse();
        s_last_epaper_pose = mode;
    }

    update_status(mode, now);
}

esp_err_t muse_ui_start(void)
{
    s_w = muse_board->width;
    s_h = muse_board->height;
    /* The full layout assumes room for the 466 px board's header and bottom
     * captions. Short landscape panels (BOX-3, and 480x320, where the full
     * layout's status line would sit above the screen) need the compact
     * layout too, as does anything narrower than its fixed 256 px captions. */
    bool short_landscape = s_w > s_h && s_h <= 320;
    s_small = s_h < 200 || s_w < 200 || short_landscape || s_w < 300;
    s_tall = s_small && s_h >= s_w + 64;
    s_long = s_tall && s_h >= 3 * s_w;
    /* Small screens keep room for the status line and button icons. A narrow
     * one is as wide as Muse gets, in whole pixels. */
    s_canvas_px = s_small ? s_h * 3 / 4 : MUSE_PX_W * 5;
    if (short_landscape) {
        /* Leave the header's first 40 rows and bottom captions clear. */
        s_canvas_px = s_h * 2 / 3;
    }
    if (muse_board->avatar_px > 0) {
        s_canvas_px = muse_board->avatar_px;
    }
    if (s_canvas_px > s_w) {
        s_canvas_px = s_w / MUSE_PX_W * MUSE_PX_W;
    }
    s_dy = (s_h - 466) / 2;
    if (!s_small && muse_board->round && s_dy < 0) {
        /* A shorter screen moves the labels out towards its edges, but a
         * smaller circle's edges curve away from them. Keep them where they
         * are and shrink Muse to whole pixels that fit between them. */
        s_canvas_px = (MUSE_PX_W * 5 + 2 * s_dy) / MUSE_PX_W * MUSE_PX_W;
        s_dy = 0;
    }

    lv_display_t *disp = muse_board->display_start(&s_indev);
    if (!disp) {
        ESP_LOGE(TAG, "display init failed");
        return ESP_FAIL;
    }

    s_image_mutex = xSemaphoreCreateMutex();
    muse_board->display_lock(-1);
    build_screen();
#if CONFIG_MUSE_BOARD_M5STACK_STOPWATCH
    care_load();
    if (face_px() != s_canvas_px) {
        set_canvas_px(face_px());   /* dev.14: an evolved avatar starts at its size */
    }
#endif
    if (s_settings) {
        muse_settings_ui_build(s_settings);
    } else {
        muse_menu_build(lv_screen_active(), s_w, s_h);
    }
    build_overlays();
    lv_timer_create(frame_tick, muse_board->frame_ms, NULL);
    s_ready = true;
    muse_board->display_unlock();

    ESP_LOGI(TAG, "UI up: %dx%d, %d px Muse, %d ms frames", s_w, s_h, s_canvas_px, muse_board->frame_ms);
    return ESP_OK;
}

void muse_ui_show_face(void)
{
    if (!s_tv) {
        return;
    }
    s_swipe_enabled = true;
    update_tileview_swipe();   /* still locked while the screen mic is held */
    lv_tileview_set_tile(s_tv, s_face, muse_board->epaper ? LV_ANIM_OFF : LV_ANIM_ON);
}

void muse_ui_set_swipe_enabled(bool enabled)
{
    if (!s_tv) {
        return;
    }
    s_swipe_enabled = enabled;
    update_tileview_swipe();
    s_shown_page = -1;
    s_next_settings_tick = 0;
}

void muse_ui_preview_brightness(int pct)
{
    s_preview_brightness = pct;
    apply_brightness(pct);
}

bool muse_ui_image_size(int *w, int *h)
{
    if (!s_ready || !heap_caps_get_total_size(MALLOC_CAP_SPIRAM)) {
        return false;
    }
    *w = s_w;
    *h = s_h;
    return true;
}

int muse_ui_image_bits(void)
{
    return muse_board->epaper ? 2 : 16;
}

/* Floyd-Steinberg to four grays, in place: each pixel becomes black, 85, 170
 * or white, so a panel that rounds every pixel to its nearest gray (the
 * Sticky's) shows shading instead of bands. err holds 2 * (w + 2) values. */
static void dither_gray4_565(uint16_t *px, int w, int h, int16_t *err)
{
    static const uint16_t grays[4] = { 0x0000, 0x52AA, 0xAD55, 0xFFFF };
    int16_t *cur = err, *next = err + w + 2;
    memset(err, 0, 2 * (size_t)(w + 2) * sizeof(*err));
    for (int y = 0; y < h; y++) {
        memset(next, 0, (size_t)(w + 2) * sizeof(*next));
        for (int x = 0; x < w; x++) {
            uint16_t p = px[(size_t)y * w + x];
            int r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;
            int v = ((77 * (r * 255 / 31) + 150 * (g * 255 / 63) + 29 * (b * 255 / 31)) >> 8) + cur[x + 1];
            int level = (v * 3 + 127) / 255;
            level = level < 0 ? 0 : level > 3 ? 3 : level;
            int e = v - level * 85;
            cur[x + 2] += (int16_t)(e * 7 / 16);
            next[x] += (int16_t)(e * 3 / 16);
            next[x + 1] += (int16_t)(e * 5 / 16);
            next[x + 2] += (int16_t)(e / 16);
            px[(size_t)y * w + x] = grays[level];
        }
        int16_t *t = cur;
        cur = next;
        next = t;
    }
}

void muse_ui_image_done(void)
{
    if (!s_ready || muse_ui_image_bits() != 2) {
        return;   /* a colour panel shows each rectangle as it comes */
    }
    int16_t *err = malloc(2 * (size_t)(s_w + 2) * sizeof(int16_t));
    if (!err) {
        return;   /* shown undithered: banded, but whole */
    }
    xSemaphoreTake(s_image_mutex, portMAX_DELAY);
    if (s_image_buf) {
        dither_gray4_565(s_image_buf, s_w, s_h, err);
        s_image_area = (lv_area_t){ 0, 0, s_w - 1, s_h - 1 };
        s_image_dirty = true;
    }
    xSemaphoreGive(s_image_mutex);
    free(err);
}

bool muse_ui_image_draw(int x, int y, int w, int h, const uint16_t *pixels)
{
    if (!s_ready || x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > s_w || y + h > s_h) {
        return false;
    }
    xSemaphoreTake(s_image_mutex, portMAX_DELAY);
    if (!s_image_buf) {
        /* The first rectangle of an image: a black screen to draw onto. */
        s_image_buf = heap_caps_calloc((size_t)s_w * s_h, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
        if (!s_image_buf) {
            xSemaphoreGive(s_image_mutex);
            return false;
        }
        /* Now, not when it's shown: asleep, the display may be paused (muse_input.c). */
        muse_state_set_asleep(false);
    }
    const uint8_t *src = (const uint8_t *)pixels;
    for (int row = 0; row < h; row++) {
        uint16_t *dst = s_image_buf + (size_t)(y + row) * s_w + x;
        for (int i = 0; i < w; i++, src += 2) {
            dst[i] = (uint16_t)(src[0] << 8 | src[1]);
        }
    }
    lv_area_t *a = &s_image_area;
    if (!s_image_dirty) {
        *a = (lv_area_t){ x, y, x + w - 1, y + h - 1 };
        s_image_dirty = true;
    } else {
        a->x1 = LV_MIN(a->x1, x);
        a->y1 = LV_MIN(a->y1, y);
        a->x2 = LV_MAX(a->x2, x + w - 1);
        a->y2 = LV_MAX(a->y2, y + h - 1);
    }
    xSemaphoreGive(s_image_mutex);
    return true;
}

void muse_ui_image_hide(void)
{
    if (!s_ready) {
        return;
    }
    muse_board->display_lock(-1);
    image_hide_locked();
#if CONFIG_MUSE_WATCHER_CAMERA
    if (s_camera_hint) lv_obj_add_flag(s_camera_hint, LV_OBJ_FLAG_HIDDEN);
#endif
    muse_board->display_unlock();
}

#if CONFIG_MUSE_WATCHER_CAMERA
void muse_ui_camera_hint(bool visible)
{
    if (!s_ready || !s_camera_hint) return;
    muse_board->display_lock(-1);
    if (visible) lv_obj_remove_flag(s_camera_hint, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_camera_hint, LV_OBJ_FLAG_HIDDEN);
    muse_board->display_unlock();
}
#endif
