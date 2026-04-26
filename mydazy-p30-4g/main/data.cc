#include "data.h"
#include "ble_nus.h"
#include "display.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>

#include <cJSON.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <nvs.h>

static const char *TAG = "data";

static tama_state_t state_;
static char pet_name_[24] = "Buddy";
static char owner_name_[24] = "";

static uint32_t now_ms() { return (uint32_t)(esp_timer_get_time() / 1000); }

// ─── NVS helpers ─────────────────────────────────────────────────────────
static void load_names() {
    nvs_handle_t h;
    if (nvs_open("buddy", NVS_READONLY, &h) != ESP_OK) return;
    size_t n = sizeof(pet_name_);
    nvs_get_str(h, "name", pet_name_, &n);
    n = sizeof(owner_name_);
    nvs_get_str(h, "owner", owner_name_, &n);
    nvs_close(h);
}
static void save_string(const char *key, const char *val) {
    nvs_handle_t h;
    if (nvs_open("buddy", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, key, val);
    nvs_commit(h);
    nvs_close(h);
}

// ─── ack helpers ─────────────────────────────────────────────────────────
static void send_ack(const char *what, bool ok, uint32_t n) {
    char buf[80];
    snprintf(buf, sizeof(buf), "{\"ack\":\"%s\",\"ok\":%s,\"n\":%lu}",
             what, ok ? "true" : "false", (unsigned long)n);
    ble_nus_send_line(buf);
}

static void send_ack_with_error(const char *what, const char *err) {
    char buf[160];
    snprintf(buf, sizeof(buf), "{\"ack\":\"%s\",\"ok\":false,\"n\":0,\"error\":\"%s\"}",
             what, err);
    ble_nus_send_line(buf);
}

// REFERENCE.md "Status response" — the desktop polls this every couple
// seconds. We omit fields we don't have (battery, fs).
static void send_status() {
    uint32_t up_s = (uint32_t)(esp_timer_get_time() / 1000000);
    uint32_t heap = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

    char buf[320];
    int n = snprintf(buf, sizeof(buf),
        "{\"ack\":\"status\",\"ok\":true,\"n\":0,\"data\":{"
        "\"name\":\"%s\",\"owner\":\"%s\",\"sec\":true,"
        "\"sys\":{\"up\":%lu,\"heap\":%lu}"
        "}}",
        pet_name_, owner_name_,
        (unsigned long)up_s, (unsigned long)heap);
    if (n > 0 && n < (int)sizeof(buf)) ble_nus_send_line(buf);
}

// ─── Sync state to display ───────────────────────────────────────────────
static void push_to_display() {
    display_set_msg(state_.msg);
    display_set_counts(state_.total, state_.running, state_.waiting);
    display_set_tokens(state_.tokens_today);
    const char *ents[3] = {0};
    for (uint8_t i = 0; i < state_.n_entries && i < 3; i++) ents[i] = state_.entries[i];
    display_set_entries(ents, state_.n_entries);
    if (state_.prompt_id[0]) {
        display_set_prompt(state_.prompt_tool, state_.prompt_hint);
    } else {
        display_clear_prompt();
    }
    display_set_link(now_ms() - state_.last_update_ms < 30000
                     ? DISPLAY_LINK_PAIRED_LIVE
                     : DISPLAY_LINK_PAIRED_IDLE);
}

// ─── JSON dispatch ───────────────────────────────────────────────────────
//
// Three message families share the same line:
//   1. Heartbeat snapshot — has total/running/etc., updates state_
//   2. Time sync          — {"time":[epoch, tz_offset]}
//   3. Command             — {"cmd":"..."} → ack
//
// REFERENCE.md doesn't allow mixing them in one line, so we dispatch on
// presence of the discriminator field.

static void apply_command(cJSON *root) {
    cJSON *jcmd = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    if (!cJSON_IsString(jcmd)) return;
    const char *cmd = jcmd->valuestring;

    if (strcmp(cmd, "name") == 0) {
        cJSON *jn = cJSON_GetObjectItemCaseSensitive(root, "name");
        if (cJSON_IsString(jn) && jn->valuestring) {
            strncpy(pet_name_, jn->valuestring, sizeof(pet_name_) - 1);
            pet_name_[sizeof(pet_name_) - 1] = 0;
            save_string("name", pet_name_);
        }
        send_ack("name", true, 0);

    } else if (strcmp(cmd, "owner") == 0) {
        cJSON *jn = cJSON_GetObjectItemCaseSensitive(root, "name");
        if (cJSON_IsString(jn) && jn->valuestring) {
            strncpy(owner_name_, jn->valuestring, sizeof(owner_name_) - 1);
            owner_name_[sizeof(owner_name_) - 1] = 0;
            save_string("owner", owner_name_);
        }
        send_ack("owner", true, 0);

    } else if (strcmp(cmd, "unpair") == 0) {
        send_ack("unpair", true, 0);
        ble_nus_clear_bonds();

    } else if (strcmp(cmd, "status") == 0) {
        send_status();

    } else if (strcmp(cmd, "permission") == 0) {
        // We never expect to receive this — desktop sends our own
        // permission JSON back? Per REFERENCE.md the device sends it,
        // not the desktop. Ignore + ack false.
        send_ack_with_error("permission", "unexpected from desktop");

    } else if (strcmp(cmd, "char_begin") == 0) {
        // Phase 1 doesn't accept folder pushes. Returning ok:false here
        // (as REFERENCE.md allows) makes the desktop time out cleanly
        // rather than streaming chunks we'd just drop.
        send_ack_with_error("char_begin", "char push not supported in phase 1");

    } else {
        // Other commands — don't ack (REFERENCE.md doesn't require it).
    }
}

static void apply_time(cJSON *jtime) {
    if (!cJSON_IsArray(jtime) || cJSON_GetArraySize(jtime) != 2) return;
    cJSON *jepoch = cJSON_GetArrayItem(jtime, 0);
    cJSON *jtz    = cJSON_GetArrayItem(jtime, 1);
    if (!cJSON_IsNumber(jepoch) || !cJSON_IsNumber(jtz)) return;
    time_t epoch = (time_t)jepoch->valuedouble;
    int32_t tz_sec = (int32_t)jtz->valuedouble;
    struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    char tz_buf[16];
    int hh = tz_sec / 3600;
    int mm = (tz_sec < 0 ? -tz_sec : tz_sec) % 3600 / 60;
    snprintf(tz_buf, sizeof(tz_buf), "UTC%+d:%02d", hh, mm);
    setenv("TZ", tz_buf, 1);
    tzset();
    ESP_LOGI(TAG, "time sync: epoch=%lld tz=%s", (long long)epoch, tz_buf);
}

static void apply_heartbeat(cJSON *root) {
    cJSON *j;
    if ((j = cJSON_GetObjectItemCaseSensitive(root, "total")) && cJSON_IsNumber(j))
        state_.total = (uint8_t)j->valueint;
    if ((j = cJSON_GetObjectItemCaseSensitive(root, "running")) && cJSON_IsNumber(j))
        state_.running = (uint8_t)j->valueint;
    if ((j = cJSON_GetObjectItemCaseSensitive(root, "waiting")) && cJSON_IsNumber(j))
        state_.waiting = (uint8_t)j->valueint;
    if ((j = cJSON_GetObjectItemCaseSensitive(root, "completed")) &&
        (cJSON_IsTrue(j) || cJSON_IsFalse(j)))
        state_.recently_completed = cJSON_IsTrue(j);
    if ((j = cJSON_GetObjectItemCaseSensitive(root, "tokens_today")) && cJSON_IsNumber(j))
        state_.tokens_today = (uint32_t)j->valuedouble;
    if ((j = cJSON_GetObjectItemCaseSensitive(root, "msg")) && cJSON_IsString(j) && j->valuestring) {
        strncpy(state_.msg, j->valuestring, sizeof(state_.msg) - 1);
        state_.msg[sizeof(state_.msg) - 1] = 0;
    }
    cJSON *jents = cJSON_GetObjectItemCaseSensitive(root, "entries");
    if (cJSON_IsArray(jents)) {
        state_.n_entries = 0;
        cJSON *e;
        cJSON_ArrayForEach(e, jents) {
            if (state_.n_entries >= 3) break;
            if (cJSON_IsString(e) && e->valuestring) {
                strncpy(state_.entries[state_.n_entries], e->valuestring,
                        sizeof(state_.entries[0]) - 1);
                state_.entries[state_.n_entries][sizeof(state_.entries[0]) - 1] = 0;
                state_.n_entries++;
            }
        }
    }
    cJSON *jp = cJSON_GetObjectItemCaseSensitive(root, "prompt");
    if (cJSON_IsObject(jp)) {
        cJSON *jid = cJSON_GetObjectItemCaseSensitive(jp, "id");
        cJSON *jt  = cJSON_GetObjectItemCaseSensitive(jp, "tool");
        cJSON *jh  = cJSON_GetObjectItemCaseSensitive(jp, "hint");
        const char *pid = (cJSON_IsString(jid) && jid->valuestring) ? jid->valuestring : "";
        const char *pt  = (cJSON_IsString(jt) && jt->valuestring) ? jt->valuestring : "";
        const char *ph  = (cJSON_IsString(jh) && jh->valuestring) ? jh->valuestring : "";
        strncpy(state_.prompt_id, pid, sizeof(state_.prompt_id) - 1);
        state_.prompt_id[sizeof(state_.prompt_id) - 1] = 0;
        strncpy(state_.prompt_tool, pt, sizeof(state_.prompt_tool) - 1);
        state_.prompt_tool[sizeof(state_.prompt_tool) - 1] = 0;
        strncpy(state_.prompt_hint, ph, sizeof(state_.prompt_hint) - 1);
        state_.prompt_hint[sizeof(state_.prompt_hint) - 1] = 0;
    } else {
        state_.prompt_id[0] = state_.prompt_tool[0] = state_.prompt_hint[0] = 0;
    }
    state_.last_update_ms = now_ms();
}

void data_apply_line(const char *line) {
    if (!line || line[0] != '{') return;
    cJSON *root = cJSON_Parse(line);
    if (!root) return;

    cJSON *jcmd  = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    cJSON *jtime = cJSON_GetObjectItemCaseSensitive(root, "time");

    if (cJSON_IsString(jcmd)) {
        apply_command(root);
    } else if (cJSON_IsArray(jtime)) {
        apply_time(jtime);
    } else {
        apply_heartbeat(root);
        push_to_display();
    }

    cJSON_Delete(root);
}

void data_on_disconnect() {
    memset(&state_, 0, sizeof(state_));
    strncpy(state_.msg, "No Claude connected", sizeof(state_.msg) - 1);
    push_to_display();
    display_clear_prompt();
}

bool data_has_pending_prompt() {
    return state_.prompt_id[0] != 0;
}

void data_send_permission(const char *decision) {
    if (!state_.prompt_id[0] || !decision) return;
    char buf[120];
    snprintf(buf, sizeof(buf),
             "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"%s\"}",
             state_.prompt_id, decision);
    ble_nus_send_line(buf);
    // Optimistically clear the local prompt — desktop will send a fresh
    // heartbeat without `prompt` shortly anyway.
    state_.prompt_id[0] = 0;
    state_.prompt_tool[0] = 0;
    state_.prompt_hint[0] = 0;
    push_to_display();
}

void data_init() {
    memset(&state_, 0, sizeof(state_));
    strncpy(state_.msg, "No Claude connected", sizeof(state_.msg) - 1);
    load_names();
    push_to_display();
}
