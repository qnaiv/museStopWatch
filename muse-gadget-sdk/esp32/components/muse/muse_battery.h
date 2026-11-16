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
#include <stddef.h>
#include <stdint.h>

#include "muse_state.h"

/*
 * Battery meter, for power work. None of the boards can measure current, so a
 * measurement pairs the fuel gauge (1% steps) with what the chip did: how long
 * the screen was off, how long it really spent in light sleep, and, with
 * CONFIG_PM_PROFILING, how long a core was busy and which power locks kept
 * it awake. A measurement starts when USB is unplugged (or at boot on
 * battery) and stops when USB comes back, keeping its numbers until the next.
 * On the bench, ">nap" on USB measures from then until the board wakes.
 */

typedef struct {
    bool started;         /* false until the board has been on battery */
    bool running;         /* on battery now; false once USB is back */
    int64_t secs;         /* time on battery */
    int pct_start, pct_now;   /* -1 unknown */
    int mv_start, mv_now;     /* 0 unknown */
    int screen_off_pm;    /* per mille of the time on battery */
    int resting_pm;       /* display stopped and the CPU allowed to sleep */
    int slept_pm;         /* actually in light sleep; -1 unknown */
    uint32_t sleeps;      /* light sleeps, each ended by a wake */
    int busy_pm;          /* a core running a task; -1 unknown */
    char awake[48];       /* the locks that kept it awake longest: "wifi 4%, bt 1%" */
} muse_battery_t;

void muse_battery_init(void);

/* From the input task: every battery reading, with muse_state_on_battery(),
 * which ">nap" sets on USB so the bench can measure; and the screen and CPU
 * state. */
void muse_battery_note_power(const muse_power_t *p, bool on_battery);
void muse_battery_note_state(bool screen_off, bool resting);

/* Whether the CPU powers down in light sleep, for the JSON; may come before init. */
void muse_battery_note_cpu_pd(bool on);

/* Starts over: from now if on battery, else at the next unplug. */
void muse_battery_reset(void);

void muse_battery_read(muse_battery_t *out);

/* Once the gauge has moved, after at least 10 minutes: the drain in tenths of a
 * percent an hour, and how many hours a full charge lasts at that rate. */
bool muse_battery_drain(const muse_battery_t *b, int *rate10, int *full_h);

/* The same with every lock and CPU mode, as one line of JSON, and internal RAM
 * ("heap": free, largest block and lowest since boot now, the lowest seen
 * resting on battery in this run, what's left of the retention-capable RAM,
 * free and largest block, and "cpu_pd"). With cpu_pd the CPU's retention
 * memory was taken at boot and kept, so it's out of all of them, and
 * "resting_min" is the headroom while resting rather than a dip for it.
 * Too long for cap, it loses whole pieces from the end, the lock list first,
 * and stays valid JSON. Returns its length, always under cap. */
int muse_battery_json(char *buf, size_t cap);

/* The JSON last saved before this boot, kept in RTC memory across resets (not
 * power loss) until muse_battery_reset(); NULL if none. It survives a board
 * that resets when its port opens, or one that crashed on battery. It's
 * muse_battery_json() without "retention_free" and "retention_largest". */
const char *muse_battery_saved_json(void);

/*
 * The history (dev.17): one point every 30 minutes at most, sooner when the
 * charge state flips or the gauge moves 5 or more, never more often than
 * every 5 minutes, taken from the readings muse_battery_note_power() already
 * gets, so keeping it wakes nothing. 336 points at 30-minute spacing cover
 * 7 days. The ring lives in PSRAM and is mirrored whole into the "hist" blob
 * of the same NVS namespace on every point, so it survives reboots and a
 * battery run flat. The gauge is a voltage estimate (see the board's
 * read_power), so single points wobble with load; read slopes, not points.
 */
#define MUSE_BATT_HIST_MAX 336
#define MUSE_BATT_HIST_FLAG_CHARGING 0x01
#define MUSE_BATT_HIST_FLAG_USB 0x02
#define MUSE_BATT_HIST_FLAG_CLOCK 0x04   /* epoch is real (SNTP/RTC), not 1970 */

typedef struct {
    uint32_t epoch;   /* time(NULL) at the reading */
    int8_t pct;       /* 0-100 */
    uint8_t flags;    /* MUSE_BATT_HIST_FLAG_* */
    uint16_t scr_min; /* minutes the screen was on since the previous point */
} muse_batt_point_t;

/* Points held, oldest first: muse_battery_hist_get(0) is the oldest. */
int muse_battery_hist_count(void);
bool muse_battery_hist_get(int i, muse_batt_point_t *out);

/* The net drain over the last span_s of points: the sum of the drops between
 * neighbouring points, rises (charging, or the gauge recovering) left out.
 * -1 when there are fewer than two points in the span. */
int muse_battery_hist_consumption(int64_t span_s);

/* Points recorded since the last muse_battery_hist_mark_sent(): what an
 * upload still owes. A boot counts everything it loaded as unsent. */
int muse_battery_hist_unsent(void);
void muse_battery_hist_mark_sent(void);

/* "\"points\":[[epoch,pct,charging,scr_min],...],\"run\":{...}" — the points
 * oldest first (points whose clock was not real carry their raw epoch; the
 * uploader drops them by the 2025 floor), then the current run's
 * muse_battery_json(). Same cut-short rules as muse_battery_json(): whole
 * pieces only, valid JSON however small cap is. Returns its length. */
int muse_battery_hist_json(char *buf, size_t cap);
