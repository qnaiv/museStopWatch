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

#include "muse_review.h"

#include <string.h>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "muse_chat_priv.h"
#include "muse_text.h"
#if CONFIG_MUSE_BOARD_M5STACK_STOPWATCH
#include "esp_timer.h"
#include "muse_settings.h"
/* The board's vibration motor (boards/board_m5stack_stopwatch.c). */
void msw_board_vibrate(int pct) __attribute__((weak));

static void vibrate_off(void *arg)
{
    (void)arg;
    if (msw_board_vibrate) msw_board_vibrate(0);
}

/* One short pulse of the motor. A second call while it runs just extends
 * it (the off timer restarts), so overlapping calls stay one pulse. */
static void pulse_motor(void)
{
    if (!msw_board_vibrate) return;
    static esp_timer_handle_t off;
    if (!off) {
        const esp_timer_create_args_t args = { .callback = vibrate_off, .name = "vib_off" };
        if (esp_timer_create(&args, &off) != ESP_OK) return;
    }
    esp_timer_stop(off);
    msw_board_vibrate(80);
    esp_timer_start_once(off, 150 * 1000);
}

/* Vibrate-only sound mode: a new reply announces itself with one short
 * pulse of the motor instead of the speaker. */
static void pulse_for_reply(void)
{
    if (muse_settings_sound_mode() != MUSE_SOUND_VIBRATE) return;
    pulse_motor();
}
#endif

/* A proactive message (nobody asked for it) is never spoken; in the sound
 * modes with sound or vibration it announces itself with one pulse. */
void muse_review_pulse(void)
{
#if CONFIG_MUSE_BOARD_M5STACK_STOPWATCH
    int mode = muse_settings_sound_mode();
    if (mode == MUSE_SOUND_ON || mode == MUSE_SOUND_VIBRATE) {
        pulse_motor();
    }
#endif
}

#define REVIEW_CAP (32 * 1024)
/* The last replies, newest first. Each buffer is lazily allocated in PSRAM,
 * so the ring costs at most REVIEW_ENTRIES * REVIEW_CAP of PSRAM (256 KB of
 * the 8 MB) and under a hundred bytes of internal RAM. RAM only: a reboot
 * forgets the history. */
#define REVIEW_ENTRIES 8

static SemaphoreHandle_t s_lock;
static char *s_slots[REVIEW_ENTRIES];
static uint32_t s_slot_turn[REVIEW_ENTRIES];
static int s_head = -1;   /* the newest entry's slot, -1 while there are none */
static int s_count;       /* entries held, up to REVIEW_ENTRIES */
static int s_sel = -1;    /* the entry being read */
static uint32_t s_turn, s_next_turn, s_version;
static bool s_have_turn, s_active;
static int s_page, s_pages;   /* of the selected entry; s_pages: 0 until paged */

/* The selected entry's text, or NULL. The lock is held. */
static const char *sel_text(void)
{
    return s_sel >= 0 ? s_slots[s_sel] : NULL;
}

/* The selected entry's position back from the newest (0 = newest). Lock held. */
static int sel_age(void)
{
    return (s_head - s_sel + REVIEW_ENTRIES) % REVIEW_ENTRIES;
}

esp_err_t muse_review_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    return s_lock ? ESP_OK : ESP_ERR_NO_MEM;
}

uint32_t muse_review_turn(void)
{
    if (!s_lock) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t turn = ++s_next_turn;
    xSemaphoreGive(s_lock);
    return turn;
}

void muse_review_store(uint32_t turn, const char *text)
{
    if (!s_lock || !text || !text[0]) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool fresh = !s_have_turn || turn != s_turn;
    int slot = -1;
    if (fresh) {
        slot = (s_head + 1) % REVIEW_ENTRIES;
        if (!s_slots[slot]) {
            s_slots[slot] = heap_caps_malloc(REVIEW_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }
        if (s_slots[slot]) {
            s_head = slot;
            s_slots[slot][0] = '\0';
            s_slot_turn[slot] = turn;
            if (s_count < REVIEW_ENTRIES) s_count++;
        } else if (s_head >= 0) {
            slot = s_head;   /* no PSRAM for a new entry: overwrite the newest */
            s_slots[slot][0] = '\0';
            s_slot_turn[slot] = turn;
        } else {
            xSemaphoreGive(s_lock);
            return;   /* nowhere to keep it, as before the ring */
        }
        s_turn = turn;
        s_have_turn = true;
        /* A new reply ends any reading in progress; the selection follows it. */
        s_active = false;
        s_sel = slot;
        s_page = 0;
        s_pages = 0;
    } else {
        for (int i = 0; i < REVIEW_ENTRIES; i++) {
            if (s_slots[i] && s_slot_turn[i] == turn) {
                slot = i;
                break;
            }
        }
    }
    char *buf = slot >= 0 ? s_slots[slot] : NULL;
    if (buf) {
        s_version++;
        if (!fresh && buf[0]) {
            strlcat(buf, "\n\n", REVIEW_CAP);
        }
        strlcat(buf, text, REVIEW_CAP);
        if (slot == s_sel) s_pages = 0;   /* steps before the next paging aren't clamped to the old count */
        /* Truncation must not leave half a UTF-8 character at the end. */
        size_t len = strlen(buf);
        if (len) {
            size_t at = len - 1;
            while (at && ((unsigned char)buf[at] & 0xc0) == 0x80) at--;
            unsigned char lead = (unsigned char)buf[at];
            size_t need = lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
            if (len - at < need) buf[at] = '\0';
        }
    }
    xSemaphoreGive(s_lock);
#if CONFIG_MUSE_BOARD_M5STACK_STOPWATCH
    if (fresh) pulse_for_reply();
#endif
}

bool muse_review_step(int direction)
{
    if (!s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const char *text = sel_text();
    bool have = text && text[0];
    if (have) {
        s_version++;
        if (!s_active) {
            s_active = true;
            s_sel = s_head;
            s_page = 0;
            s_pages = 0;
        } else {
            s_page += direction;
            if (s_page < 0) s_page = 0;
            if (s_pages && s_page >= s_pages) s_page = s_pages - 1;
        }
    }
    xSemaphoreGive(s_lock);
    return have;
}

bool muse_review_entry_step(int direction)
{
    if (!s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const char *text = sel_text();
    bool have = s_count > 0 && text && text[0];
    if (have) {
        s_version++;
        if (!s_active) {
            s_active = true;
            s_sel = s_head;
        } else {
            int age = sel_age() + direction;
            if (age < 0) age = 0;
            if (age > s_count - 1) age = s_count - 1;
            s_sel = (s_head - age + REVIEW_ENTRIES) % REVIEW_ENTRIES;
        }
        s_page = 0;
        s_pages = 0;
    }
    xSemaphoreGive(s_lock);
    return have;
}

void muse_review_open(void)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_count > 0) {
        s_active = true;
        s_sel = s_head;
        s_page = 0;
        s_pages = 0;
        s_version++;
    }
    xSemaphoreGive(s_lock);
}

bool muse_review_info(int *at, int *count)
{
    if (!s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool have = s_count > 0;
    if (have) {
        if (at) *at = sel_age() + 1;
        if (count) *count = s_count;
    }
    xSemaphoreGive(s_lock);
    return have;
}

void muse_review_close(void)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_active) s_version++;
    s_active = false;
    xSemaphoreGive(s_lock);
}

bool muse_review_active(void)
{
    if (!s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool active = s_active;
    xSemaphoreGive(s_lock);
    return active;
}

bool muse_review_page(char *out, size_t cap, int *page, int *pages)
{
    if (!s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const char *text = sel_text();
    bool active = s_active && text && text[0];
    if (active) {
        s_pages = muse_chat_caption_page(text, s_page, out, cap);
        if (s_pages) muse_text_to_caption(out, cap);
        if (!s_pages) {
            s_active = active = false;
            s_version++;
        } else {
            if (s_page >= s_pages) s_page = s_pages - 1;
            *page = s_page + 1;
            *pages = s_pages;
        }
    }
    xSemaphoreGive(s_lock);
    return active;
}

bool muse_review_frame(muse_review_view_t *view, char *out, size_t cap, bool *changed)
{
    *changed = false;
    if (!s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const char *text = sel_text();
    bool active = s_active && text && text[0];
    uint32_t version = s_version;
    xSemaphoreGive(s_lock);
    if (active && (!view->shown || view->stale || view->version != version)) {
        /* Another store or step in between moves the version again: the next frame catches it. */
        view->version = version;
        view->stale = false;
        active = muse_review_page(out, cap, &view->page, &view->pages);
        *changed = active;
    }
    if (active != view->shown) {
        view->shown = active;
        *changed = true;
    }
    return active;
}
