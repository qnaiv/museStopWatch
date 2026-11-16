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

#include "transfer_reserve.h"

#include "sdkconfig.h"

#if CONFIG_SPIRAM

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "link.reserve";

// R in the dev.20 memory rules (§3.3): at least the install gate's keep
// (29,850 B), rounded up, so freeing the block satisfies the gate by
// construction whatever the heap's history, and the freed block is
// itself one contiguous block above both operations' stack needs
// (6,144 B install, 16,384 B OTA). The DMA cap matters: a plain
// internal block could land outside the DMA heap and would not lift
// dma_free when freed.
#define TRANSFER_RESERVE_BYTES 30720
#define TRANSFER_RESERVE_CAPS  (MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
// Two-piece re-arm fallback: a stack-sized piece (the OTA stack is
// 16,384 B) plus the remainder, tolerating partial fragmentation of
// the freed region.
#define TRANSFER_RESERVE_PIECE1 (17 * 1024)
#define TRANSFER_RESERVE_PIECE2 (TRANSFER_RESERVE_BYTES - TRANSFER_RESERVE_PIECE1)

static SemaphoreHandle_t s_lock;
static void *s_block1, *s_block2;   // the held block, or its two pieces
static transfer_reserve_holder_t s_holder = TRANSFER_RESERVE_NONE;
static bool s_rearm_failed;

static void lock(void) {
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void) {
    if (s_lock) xSemaphoreGive(s_lock);
}

// Allocate the held block into s_block1/s_block2. False when neither
// the single block nor the two pieces fit.
static bool arm(void) {
    void *b1 = heap_caps_malloc(TRANSFER_RESERVE_BYTES, TRANSFER_RESERVE_CAPS);
    void *b2 = NULL;
    if (!b1) {
        b1 = heap_caps_malloc(TRANSFER_RESERVE_PIECE1, TRANSFER_RESERVE_CAPS);
        b2 = heap_caps_malloc(TRANSFER_RESERVE_PIECE2, TRANSFER_RESERVE_CAPS);
        if (!b1 || !b2) {
            heap_caps_free(b1);
            heap_caps_free(b2);
            return false;
        }
    }
    lock();
    s_block1 = b1;
    s_block2 = b2;
    s_rearm_failed = false;
    unlock();
    return true;
}

void transfer_reserve_init(void) {
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (arm()) {
        ESP_LOGI(TAG, "transfer reserve armed: %d bytes", TRANSFER_RESERVE_BYTES);
    } else {
        lock();
        s_rearm_failed = true;
        unlock();
        ESP_LOGE(TAG, "transfer reserve could not be allocated; "
                      "installs and OTA keep the pre-reserve behaviour");
    }
}

bool transfer_reserve_acquire(transfer_reserve_holder_t who) {
    lock();
    if (s_holder != TRANSFER_RESERVE_NONE) {
        unlock();
        return false;
    }
    s_holder = who;
    void *b1 = s_block1, *b2 = s_block2;
    s_block1 = s_block2 = NULL;
    unlock();
    heap_caps_free(b1);
    heap_caps_free(b2);
    return true;
}

void transfer_reserve_rearm(transfer_reserve_holder_t who) {
    lock();
    if (s_holder != who) {
        unlock();
        return;
    }
    s_holder = TRANSFER_RESERVE_NONE;
    unlock();
    // Let the idle task reap a just-deleted task's stack before asking
    // for the block back, as reserve_stack() does in noise_control.cpp.
    vTaskDelay(pdMS_TO_TICKS(20));
    if (!arm()) {
        lock();
        s_rearm_failed = true;
        unlock();
        ESP_LOGE(TAG, "transfer reserve re-arm failed; installs and OTA "
                      "keep the pre-reserve behaviour until a re-arm succeeds");
    }
}

const char *transfer_reserve_state(void) {
    lock();
    const char *state = s_holder != TRANSFER_RESERVE_NONE ? "reserve in use"
                        : s_block1                        ? "reserve held"
                        : s_rearm_failed                  ? "reserve rearming-failed"
                                                          : "reserve unavailable";
    unlock();
    return state;
}

#else   // no PSRAM: no reserve, and no behaviour change

void transfer_reserve_init(void) {}

bool transfer_reserve_acquire(transfer_reserve_holder_t who) {
    (void)who;
    return true;
}

void transfer_reserve_rearm(transfer_reserve_holder_t who) {
    (void)who;
}

const char *transfer_reserve_state(void) {
    return "reserve unavailable";
}

#endif
