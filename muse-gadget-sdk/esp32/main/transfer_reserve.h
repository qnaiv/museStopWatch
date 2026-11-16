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

#ifdef __cplusplus
extern "C" {
#endif

/*
 * museStopWatch dev.20: the transfer reserve (work/dev20-memory-rules-
 * 2026-11-16.md §3.3, rule R3). One internal-RAM block, allocated at boot
 * before Wi-Fi/UI churn fragments the heap and held idle, shared by the
 * two transfer-class operations whose starts must not fail on heap
 * history: app.install's file download (image_fetch.c) and device.ota
 * (ota.c). At admission the holder frees the block, so the operation's
 * existing memory gate passes by construction; when the operation's
 * task ends (or its start fails after admission) the block is
 * re-allocated ("re-armed"). Generalises noise_control.cpp's
 * reserve_stack() pattern.
 *
 * One holder at a time: a second transfer-class operation while one is
 * active is refused admission and applies its existing busy behaviour.
 * On builds without PSRAM there is no reserve: acquire always succeeds
 * and frees nothing, so those builds behave exactly as before.
 */
typedef enum {
    TRANSFER_RESERVE_NONE = 0,
    TRANSFER_RESERVE_INSTALL,   // image_fetch.c's vm_file download
    TRANSFER_RESERVE_OTA,       // ota.c's OTA download
} transfer_reserve_holder_t;

// Allocate the held block. Call once, early in app_main, before Wi-Fi
// and the UI start. No-op on builds without PSRAM.
void transfer_reserve_init(void);

// Admission: free the held block and mark `who` as the holder. False
// when another transfer-class operation already holds the reserve.
bool transfer_reserve_acquire(transfer_reserve_holder_t who);

// The holder is done: re-allocate the held block, in two pieces if the
// freed region has fragmented. No-op when `who` is not the holder. A
// failed re-arm is remembered for transfer_reserve_state(): the system
// has degraded to the pre-reserve behaviour, never worse.
void transfer_reserve_rearm(transfer_reserve_holder_t who);

// "reserve held", "reserve in use", "reserve rearming-failed" or
// "reserve unavailable": for refusal-message suffixes (R8).
const char *transfer_reserve_state(void);

#ifdef __cplusplus
}
#endif
