// Hoptics-MC web page (see hx_web.h). MIT licence.
#include "hx_web.h"
#include "hx_log.h"
#include "hx_ota.h"
#include "hx_parse.h"
#include "map/hx_tiles.h"

#if defined(ARDUINO_ARCH_ESP32)
#include "hx_sd.h"
#include "hx_web_files.h"
#include <Arduino.h>
#include <ff.h>
#include <Preferences.h>
#include <esp_http_server.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <algorithm>
#include <string.h>

namespace hxweb
{
namespace
{
httpd_handle_t srv = nullptr;
SemaphoreHandle_t lock = nullptr;  // the PIN, the sessions and the waiting query
SemaphoreHandle_t one = nullptr;   // one data request at a time
SemaphoreHandle_t ready = nullptr; // the main loop answered
char pin[8] = "";
constexpr uint32_t SESSION_MS = 12ul * 3600 * 1000;
struct Session {
    char tok[33];
    uint32_t last;
} ses[4] = {};
uint8_t fails = 0;
uint32_t lockUntil = 0;

// the sessions survive a restart (a firmware update over Wi-Fi restarts the node: the page shouldn't have to sign
// in again): their tokens are kept with the PIN they were made under, and only come back for that PIN
void keepSessions()
{
    char toks[4][33];
    xSemaphoreTake(lock, portMAX_DELAY);
    for (int i = 0; i < 4; i++)
        memcpy(toks[i], ses[i].tok, 33);
    char p[8];
    memcpy(p, pin, sizeof(p));
    xSemaphoreGive(lock);
    Preferences nv;
    if (!nv.begin("hxweb", false))
        return;
    nv.putBytes("ses", toks, sizeof(toks));
    nv.putString("sespin", p);
    nv.end();
}
volatile bool waiting = false; // read without the lock by the main loop on every pass
bool taken = false;
Query pending;
std::string *reply = nullptr;

// what the page may load and talk to: itself, the map tiles, the terrain service, and a CoreScope live feed
const char *CSP = "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data: "
                  "https://tile.openstreetmap.org; connect-src 'self' wss: ws: https://api.open-meteo.com; frame-ancestors 'none'; "
                  "base-uri 'none'; form-action 'none'";

void headers(httpd_req_t *r)
{
    httpd_resp_set_hdr(r, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(r, "X-Frame-Options", "DENY");
    httpd_resp_set_hdr(r, "Referrer-Policy", "no-referrer");
    httpd_resp_set_hdr(r, "Content-Security-Policy", CSP);
}
esp_err_t sendJson(httpd_req_t *r, const char *status, const char *json)
{
    httpd_resp_set_status(r, status);
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_send(r, json, HTTPD_RESP_USE_STRLEN);
}

// the session token in the "hx" cookie, if it is one of ours (and still fresh): its slot, or -1
int sessionOf(httpd_req_t *r)
{
    char c[640]; // other sites' cookies on the same address come along too: a short buffer read as "signed out"
    if (httpd_req_get_hdr_value_str(r, "Cookie", c, sizeof(c)) != ESP_OK)
        return -1;
    const char *p = strstr(c, "hx=");
    while (p && p != c && p[-1] != ' ' && p[-1] != ';')
        p = strstr(p + 3, "hx=");
    if (!p)
        return -1;
    p += 3;
    if (strspn(p, "0123456789abcdef") != 32)
        return -1;
    int found = -1;
    xSemaphoreTake(lock, portMAX_DELAY);
    const uint32_t now = millis();
    for (int i = 0; i < 4; i++)
        if (ses[i].tok[0] && !strncmp(ses[i].tok, p, 32)) {
            if (now - ses[i].last < SESSION_MS) {
                ses[i].last = now;
                found = i;
            } else
                ses[i].tok[0] = 0; // expired
        }
    xSemaphoreGive(lock);
    return found;
}

// hands a query to the main loop and waits for its answer. took: whether the main loop took it (a RESTORE's data is
// then the main loop's to free, answered in time or not)
bool ask(const Query &q, std::string &out, uint32_t waitMs = 1500, bool *took = nullptr)
{
    xSemaphoreTake(one, portMAX_DELAY);
    xSemaphoreTake(ready, 0); // a late answer to a request that gave up
    xSemaphoreTake(lock, portMAX_DELAY);
    pending = q;
    reply = &out;
    waiting = true;
    taken = false;
    xSemaphoreGive(lock);
    const bool ok = xSemaphoreTake(ready, pdMS_TO_TICKS(waitMs)) == pdTRUE;
    xSemaphoreTake(lock, portMAX_DELAY);
    if (took)
        *took = taken;
    waiting = taken = false;
    reply = nullptr;
    xSemaphoreGive(lock);
    xSemaphoreGive(one);
    return ok;
}

uint32_t queryNum(const char *qs, const char *key)
{
    char v[16];
    return httpd_query_key_value(qs, key, v, sizeof(v)) == ESP_OK ? strtoul(v, nullptr, 10) : 0;
}

esp_err_t onFile(httpd_req_t *r)
{
    const File &f = files[(int)(intptr_t)r->user_ctx];
    headers(r);
    char inm[24];
    if (httpd_req_get_hdr_value_str(r, "If-None-Match", inm, sizeof(inm)) == ESP_OK && !strcmp(inm, f.etag)) {
        httpd_resp_set_status(r, "304 Not Modified");
        return httpd_resp_send(r, nullptr, 0);
    }
    httpd_resp_set_type(r, f.type);
    httpd_resp_set_hdr(r, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(r, "Cache-Control", "no-cache"); // kept, but checked against the ETag each time
    httpd_resp_set_hdr(r, "ETag", f.etag);
    return httpd_resp_send(r, (const char *)f.gz, f.len);
}

esp_err_t onLogin(httpd_req_t *r)
{
    headers(r);
    char ct[40], body[80];
    if (httpd_req_get_hdr_value_str(r, "Content-Type", ct, sizeof(ct)) != ESP_OK || strncmp(ct, "application/json", 16))
        return sendJson(r, "415 Unsupported Media Type", "{\"error\":\"Send JSON\"}");
    if (r->content_len <= 0 || r->content_len >= sizeof(body))
        return sendJson(r, "400 Bad Request", "{\"error\":\"Bad request\"}");
    int n = httpd_req_recv(r, body, r->content_len);
    if (n <= 0)
        return sendJson(r, "400 Bad Request", "{\"error\":\"Bad request\"}");
    body[n] = 0;
    char given[8] = "";
    if (const char *p = strstr(body, "\"pin\"")) {
        p += 5;
        p += strspn(p, " :\"");
        if (strspn(p, "0123456789") == 6)
            memcpy(given, p, 6), given[6] = 0;
    }
    char resp[96];
    xSemaphoreTake(lock, portMAX_DELAY);
    const uint32_t now = millis();
    if (lockUntil && (int32_t)(lockUntil - now) > 0) {
        const unsigned wait = (lockUntil - now + 999) / 1000;
        xSemaphoreGive(lock);
        snprintf(resp, sizeof(resp), "{\"error\":\"Too many wrong PINs.\",\"wait\":%u}", wait);
        return sendJson(r, "429 Too Many Requests", resp);
    }
    uint8_t diff = strlen(pin) != 6 || strlen(given) != 6;
    for (int i = 0; i < 6; i++) // the same time whatever the PIN
        diff |= (uint8_t)(pin[i] ^ given[i]);
    if (diff) {
        fails++;
        if (fails >= 5) // a minute, then longer each time, up to 15 minutes
            lockUntil = now + std::min<uint32_t>(60000u << std::min<int>(fails - 5, 4), 900000u);
        xSemaphoreGive(lock);
        hxlog::line("web page: a sign-in failed");
        return sendJson(r, "401 Unauthorized", "{\"error\":\"That PIN didn't work.\"}");
    }
    fails = 0;
    lockUntil = 0;
    int slot = 0;
    for (int i = 0; i < 4; i++) { // a free slot, or the one used longest ago
        if (!ses[i].tok[0]) {
            slot = i;
            break;
        }
        if (ses[i].last < ses[slot].last)
            slot = i;
    }
    uint8_t rnd[16];
    esp_fill_random(rnd, sizeof(rnd));
    for (int i = 0; i < 16; i++)
        snprintf(ses[slot].tok + i * 2, 3, "%02x", rnd[i]);
    ses[slot].last = now;
    char cookie[96];
    snprintf(cookie, sizeof(cookie), "hx=%s; Path=/; HttpOnly; SameSite=Strict; Max-Age=43200", ses[slot].tok);
    xSemaphoreGive(lock);
    keepSessions();
    hxlog::line("web page: a browser signed in");
    httpd_resp_set_hdr(r, "Set-Cookie", cookie);
    return sendJson(r, "200 OK", "{\"ok\":true}");
}

esp_err_t onLogout(httpd_req_t *r)
{
    headers(r);
    const int s = sessionOf(r);
    if (s >= 0) {
        xSemaphoreTake(lock, portMAX_DELAY);
        ses[s].tok[0] = 0;
        xSemaphoreGive(lock);
        keepSessions();
    }
    httpd_resp_set_hdr(r, "Set-Cookie", "hx=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
    return sendJson(r, "200 OK", "{\"ok\":true}");
}

esp_err_t onState(httpd_req_t *r)
{
    headers(r);
    if (sessionOf(r) < 0)
        return sendJson(r, "401 Unauthorized", "{\"error\":\"Sign in\"}");
    Query q;
    q.kind = Query::STATE;
    char qs[96];
    if (httpd_req_get_url_query_str(r, qs, sizeof(qs)) == ESP_OK) {
        q.contacts = queryNum(qs, "cv");
        q.packets = queryNum(qs, "ps");
        q.msgs = queryNum(qs, "ms");
        q.hist = queryNum(qs, "hv");
    }
    std::string body;
    if (!ask(q, body))
        return sendJson(r, "503 Service Unavailable", "{\"error\":\"The node is busy, try again\"}");
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_send(r, body.data(), body.size());
}

esp_err_t onLog(httpd_req_t *r)
{
    headers(r);
    if (sessionOf(r) < 0)
        return sendJson(r, "401 Unauthorized", "{\"error\":\"Sign in\"}");
    Query q;
    q.kind = Query::LOG;
    std::string body;
    if (!ask(q, body))
        return sendJson(r, "503 Service Unavailable", "{\"error\":\"The node is busy, try again\"}");
    httpd_resp_set_type(r, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_send(r, body.data(), body.size());
}
// a firmware update: the .update.bin as the body, from the signed-in page only (it alone sends X-Hoptics: a form on
// another site can't, and the cookie is SameSite=Strict too). Written to the other app slot as it arrives.
esp_err_t onUpdate(httpd_req_t *r)
{
    headers(r);
    if (sessionOf(r) < 0)
        return sendJson(r, "401 Unauthorized", "{\"error\":\"Sign in\"}");
    char x[4];
    if (httpd_req_get_hdr_value_str(r, "X-Hoptics", x, sizeof(x)) != ESP_OK)
        return sendJson(r, "403 Forbidden", "{\"error\":\"Send it from the Hoptics page\"}");
    static hxota::Upload up; // one at a time (the server has one task)
    const char *err = nullptr;
    if (r->content_len < 64 * 1024 || !hxota::begin(up, r->content_len, err)) {
        char b[160];
        snprintf(b, sizeof(b), "{\"error\":\"%s\"}", err ? err : "That file is too small to be firmware");
        return sendJson(r, "400 Bad Request", b);
    }
    hxlog::line("update: receiving %u KB from the web page", (unsigned)(r->content_len / 1024));
    uint8_t *buf = (uint8_t *)malloc(4096);
    size_t left = r->content_len;
    int timeouts = 0;
    while (buf && left && !err) {
        const int n = httpd_req_recv(r, (char *)buf, left < 4096 ? left : 4096);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 5)
            continue;
        if (n <= 0) {
            err = "The upload stopped part way";
            break;
        }
        timeouts = 0;
        left -= n;
        hxota::write(up, buf, n, err);
    }
    free(buf);
    if (!err && left)
        err = "Out of memory";
    if (err || !hxota::finish(up, err)) {
        hxota::abort(up);
        hxlog::line("update: refused (%s)", err);
        char b[160];
        snprintf(b, sizeof(b), "{\"error\":\"%s\"}", err);
        return sendJson(r, "400 Bad Request", b);
    }
    return sendJson(r, "200 OK", "{\"ok\":true}");
}
// a channel message from the page: POST /api/send {"c":<channel>,"text":"..."}. Sent by the main loop, as from
// the screen, so it shows in the node's chats too.
esp_err_t onSend(httpd_req_t *r)
{
    headers(r);
    if (sessionOf(r) < 0)
        return sendJson(r, "401 Unauthorized", "{\"error\":\"Sign in\"}");
    char x[4], body[640];
    if (httpd_req_get_hdr_value_str(r, "X-Hoptics", x, sizeof(x)) != ESP_OK)
        return sendJson(r, "403 Forbidden", "{\"error\":\"Send it from the Hoptics page\"}");
    if (r->content_len <= 0 || r->content_len >= sizeof(body))
        return sendJson(r, "400 Bad Request", "{\"error\":\"That message is too long\"}");
    size_t got = 0;
    while (got < (size_t)r->content_len) {
        const int n = httpd_req_recv(r, body + got, r->content_len - got);
        if (n <= 0)
            return sendJson(r, "400 Bad Request", "{\"error\":\"Bad request\"}");
        got += n;
    }
    body[got] = 0;
    Query q;
    q.kind = Query::SEND;
    const char *c = strstr(body, "\"c\""), *t = strstr(body, "\"text\"");
    if (!c || !t)
        return sendJson(r, "400 Bad Request", "{\"error\":\"Bad request\"}");
    c += 3;
    c += strspn(c, " :");
    if (*c < '0' || *c > '9') // a number only: anything else must not fall back to channel 0 (Public)
        return sendJson(r, "400 Bad Request", "{\"error\":\"Bad channel\"}");
    q.channel = (uint8_t)atoi(c);
    t += 6;
    t += strspn(t, " :");
    if (*t != '"' || !hxparse::jsonText(t + 1, q.text, sizeof(q.text)) || !q.text[0])
        return sendJson(r, "400 Bad Request", "{\"error\":\"That message is too long\"}");
    std::string out;
    if (!ask(q, out))
        return sendJson(r, "503 Service Unavailable", "{\"error\":\"The node is busy, try again\"}");
    return sendJson(r, out.find("\"ok\"") != std::string::npos ? "200 OK" : "400 Bad Request", out.c_str());
}

// join a #hashtag channel: POST /api/channel {"name":"#test"}. Done by the main loop, which owns the channels
esp_err_t onJoin(httpd_req_t *r)
{
    headers(r);
    if (sessionOf(r) < 0)
        return sendJson(r, "401 Unauthorized", "{\"error\":\"Sign in\"}");
    char x[4], body[128];
    if (httpd_req_get_hdr_value_str(r, "X-Hoptics", x, sizeof(x)) != ESP_OK)
        return sendJson(r, "403 Forbidden", "{\"error\":\"Send it from the Hoptics page\"}");
    if (r->content_len <= 0 || r->content_len >= sizeof(body))
        return sendJson(r, "400 Bad Request", "{\"error\":\"Bad request\"}");
    size_t got = 0;
    while (got < (size_t)r->content_len) {
        const int n = httpd_req_recv(r, body + got, r->content_len - got);
        if (n <= 0)
            return sendJson(r, "400 Bad Request", "{\"error\":\"Bad request\"}");
        got += n;
    }
    body[got] = 0;
    Query q;
    q.kind = Query::JOIN;
    const char *t = strstr(body, "\"name\"");
    if (!t)
        return sendJson(r, "400 Bad Request", "{\"error\":\"Bad request\"}");
    t += 6;
    t += strspn(t, " :");
    if (*t != '"' || !hxparse::jsonText(t + 1, q.text, 32) || !q.text[0])
        return sendJson(r, "400 Bad Request", "{\"error\":\"That name is too long\"}");
    std::string out;
    if (!ask(q, out))
        return sendJson(r, "503 Service Unavailable", "{\"error\":\"The node is busy, try again\"}");
    return sendJson(r, out.find("\"ok\"") != std::string::npos ? "200 OK" : "400 Bad Request", out.c_str());
}

// one map tile from the user's own tile pack: POST /api/tile?z=&x=&y= with the PNG as the body
esp_err_t onTile(httpd_req_t *r)
{
    headers(r);
    if (sessionOf(r) < 0)
        return sendJson(r, "401 Unauthorized", "{\"error\":\"Sign in\"}");
    char x[4], qs[48];
    if (httpd_req_get_hdr_value_str(r, "X-Hoptics", x, sizeof(x)) != ESP_OK)
        return sendJson(r, "403 Forbidden", "{\"error\":\"Send it from the Hoptics page\"}");
    if (httpd_req_get_url_query_str(r, qs, sizeof(qs)) != ESP_OK || r->content_len < 64 || r->content_len > 192 * 1024)
        return sendJson(r, "400 Bad Request", "{\"error\":\"Not a map tile\"}");
    uint8_t *png = (uint8_t *)heap_caps_malloc(r->content_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!png)
        png = (uint8_t *)malloc(r->content_len);
    if (!png)
        return sendJson(r, "503 Service Unavailable", "{\"error\":\"Out of memory\"}");
    size_t got = 0;
    while (got < r->content_len) {
        const int n = httpd_req_recv(r, (char *)png + got, r->content_len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT)
            continue;
        if (n <= 0)
            break;
        got += n;
    }
    hoptics::TileMap::ImportResult res = hoptics::TileMap::BAD;
    if (got == r->content_len)
        res = hoptics::TileMap::get().importTile((uint8_t)queryNum(qs, "z"), queryNum(qs, "x"), queryNum(qs, "y"), png, got, 8000);
    free(png);
    switch (res) {
    case hoptics::TileMap::IMPORTED:
        return sendJson(r, "200 OK", "{\"ok\":true}");
    case hoptics::TileMap::FULL:
        return sendJson(r, "507 Insufficient Storage", "{\"error\":\"The node's map store is full\"}");
    case hoptics::TileMap::BUSY:
        return sendJson(r, "503 Service Unavailable", "{\"error\":\"Busy, try again\"}");
    default:
        return sendJson(r, "400 Bad Request", "{\"error\":\"Not a PNG map tile\"}");
    }
}

esp_err_t onTilesClear(httpd_req_t *r)
{
    headers(r);
    if (sessionOf(r) < 0)
        return sendJson(r, "401 Unauthorized", "{\"error\":\"Sign in\"}");
    char x[4];
    if (httpd_req_get_hdr_value_str(r, "X-Hoptics", x, sizeof(x)) != ESP_OK)
        return sendJson(r, "403 Forbidden", "{\"error\":\"Send it from the Hoptics page\"}");
    hoptics::TileMap::get().clearSaved();
    hxlog::line("web page: saved map tiles cleared");
    return sendJson(r, "200 OK", "{\"ok\":true}");
}

// ---- the microSD card's files (the page's Files tab). Signed-in pages only, changes only from the page itself
// (X-Hoptics), and only inside the card: every path goes through hxparse::cleanPath. Card work is done by the main loop
// a piece at a time (hxsd::run), so a big file never holds the bus for long. The server has one task: the static
// buffers below are its own
bool allowed(httpd_req_t *r, bool change, esp_err_t &res)
{
    headers(r);
    char x[4];
    if (sessionOf(r) < 0)
        res = sendJson(r, "401 Unauthorized", "{\"error\":\"Sign in\"}");
    else if (change && httpd_req_get_hdr_value_str(r, "X-Hoptics", x, sizeof(x)) != ESP_OK)
        res = sendJson(r, "403 Forbidden", "{\"error\":\"Send it from the Hoptics page\"}");
    else if (!hxsd::mounted())
        res = sendJson(r, "404 Not Found", "{\"error\":\"There is no card in the node\"}");
    else
        return true;
    return false;
}

// ?path= as a path on the card (FatFS's name for it in fat); false: missing, or not a plain path on the card
bool cardPath(httpd_req_t *r, char *path, size_t n, char *fat, size_t fn)
{
    static char qs[512], raw[384], dec[256];
    if (httpd_req_get_url_query_str(r, qs, sizeof(qs)) != ESP_OK || httpd_query_key_value(qs, "path", raw, sizeof(raw)) != ESP_OK ||
        !hxparse::urlDecode(raw, dec, sizeof(dec)) || !hxparse::cleanPath(dec, path, n))
        return false;
    hxsd::fatPath(fat, fn, path);
    return true;
}
uint32_t queryOf(httpd_req_t *r, const char *key)
{
    static char qs[512];
    return httpd_req_get_url_query_str(r, qs, sizeof(qs)) == ESP_OK ? queryNum(qs, key) : 0;
}
esp_err_t notPath(httpd_req_t *r) { return sendJson(r, "400 Bad Request", "{\"error\":\"That isn't a path on the card\"}"); }
esp_err_t cardBusy(httpd_req_t *r) { return sendJson(r, "503 Service Unavailable", "{\"error\":\"The node is busy, try again\"}"); }
FIL *newFile(void) { return (FIL *)heap_caps_malloc(sizeof(FIL), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); } // ~4 KB
uint8_t *newChunk(void) { return (uint8_t *)heap_caps_malloc(8192, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }

// GET /api/files?path=/maps&from=0: a folder, 200 entries at a time, with the card's size and free space
esp_err_t onFiles(httpd_req_t *r)
{
    esp_err_t res;
    if (!allowed(r, false, res))
        return res;
    char path[200], fat[208];
    if (!cardPath(r, path, sizeof(path), fat, sizeof(fat)))
        return notPath(r);
    const uint32_t from = queryOf(r, "from");
    std::string items;
    bool open = false, more = false;
    uint32_t n = 0;
    if (!hxsd::run([&] {
            FF_DIR d;
            FILINFO fi;
            if (f_opendir(&d, fat) != FR_OK)
                return;
            open = true;
            for (uint32_t i = 0; f_readdir(&d, &fi) == FR_OK && fi.fname[0]; i++) {
                if (i < from)
                    continue;
                if (n == 200) {
                    more = true;
                    break;
                }
                items += n++ ? ",{\"n\":" : "{\"n\":";
                hxparse::jsonString(items, fi.fname);
                char b[96];
                // FAT keeps local time in two words: 7 bits of year from 1980, month, day; hours, minutes, seconds / 2
                snprintf(b, sizeof(b), ",\"d\":%s,\"s\":%lu,\"t\":\"%04u-%02u-%02u %02u:%02u\"}", (fi.fattrib & AM_DIR) ? "true" : "false",
                         (unsigned long)fi.fsize, 1980u + (fi.fdate >> 9), (fi.fdate >> 5) & 15u, fi.fdate & 31u,
                         (unsigned)(fi.ftime >> 11), (fi.ftime >> 5) & 63u);
                items += b;
            }
            f_closedir(&d);
        }))
        return cardBusy(r);
    if (!open)
        return sendJson(r, "404 Not Found", "{\"error\":\"That folder isn't on the card\"}");
    std::string o = "{\"path\":";
    hxparse::jsonString(o, path);
    char b[128];
    snprintf(b, sizeof(b), ",\"totalKB\":%lu,\"freeKB\":%lu,\"more\":%s,\"next\":%lu,\"items\":[", (unsigned long)(hxsd::totalBytes() >> 10),
             (unsigned long)(hxsd::freeBytes() >> 10), more ? "true" : "false", (unsigned long)(from + n));
    o += b;
    o += items;
    o += "]}";
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_send(r, o.data(), o.size());
}

// GET /api/files/get?path=: the file, 8 KB per card job, always as a download (never shown by the browser)
esp_err_t onFileGet(httpd_req_t *r)
{
    esp_err_t res;
    if (!allowed(r, false, res))
        return res;
    static char path[200], fat[208], cd[300];
    if (!cardPath(r, path, sizeof(path), fat, sizeof(fat)) || !strcmp(path, "/"))
        return notPath(r);
    FIL *f = newFile();
    uint8_t *buf = newChunk();
    bool open = false;
    if (!f || !buf || !hxsd::run([&] { open = f_open(f, fat, FA_READ) == FR_OK; })) {
        free(f);
        free(buf);
        return cardBusy(r);
    }
    if (!open) {
        free(f);
        free(buf);
        return sendJson(r, "404 Not Found", "{\"error\":\"That file isn't on the card\"}");
    }
    // the name as plain ASCII for the Save dialog (anything else becomes _)
    size_t k = snprintf(cd, sizeof(cd), "attachment; filename=\"");
    for (const char *p = hxparse::baseName(path); *p && k < sizeof(cd) - 3; p++)
        cd[k++] = (*p >= 0x20 && *p < 0x7f && *p != '"' && *p != '\\') ? *p : '_';
    cd[k++] = '"';
    cd[k] = 0;
    httpd_resp_set_type(r, "application/octet-stream");
    httpd_resp_set_hdr(r, "Content-Disposition", cd);
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    res = ESP_OK;
    for (;;) {
        UINT got = 0;
        bool ok = false;
        if (!hxsd::run([&] { ok = f_read(f, buf, 8192, &got) == FR_OK; }) || !ok) {
            res = ESP_FAIL; // cut short: the browser sees an incomplete download
            break;
        }
        if (!got)
            break;
        if (httpd_resp_send_chunk(r, (const char *)buf, got) != ESP_OK) {
            res = ESP_FAIL; // the browser went away
            break;
        }
    }
    if (!hxsd::run([&] { f_close(f); }))
        f_close(f); // a file opened for reading closes without touching the card
    free(f);
    free(buf);
    if (res == ESP_OK)
        httpd_resp_send_chunk(r, nullptr, 0);
    return res;
}

// POST /api/files/put?path=/maps/a.png[&replace=1], the file as the body: written 8 KB per card job as it arrives
esp_err_t onFilePut(httpd_req_t *r)
{
    esp_err_t res;
    if (!allowed(r, true, res))
        return res;
    static char path[200], fat[208];
    if (!cardPath(r, path, sizeof(path), fat, sizeof(fat)) || !strcmp(path, "/"))
        return notPath(r);
    if ((uint64_t)r->content_len + (1u << 20) > hxsd::freeBytes()) // and a megabyte to spare
        return sendJson(r, "507 Insufficient Storage", "{\"error\":\"There isn't room on the card for that\"}");
    const BYTE mode = FA_WRITE | (queryOf(r, "replace") ? FA_CREATE_ALWAYS : FA_CREATE_NEW);
    FIL *f = newFile();
    uint8_t *buf = newChunk();
    FRESULT fr = FR_INT_ERR;
    if (!f || !buf || !hxsd::run([&] { fr = f_open(f, fat, mode); })) {
        free(f);
        free(buf);
        return cardBusy(r);
    }
    if (fr != FR_OK) {
        free(f);
        free(buf);
        return fr == FR_EXIST ? sendJson(r, "409 Conflict", "{\"error\":\"There is already a file with that name\"}")
                              : sendJson(r, "400 Bad Request", "{\"error\":\"That file can't be made there\"}");
    }
    const char *err = nullptr;
    size_t left = r->content_len;
    int timeouts = 0;
    while (left && !err) {
        const int n = httpd_req_recv(r, (char *)buf, left < 8192 ? left : 8192);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 5)
            continue;
        if (n <= 0) {
            err = "The upload stopped part way";
            break;
        }
        timeouts = 0;
        left -= n;
        UINT wrote = 0;
        bool ok = false;
        if (!hxsd::run([&] { ok = f_write(f, buf, (UINT)n, &wrote) == FR_OK && wrote == (UINT)n; }))
            err = "The node was too busy to keep up";
        else if (!ok)
            err = "The card is full, or couldn't be written";
    }
    bool closed = false;
    for (int i = 0; i < 3 && !closed; i++) // it must be closed (its last piece is written then)
        closed = hxsd::run([&] {
            f_close(f);
            if (err)
                f_unlink(fat); // nothing half-written left behind
        });
    free(f);
    free(buf);
    if (!err && !closed)
        err = "The node was too busy to finish the file";
    if (err) {
        char b[96];
        snprintf(b, sizeof(b), "{\"error\":\"%s\"}", err);
        return sendJson(r, "400 Bad Request", b);
    }
    return sendJson(r, "200 OK", "{\"ok\":true}");
}

// POST /api/files/mkdir?path= and /api/files/delete?path= (a file, or a folder that is empty)
esp_err_t fileChange(httpd_req_t *r, bool make)
{
    esp_err_t res;
    if (!allowed(r, true, res))
        return res;
    char path[200], fat[208];
    if (!cardPath(r, path, sizeof(path), fat, sizeof(fat)) || !strcmp(path, "/"))
        return notPath(r);
    FRESULT fr = FR_INT_ERR;
    if (!hxsd::run([&] { fr = make ? f_mkdir(fat) : f_unlink(fat); }))
        return cardBusy(r);
    switch (fr) {
    case FR_OK:
        return sendJson(r, "200 OK", "{\"ok\":true}");
    case FR_EXIST:
        return sendJson(r, "409 Conflict", "{\"error\":\"There is already something with that name\"}");
    case FR_NO_FILE:
    case FR_NO_PATH:
        return sendJson(r, "404 Not Found", "{\"error\":\"That isn't on the card\"}");
    case FR_DENIED:
        return sendJson(r, "409 Conflict", make ? "{\"error\":\"The card is full\"}" : "{\"error\":\"Only an empty folder can be deleted\"}");
    default:
        return sendJson(r, "400 Bad Request", "{\"error\":\"The card couldn't do that\"}");
    }
}
esp_err_t onMkdir(httpd_req_t *r) { return fileChange(r, true); }
esp_err_t onDelete(httpd_req_t *r) { return fileChange(r, false); }

// ---- backup and restore: POST /api/backup makes one (on the card too, when there is one); POST /api/restore with a
// backup as the body brings back the parts asked for (?contacts=1&channels=1&settings=1). Both by the main loop
esp_err_t onBackup(httpd_req_t *r)
{
    headers(r);
    char x[4];
    if (sessionOf(r) < 0)
        return sendJson(r, "401 Unauthorized", "{\"error\":\"Sign in\"}");
    if (httpd_req_get_hdr_value_str(r, "X-Hoptics", x, sizeof(x)) != ESP_OK)
        return sendJson(r, "403 Forbidden", "{\"error\":\"Send it from the Hoptics page\"}");
    Query q;
    q.kind = Query::BACKUP;
    std::string out;
    if (!ask(q, out, 8000))
        return cardBusy(r);
    return sendJson(r, out.find("\"ok\"") == 1 ? "200 OK" : "500 Internal Server Error", out.c_str());
}

esp_err_t onRestore(httpd_req_t *r)
{
    headers(r);
    char x[4];
    if (sessionOf(r) < 0)
        return sendJson(r, "401 Unauthorized", "{\"error\":\"Sign in\"}");
    if (httpd_req_get_hdr_value_str(r, "X-Hoptics", x, sizeof(x)) != ESP_OK)
        return sendJson(r, "403 Forbidden", "{\"error\":\"Send it from the Hoptics page\"}");
    if (r->content_len < 2 || r->content_len > 512 * 1024)
        return sendJson(r, "400 Bad Request", "{\"error\":\"That isn't a Hoptics backup\"}");
    Query q;
    q.kind = Query::RESTORE;
    q.parts = (queryOf(r, "contacts") ? Query::CONTACTS : 0) | (queryOf(r, "channels") ? Query::CHANNELS : 0) |
              (queryOf(r, "settings") ? Query::SETTINGS : 0);
    q.len = r->content_len;
    q.data = (char *)heap_caps_malloc(q.len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!q.data)
        return sendJson(r, "503 Service Unavailable", "{\"error\":\"Out of memory\"}");
    size_t got = 0;
    int timeouts = 0;
    while (got < q.len) {
        const int n = httpd_req_recv(r, q.data + got, q.len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 5)
            continue;
        if (n <= 0)
            break;
        got += n;
    }
    if (got < q.len) {
        free(q.data);
        return sendJson(r, "400 Bad Request", "{\"error\":\"The upload stopped part way\"}");
    }
    q.data[q.len] = 0;
    std::string out;
    bool took = false;
    const bool ok = ask(q, out, 10000, &took);
    if (!took)
        free(q.data); // never reached the main loop (it frees what it takes)
    if (!ok)
        return cardBusy(r);
    return sendJson(r, out.find("\"ok\"") == 1 ? "200 OK" : "400 Bad Request", out.c_str());
}
} // namespace

bool start(uint16_t port)
{
    if (srv)
        return true;
    if (!lock) {
        lock = xSemaphoreCreateMutex();
        one = xSemaphoreCreateMutex();
        ready = xSemaphoreCreateBinary();
    }
    httpd_config_t c = HTTPD_DEFAULT_CONFIG();
    c.server_port = port;
    c.ctrl_port = 32770;
    c.stack_size = 6144;
    c.max_open_sockets = 7; // a browser opens up to 6 at once; fewer made it drop live requests (page flapping)
    c.lru_purge_enable = true;
    c.max_uri_handlers = 22;
    c.recv_wait_timeout = 10; // an update arrives over a slow link sometimes
    c.max_resp_headers = 12;
    c.core_id = tskNO_AFFINITY;
    c.task_priority = tskIDLE_PRIORITY + 3;
    if (httpd_start(&srv, &c) != ESP_OK) {
        srv = nullptr;
        hxlog::line("web page: the server did not start");
        return false;
    }
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        httpd_uri_t u = {files[i].path, HTTP_GET, onFile, (void *)(intptr_t)i};
        httpd_register_uri_handler(srv, &u);
    }
    const httpd_uri_t api[] = {{"/api/login", HTTP_POST, onLogin, nullptr},
                               {"/api/logout", HTTP_POST, onLogout, nullptr},
                               {"/api/state", HTTP_GET, onState, nullptr},
                               {"/api/log", HTTP_GET, onLog, nullptr},
                               {"/api/update", HTTP_POST, onUpdate, nullptr},
                               {"/api/tile", HTTP_POST, onTile, nullptr},
                               {"/api/send", HTTP_POST, onSend, nullptr},
                               {"/api/channel", HTTP_POST, onJoin, nullptr},
                               {"/api/tiles/clear", HTTP_POST, onTilesClear, nullptr},
                               {"/api/files", HTTP_GET, onFiles, nullptr},
                               {"/api/files/get", HTTP_GET, onFileGet, nullptr},
                               {"/api/files/put", HTTP_POST, onFilePut, nullptr},
                               {"/api/files/mkdir", HTTP_POST, onMkdir, nullptr},
                               {"/api/files/delete", HTTP_POST, onDelete, nullptr},
                               {"/api/backup", HTTP_POST, onBackup, nullptr},
                               {"/api/restore", HTTP_POST, onRestore, nullptr}};
    for (const auto &u : api)
        httpd_register_uri_handler(srv, &u);
    hxlog::line("web page on, port %u", (unsigned)port);
    return true;
}

void stop()
{
    if (!srv)
        return;
    httpd_stop(srv);
    srv = nullptr;
    hxlog::line("web page off");
}

bool running() { return srv != nullptr; }

void setPin(const char *p)
{
    if (!lock) {
        lock = xSemaphoreCreateMutex();
        one = xSemaphoreCreateMutex();
        ready = xSemaphoreCreateBinary();
    }
    const bool first = !pin[0]; // just started: the sessions kept under this same PIN come back
    bool changed = false;
    xSemaphoreTake(lock, portMAX_DELAY);
    if (strcmp(pin, p)) {
        strncpy(pin, p, sizeof(pin) - 1);
        memset(ses, 0, sizeof(ses)); // a new PIN signs everyone out
        fails = 0;
        lockUntil = 0;
        changed = true;
    }
    xSemaphoreGive(lock);
    if (!changed)
        return;
    Preferences nv;
    char toks[4][33] = {};
    bool restore = false;
    if (first && nv.begin("hxweb", true)) {
        restore = nv.getString("sespin", "") == String(p) && nv.getBytes("ses", toks, sizeof(toks)) == sizeof(toks);
        nv.end();
    }
    if (!restore) {
        if (!first)
            keepSessions(); // a new PIN: the kept sessions go too
        return;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    const uint32_t now = millis();
    for (int i = 0; i < 4; i++)
        if (strspn(toks[i], "0123456789abcdef") == 32) {
            memcpy(ses[i].tok, toks[i], 32);
            ses[i].tok[32] = 0;
            ses[i].last = now; // a fresh 12 hours from the restart
        }
    xSemaphoreGive(lock);
}

uint8_t sessions()
{
    if (!lock)
        return 0;
    uint8_t n = 0;
    xSemaphoreTake(lock, portMAX_DELAY);
    const uint32_t now = millis();
    for (auto &s : ses)
        n += s.tok[0] && now - s.last < SESSION_MS;
    xSemaphoreGive(lock);
    return n;
}

bool take(Query &q)
{
    if (!waiting || !lock) // a plain read first: the main loop asks on every pass
        return false;
    xSemaphoreTake(lock, portMAX_DELAY);
    const bool mine = waiting && !taken;
    if (mine) {
        q = pending;
        taken = true;
    }
    xSemaphoreGive(lock);
    return mine;
}

void answer(std::string &&body)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    if (reply && taken) {
        *reply = std::move(body);
        xSemaphoreGive(ready);
    }
    xSemaphoreGive(lock);
}
} // namespace hxweb

#else // a PC (the screen simulator): no server

namespace hxweb
{
bool start(uint16_t) { return false; }
void stop() {}
bool running() { return false; }
void setPin(const char *) {}
uint8_t sessions() { return 0; }
bool take(Query &) { return false; }
void answer(std::string &&) {}
} // namespace hxweb

#endif
