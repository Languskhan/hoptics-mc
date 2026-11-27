// Hoptics-MC web page (see hx_web.h). MIT licence.
#include "hx_web.h"
#include "hx_log.h"
#include "hx_ota.h"
#include "map/hx_tiles.h"

#if defined(ARDUINO_ARCH_ESP32)
#include "hx_web_files.h"
#include <Arduino.h>
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
    char c[256];
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

// hands a query to the main loop and waits for its answer
bool ask(const Query &q, std::string &out)
{
    xSemaphoreTake(one, portMAX_DELAY);
    xSemaphoreTake(ready, 0); // a late answer to a request that gave up
    xSemaphoreTake(lock, portMAX_DELAY);
    pending = q;
    reply = &out;
    waiting = true;
    taken = false;
    xSemaphoreGive(lock);
    const bool ok = xSemaphoreTake(ready, pdMS_TO_TICKS(1500)) == pdTRUE;
    xSemaphoreTake(lock, portMAX_DELAY);
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
bool jsonText(const char *p, char *out, size_t n) // the JSON string at p (just after its opening quote)
{
    size_t k = 0;
    while (*p && *p != '"') {
        uint32_t c = (uint8_t)*p++;
        if (c == '\\') {
            const char e = *p++;
            if (e == 'n' || e == 'r' || e == 't')
                c = ' ';
            else if (e == 'u') {
                char h[5] = {};
                for (int i = 0; i < 4 && *p; i++)
                    h[i] = *p++;
                c = strtoul(h, nullptr, 16);
                if (c >= 0xD800 && c < 0xDC00 && p[0] == '\\' && p[1] == 'u') { // a surrogate pair (emoji)
                    char l[5] = {};
                    for (int i = 0; i < 4 && p[2 + i]; i++)
                        l[i] = p[2 + i];
                    const uint32_t lo = strtoul(l, nullptr, 16);
                    if (lo >= 0xDC00 && lo < 0xE000) {
                        c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                        p += 6;
                    }
                }
                if (c < 0x20)
                    c = ' ';
                char u[4];
                int m = c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
                if (m == 1) u[0] = (char)c;
                else if (m == 2) u[0] = (char)(0xC0 | c >> 6), u[1] = (char)(0x80 | (c & 63));
                else if (m == 3) u[0] = (char)(0xE0 | c >> 12), u[1] = (char)(0x80 | ((c >> 6) & 63)), u[2] = (char)(0x80 | (c & 63));
                else u[0] = (char)(0xF0 | c >> 18), u[1] = (char)(0x80 | ((c >> 12) & 63)), u[2] = (char)(0x80 | ((c >> 6) & 63)), u[3] = (char)(0x80 | (c & 63));
                if (k + m >= n)
                    return false;
                memcpy(out + k, u, m);
                k += m;
                continue;
            } else if (e)
                c = (uint8_t)e;
            else
                return false;
        }
        if (k + 1 >= n)
            return false;
        out[k++] = (char)c;
    }
    out[k] = 0;
    return *p == '"';
}

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
    q.channel = (uint8_t)atoi(c);
    t += 6;
    t += strspn(t, " :");
    if (*t != '"' || !jsonText(t + 1, q.text, sizeof(q.text)) || !q.text[0])
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
    if (*t != '"' || !jsonText(t + 1, q.text, 32) || !q.text[0])
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
    c.max_open_sockets = 4;
    c.lru_purge_enable = true;
    c.max_uri_handlers = 14;
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
                               {"/api/tiles/clear", HTTP_POST, onTilesClear, nullptr}};
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
