// Hoptics-MC firmware updates from the web page (see hx_ota.h). MIT licence.
#include "hx_ota.h"
#include "hx_log.h"
#include <string.h>

// which board this build is for, findable in the image itself: an upload must carry the same one
#if defined(LILYGO_TDECK)
#define HX_BOARD_ID "tdeck"
#elif defined(THINKNODE_M9)
#define HX_BOARD_ID "m9"
#elif defined(HELTEC_V4_R8_TFT)
#define HX_BOARD_ID "v4r8"
#elif defined(HELTEC_LORA_V4_TFT)
#define HX_BOARD_ID "v4"
#else
#define HX_BOARD_ID "pc"
#endif
extern "C" __attribute__((used)) const char hxBoardTag[] = "HXBOARD:" HX_BOARD_ID ";";

namespace hxota
{
volatile bool restartWanted = false;
}

#if defined(ARDUINO_ARCH_ESP32)
#include <Arduino.h>
#if __has_include(<esp_app_desc.h>) // ESP-IDF 5; in 4.4 it is part of esp_app_format.h
#include <esp_app_desc.h>
#endif
#include <esp_app_format.h>
#include <esp_ota_ops.h>
#ifndef OTA_WITH_SEQUENTIAL_WRITES
#define OTA_WITH_SEQUENTIAL_WRITES OTA_SIZE_UNKNOWN // erases the slot first: slower, same result
#endif
#include <nvs.h>

// Arduino keeps a new build at start-up unless told otherwise: Hoptics keeps it itself, after a healthy minute
extern "C" bool verifyRollbackLater() { return true; }

namespace hxota
{
namespace
{
const char *const NS = "hx_ota", *const KEY = "slot0"; // the first slot's build when a web update went to the second

bool pendingVerify()
{
    esp_ota_img_states_t st;
    const esp_partition_t *run = esp_ota_get_running_partition();
    return run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;
}

// the board id after "HXBOARD:" in a piece of the image (and the carry from the last piece)
void scan(Upload &u, const uint8_t *d, size_t n)
{
    if (u.board[0])
        return;
    static const char NEEDLE[] = "HXBOARD:";
    const size_t NL = sizeof(NEEDLE) - 1;
    uint8_t buf[24 + 512];
    size_t off = 0;
    while (off < n && !u.board[0]) {
        const size_t take = n - off < 512 ? n - off : 512;
        memcpy(buf, u.carry, u.carryLen);
        memcpy(buf + u.carryLen, d + off, take);
        const size_t len = u.carryLen + take;
        for (size_t i = 0; i + NL + 2 <= len; i++) {
            if (buf[i] != 'H' || memcmp(buf + i, NEEDLE, NL))
                continue;
            size_t k = 0;
            while (i + NL + k < len && k < sizeof(u.board) - 1 && ((buf[i + NL + k] >= 'a' && buf[i + NL + k] <= 'z') || (buf[i + NL + k] >= '0' && buf[i + NL + k] <= '9')))
                k++;
            if (k && i + NL + k < len && buf[i + NL + k] == ';') { // the tag (this file's own needle has no id after it)
                memcpy(u.board, buf + i + NL, k);
                u.board[k] = 0;
                break;
            }
        }
        u.carryLen = len < sizeof(u.carry) ? len : sizeof(u.carry); // a tag cut across two pieces is found in the next
        memcpy(u.carry, buf + len - u.carryLen, u.carryLen);
        off += take;
    }
}
} // namespace

const char *slot()
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (!run)
        return "?";
    return run->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0 ? "ota_0" : run->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1 ? "ota_1" : "factory";
}

bool trial() { return pendingVerify(); }

void bootCheck()
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *first = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
    if (!run || !first || run->subtype != ESP_PARTITION_SUBTYPE_APP_OTA_1)
        return;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK)
        return;
    uint8_t was[32];
    size_t n = sizeof(was);
    const bool known = nvs_get_blob(h, KEY, was, &n) == ESP_OK && n == sizeof(was);
    nvs_close(h);
    esp_app_desc_t d;
    if (!known || esp_ota_get_partition_description(first, &d) != ESP_OK || !memcmp(d.app_elf_sha256, was, sizeof(was)))
        return;
    hxlog::line("update: a new build was flashed over USB, starting it");
    if (esp_ota_set_boot_partition(first) == ESP_OK)
        esp_restart();
}

void confirm()
{
    if (pendingVerify() && esp_ota_mark_app_valid_cancel_rollback() == ESP_OK)
        hxlog::line("update: the new build started well and is kept");
}

void bootFailed()
{
    if (!pendingVerify())
        return;
    hxlog::line("update: the new build stopped at start-up, going back to the last one");
    hxlog::save();
    delay(10000); // the error stays on the screen for a moment
    esp_ota_mark_app_invalid_rollback_and_reboot();
}

bool begin(Upload &u, size_t total, const char *&err)
{
    abort(u);
    u = Upload();
    const esp_partition_t *part = esp_ota_get_next_update_partition(nullptr);
    if (!part)
        return err = "This board has no second app slot for updates", false;
    if (total > part->size)
        return err = "That file is too big to be firmware for this board", false;
    if (pendingVerify())
        return err = "The last update is still on trial: wait a minute after it starts", false;
    u.total = total;
    return true;
}

bool write(Upload &u, const uint8_t *data, size_t len, const char *&err)
{
    scan(u, data, len);
    if (!u.open) { // the first bytes: an app image (not the full .bin with the bootloader), then start writing
        const size_t take = len < sizeof(u.head) - u.headLen ? len : sizeof(u.head) - u.headLen;
        memcpy(u.head + u.headLen, data, take);
        u.headLen += take;
        data += take;
        len -= take;
        if (u.headLen < sizeof(u.head))
            return true;
        uint32_t magic;
        memcpy(&magic, u.head + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t), 4);
        if (u.head[0] != ESP_IMAGE_HEADER_MAGIC || magic != ESP_APP_DESC_MAGIC_WORD)
            return err = "That isn't an app image: choose the file ending in .update.bin", false;
        esp_ota_handle_t h = 0;
        if (esp_ota_begin(esp_ota_get_next_update_partition(nullptr), OTA_WITH_SEQUENTIAL_WRITES, &h) != ESP_OK)
            return err = "Couldn't start writing the update", false;
        u.handle = (void *)h;
        u.open = true;
        if (esp_ota_write(h, u.head, u.headLen) != ESP_OK)
            return err = "Couldn't write the update", false;
        u.got = u.headLen;
    }
    if (len && esp_ota_write((esp_ota_handle_t)u.handle, data, len) != ESP_OK)
        return err = "Couldn't write the update", false;
    u.got += len;
    return true;
}

bool finish(Upload &u, const char *&err)
{
    if (!u.open || u.got != u.total)
        return abort(u), err = "The upload stopped part way", false;
    const size_t idLen = strlen(hxBoardTag) - 9; // without "HXBOARD:" and ';'
    if (strlen(u.board) != idLen || strncmp(u.board, hxBoardTag + 8, idLen)) {
        abort(u);
        return err = u.board[0] ? "That firmware is for a different board" : "That isn't Hoptics-MC firmware", false;
    }
    const esp_ota_handle_t h = (esp_ota_handle_t)u.handle;
    u.open = false;
    if (esp_ota_end(h) != ESP_OK) // checks the image's own checksum and hash
        return err = "The file was damaged on the way: try again", false;
    const esp_partition_t *part = esp_ota_get_next_update_partition(nullptr), *run = esp_ota_get_running_partition();
    nvs_handle_t nh;
    if (nvs_open(NS, NVS_READWRITE, &nh) == ESP_OK) { // what the first slot holds now, to notice a USB flash later
        esp_app_desc_t d;
        if (part->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1 && run && esp_ota_get_partition_description(run, &d) == ESP_OK)
            nvs_set_blob(nh, KEY, d.app_elf_sha256, sizeof(d.app_elf_sha256));
        else
            nvs_erase_key(nh, KEY);
        nvs_commit(nh);
        nvs_close(nh);
    }
    if (esp_ota_set_boot_partition(part) != ESP_OK)
        return err = "Couldn't select the update to start", false;
    hxlog::line("update: received and checked, %u KB, starting it on trial", (unsigned)(u.total / 1024));
    restartWanted = true;
    return true;
}

void abort(Upload &u)
{
    if (u.open)
        esp_ota_abort((esp_ota_handle_t)u.handle);
    u.open = false;
}
} // namespace hxota

#else // a PC: no updates

namespace hxota
{
void bootCheck() {}
bool trial() { return false; }
void confirm() {}
void bootFailed() {}
const char *slot() { return "pc"; }
bool begin(Upload &, size_t, const char *&err) { return err = "No updates on a PC", false; }
bool write(Upload &, const uint8_t *, size_t, const char *&err) { return err = "No updates on a PC", false; }
bool finish(Upload &, const char *&err) { return err = "No updates on a PC", false; }
void abort(Upload &) {}
} // namespace hxota

#endif
