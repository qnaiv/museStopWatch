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

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "sdkconfig.h"

/*
 * Bring up the display and build the UI: the avatar on the first tile,
 * settings one swipe to the left. Also owns screen sleep and brightness.
 */
esp_err_t muse_ui_start(void);

/* From any task: the screen has gone dark for sleep (and not yet woken). */
bool muse_ui_dark(void);

/* From any task: the face tile is the currently visible tile. */
bool muse_ui_face_active(void);

/* The functions below run in the LVGL task (or with the display lock held). */

/* Slide back to the face (e.g. when a talk starts). */
void muse_ui_show_face(void);
#if CONFIG_MUSE_BOARD_M5STACK_STOPWATCH
/* museStopWatch dev.14: the yellow button switches the face between the
 * avatar and the clock. From any task: the press is counted here and
 * applied in the LVGL task; returns the mode switched to (true: clock). */
bool muse_ui_toggle_face_mode(void);
/* From any task: the face currently shows the clock. */
bool muse_ui_clock_mode(void);
/* museStopWatch dev.16: open the countdown timer (the quick tile's
 * button, the launcher's built-in cell, the yellow button's long press).
 * From any task: the request is counted here and applied in the LVGL
 * task (the face tile comes forward and the timer panel opens).
 * dev.23: the panel is the unified timer/pomodoro panel, and this
 * door reopens it in the mode last used (kept in NVS). */
void muse_ui_request_timer(void);
/* museStopWatch dev.16: the yellow button's long-press assignment
 * (Settings > Yellow button), kept in NVS across reboots. A short press
 * always switches the face (dev.14); only the hold is assignable. */
enum { MUSE_YEL_LONG_TIMER = 0, MUSE_YEL_LONG_APP = 1, MUSE_YEL_LONG_FACE = 2, MUSE_YEL_LONG_NONE = 3,
       MUSE_YEL_LONG_POMODORO = 4 };   /* dev.22: appended, so stored 0..3 keep their meaning.
                                        * dev.23: no longer offered in Settings (the timer
                                        * door covers it), but a stored 4 stays valid: it
                                        * opens the unified panel in pomodoro mode. */
int muse_ui_yellow_long_action(void);
const char *muse_ui_yellow_long_app(void);   /* the app id for MUSE_YEL_LONG_APP */
void muse_ui_set_yellow_long(int action, const char *app_id);
/* museStopWatch dev.22: open the pomodoro panel. From any task,
 * like muse_ui_request_timer(). dev.23: the panel is the unified
 * timer/pomodoro panel; this door lands on its pomodoro tab (and
 * makes pomodoro the mode the unified doors reopen in). */
void muse_ui_request_pomodoro(void);
/* museStopWatch dev.22: the pomodoro lengths (Settings > Pomodoro),
 * kept in NVS across reboots. Minutes, and how many focus phases make
 * a long break. Defaults 25 / 5 / 15 / 4. */
typedef struct {
    int focus_min, break_min, long_min, sets;
} muse_pomo_cfg_t;
void muse_ui_pomodoro_config(muse_pomo_cfg_t *out);
void muse_ui_set_pomodoro_config(const muse_pomo_cfg_t *cfg);
/* museStopWatch dev.17: ask for the battery history to be uploaded to the
 * VM (workspace/battery_history.json) so Muse can analyse it. Cheap to
 * call often (the Battery page's tick does): it starts nothing while an
 * upload is recent, running, or barely owed. */
void muse_batt_hist_upload_request(void);
/* museStopWatch dev.21: the last upload attempt, for the Battery page's
 * note — dev.17's failures went only to the log, which cannot be read
 * off the device, so a refused upload looked exactly like a landed one.
 * stage is one of MUSE_BATT_HIST_UP_*; when it is UP_WRITE, piece (0-based),
 * http (0: no reply arrived), ok (1 true, 0 false, -1 absent/unread) and
 * reply (the reply's first bytes) describe the piece that failed. path is
 * the /fs/write path form the attempt used (0 "workspace/…", 1
 * "/workspace/…", 2 the bare name); at is when the attempt ended (wall
 * clock, seconds). */
enum {
    MUSE_BATT_HIST_UP_NONE = 0,      /* no attempt yet this boot */
    MUSE_BATT_HIST_UP_WAIT_LINK,     /* the session never came up */
    MUSE_BATT_HIST_UP_DASH_BUSY,     /* a dash fetch was running; skipped */
    MUSE_BATT_HIST_UP_BUILD,         /* nothing to send */
    MUSE_BATT_HIST_UP_WRITE,         /* a piece failed */
    MUSE_BATT_HIST_UP_DONE,          /* every piece landed */
};
typedef struct {
    int64_t at;
    int stage, piece, http, ok, path;
    char reply[49];
} muse_batt_hist_up_t;
void muse_batt_hist_upload_last(muse_batt_hist_up_t *out);
#endif
/* Settings sub-pages turn off the tile swipe so they can use horizontal gestures. */
void muse_ui_set_swipe_enabled(bool enabled);
/* Temporarily applies a brightness while a slider is dragged. */
void muse_ui_preview_brightness(int pct);

/*
 * display.draw_url, from any task. An image covers the face until a tap, a
 * talk, the menu or muse_ui_image_hide(). Pixels are RGB565, high byte first.
 * The size is false without PSRAM for the image or before the UI is up.
 */
bool muse_ui_image_size(int *w, int *h);
/* What the panel shows of those pixels: 16 (colour), or 2 on a four-gray e-paper. */
int muse_ui_image_bits(void);
/* The image is complete: a four-gray e-paper dithers it to its grays. */
void muse_ui_image_done(void);
bool muse_ui_image_draw(int x, int y, int w, int h, const uint16_t *pixels);
void muse_ui_image_hide(void);
/* Watcher camera mode: shows an on-screen shutter hint over the live image. */
void muse_ui_camera_hint(bool visible);

/* Bench testing, from any task: streams the screen over USB serial. */
void muse_ui_request_snapshot(void);
#if CONFIG_LV_USE_SNAPSHOT
/* Bench-only, from any task: select the touch tile without pretending a/s
 * are touch-menu navigation. No effect on non-touch boards. */
void muse_ui_bench_page(bool settings);
/* Display-only demo name for privacy-safe screenshots; no provisioning changes. */
void muse_ui_bench_demo(bool enabled);
#endif
