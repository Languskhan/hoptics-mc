// Hoptics-MC web page over HTTPS (see hx_tls.h). mbedTLS directly over esp_http_server's transport hooks, so it needs
// nothing beyond what the Arduino core already links. Its buffers go to PSRAM (the calloc hook in UITask). MIT licence.
#include "hx_tls.h"

#if defined(ARDUINO_ARCH_ESP32)
#include "hx_log.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <Preferences.h>
#include <esp_http_server.h>
#include <esp_system.h>
#include <lwip/sockets.h>
#include <mbedtls/ecp.h>
#include <mbedtls/error.h>
#include <mbedtls/oid.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <mbedtls/ssl.h>
#include <mbedtls/version.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/net_sockets.h>
#if defined(MBEDTLS_SSL_TICKET_C)
#include <mbedtls/ssl_ticket.h>
#elif defined(MBEDTLS_SSL_CACHE_C)
#include <mbedtls/ssl_cache.h>
#endif
#include <errno.h>
#include <string.h>
#include <time.h>

namespace hxtls
{
namespace
{
bool ok = false;
mbedtls_ssl_config conf;
mbedtls_x509_crt crt;
mbedtls_pk_context key;
#if defined(MBEDTLS_SSL_TICKET_C)
mbedtls_ssl_ticket_context tickets; // a browser coming back skips the slow part of the handshake
#elif defined(MBEDTLS_SSL_CACHE_C)
mbedtls_ssl_cache_context cache;
#endif
uint8_t der[640];
size_t derLen = 0;
char fp[96] = "";
Stats st;

int rng(void *, unsigned char *out, size_t n)
{
    esp_fill_random(out, n); // the hardware RNG (Wi-Fi is on whenever the page is)
    return 0;
}

// ---- the key and certificate
bool clockKnown(time_t &now)
{
    now = time(nullptr);
    return now > 1735689600; // after 2025-01-01: set from GPS, the phone or the network
}

void stamp(char out[16], time_t t)
{
    struct tm g;
    gmtime_r(&t, &g);
    strftime(out, 16, "%Y%m%d%H%M%S", &g);
}

// a new P-256 key and a self-signed certificate for <host>.local: 800 days (Apple refuses longer for a trusted server
// certificate), serverAuth, the name as a SAN (browsers ignore the CN)
bool make(const char *host, Preferences &nv)
{
    mbedtls_pk_context k;
    mbedtls_pk_init(&k);
    mbedtls_x509write_cert w;
    mbedtls_x509write_crt_init(&w);
    mbedtls_mpi serial;
    mbedtls_mpi_init(&serial);
    static uint8_t buf[1024]; // the DER is written at the end of the buffer
    bool done = false;
    do {
        if (mbedtls_pk_setup(&k, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) ||
            mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(k), rng, nullptr))
            break;
        char name[96], dn[128];
        snprintf(name, sizeof(name), "%s.local", host);
        snprintf(dn, sizeof(dn), "CN=%s,O=Hoptics", name);
        time_t now;
        if (!clockKnown(now))
            now = 1767225600; // 2026-01-01: a node without the time yet
        char from[16], to[16];
        stamp(from, now - 86400);
        stamp(to, now - 86400 + 800 * 86400);
        uint8_t sn[16];
        esp_fill_random(sn, sizeof(sn));
        sn[0] &= 0x7f; // positive
        const size_t nl = strlen(name);
        uint8_t san[4 + 96] = {0x30, (uint8_t)(nl + 2), 0x82, (uint8_t)nl}; // SEQUENCE { [2] dNSName }
        memcpy(san + 4, name, nl);
        static const uint8_t eku[] = {0x30, 0x0a, 0x06, 0x08, 0x2b, 0x06, 0x01, 0x05, 0x05, 0x07, 0x03, 0x01}; // serverAuth
        mbedtls_x509write_crt_set_version(&w, MBEDTLS_X509_CRT_VERSION_3);
        mbedtls_x509write_crt_set_md_alg(&w, MBEDTLS_MD_SHA256);
        mbedtls_x509write_crt_set_subject_key(&w, &k);
        mbedtls_x509write_crt_set_issuer_key(&w, &k);
        if (mbedtls_mpi_read_binary(&serial, sn, sizeof(sn)) || mbedtls_x509write_crt_set_serial(&w, &serial) ||
            mbedtls_x509write_crt_set_subject_name(&w, dn) || mbedtls_x509write_crt_set_issuer_name(&w, dn) ||
            mbedtls_x509write_crt_set_validity(&w, from, to) || mbedtls_x509write_crt_set_basic_constraints(&w, 0, -1) ||
            mbedtls_x509write_crt_set_key_usage(&w, MBEDTLS_X509_KU_DIGITAL_SIGNATURE) ||
            mbedtls_x509write_crt_set_extension(&w, MBEDTLS_OID_SUBJECT_ALT_NAME, MBEDTLS_OID_SIZE(MBEDTLS_OID_SUBJECT_ALT_NAME), 0,
                                                san, nl + 4) ||
            mbedtls_x509write_crt_set_extension(&w, MBEDTLS_OID_EXTENDED_KEY_USAGE, MBEDTLS_OID_SIZE(MBEDTLS_OID_EXTENDED_KEY_USAGE), 0,
                                                eku, sizeof(eku)))
            break;
        int n = mbedtls_x509write_crt_der(&w, buf, sizeof(buf), rng, nullptr);
        if (n <= 0 || (size_t)n > sizeof(der))
            break;
        nv.putBytes("crt", buf + sizeof(buf) - n, n);
        n = mbedtls_pk_write_key_der(&k, buf, sizeof(buf));
        if (n <= 0)
            break;
        nv.putBytes("key", buf + sizeof(buf) - n, n);
        memset(buf, 0, sizeof(buf));
        done = true;
    } while (false);
    mbedtls_mpi_free(&serial);
    mbedtls_x509write_crt_free(&w);
    mbedtls_pk_free(&k);
    return done;
}

// the kept certificate, if it is for this name and not about to run out
bool usable(const char *host)
{
    char want[96];
    snprintf(want, sizeof(want), "%s.local", host);
    char cn[128];
    if (mbedtls_x509_dn_gets(cn, sizeof(cn), &crt.subject) <= 0 || !strstr(cn, want))
        return false;
    time_t now;
    if (!clockKnown(now))
        return true;
    struct tm g;
    const time_t soon = now + 30 * 86400;
    gmtime_r(&soon, &g);
    const mbedtls_x509_time &t = crt.valid_to;
    return t.year > g.tm_year + 1900 || (t.year == g.tm_year + 1900 && (t.mon > g.tm_mon + 1 || (t.mon == g.tm_mon + 1 && t.day > g.tm_mday)));
}

bool load(Preferences &nv)
{
    static uint8_t k[256];
    derLen = nv.getBytes("crt", der, sizeof(der));
    const size_t kl = nv.getBytes("key", k, sizeof(k));
    mbedtls_x509_crt_free(&crt);
    mbedtls_x509_crt_init(&crt);
    mbedtls_pk_free(&key);
    mbedtls_pk_init(&key);
    if (!derLen || !kl || mbedtls_x509_crt_parse_der(&crt, der, derLen))
        return false;
#if MBEDTLS_VERSION_MAJOR >= 3
    const int r = mbedtls_pk_parse_key(&key, k, kl, nullptr, 0, rng, nullptr);
#else
    const int r = mbedtls_pk_parse_key(&key, k, kl, nullptr, 0);
#endif
    memset(k, 0, sizeof(k));
    return r == 0;
}

// ---- a session per socket
struct Sess {
    mbedtls_ssl_context ssl;
    int fd;
};

int bioSend(void *c, const unsigned char *b, size_t n)
{
    const int r = lwip_send(((Sess *)c)->fd, b, n, 0);
    if (r >= 0)
        return r;
    return errno == EAGAIN || errno == EWOULDBLOCK ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_NET_SEND_FAILED;
}
int bioRecv(void *c, unsigned char *b, size_t n)
{
    const int r = lwip_recv(((Sess *)c)->fd, b, n, 0);
    if (r >= 0)
        return r; // 0: the browser closed
    return errno == EAGAIN || errno == EWOULDBLOCK ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_NET_RECV_FAILED;
}

void freeSess(void *c)
{
    Sess *s = (Sess *)c;
    mbedtls_ssl_free(&s->ssl);
    free(s);
    if (st.open)
        st.open--;
}

bool handshakeOver(const mbedtls_ssl_context &ssl)
{
#if MBEDTLS_VERSION_NUMBER >= 0x03020000
    return mbedtls_ssl_is_handshake_over(&ssl);
#elif MBEDTLS_VERSION_MAJOR >= 3
    return ssl.MBEDTLS_PRIVATE(state) == MBEDTLS_SSL_HANDSHAKE_OVER;
#else
    return ssl.state == MBEDTLS_SSL_HANDSHAKE_OVER;
#endif
}

int onRecv(httpd_handle_t hd, int fd, char *buf, size_t len, int)
{
    Sess *s = (Sess *)httpd_sess_get_transport_ctx(hd, fd);
    if (!s)
        return HTTPD_SOCK_ERR_FAIL;
    const int r = mbedtls_ssl_read(&s->ssl, (unsigned char *)buf, len);
    if (r >= 0)
        return r;
    if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)
        return HTTPD_SOCK_ERR_TIMEOUT;
    if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
        return 0;
    return HTTPD_SOCK_ERR_FAIL;
}

int onSend(httpd_handle_t hd, int fd, const char *buf, size_t len, int)
{
    Sess *s = (Sess *)httpd_sess_get_transport_ctx(hd, fd);
    if (!s)
        return HTTPD_SOCK_ERR_FAIL;
    const int r = mbedtls_ssl_write(&s->ssl, (const unsigned char *)buf, len);
    if (r >= 0)
        return r;
    if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)
        return HTTPD_SOCK_ERR_TIMEOUT;
    return HTTPD_SOCK_ERR_FAIL;
}

int onPending(httpd_handle_t hd, int fd)
{
    Sess *s = (Sess *)httpd_sess_get_transport_ctx(hd, fd);
    return s ? (int)mbedtls_ssl_get_bytes_avail(&s->ssl) : 0;
}
} // namespace

bool begin(const char *host)
{
    if (ok)
        return true;
    Preferences nv;
    if (!nv.begin("hxtls", false))
        return false;
    const uint32_t t0 = millis();
    if (!load(nv) || !usable(host)) {
        hxlog::line("web page: making its certificate");
        if (!make(host, nv) || !load(nv)) {
            nv.end();
            hxlog::line("web page: couldn't make its certificate");
            return false;
        }
        hxlog::line("web page: certificate made in %u ms", (unsigned)(millis() - t0));
    }
    nv.end();
    uint8_t h[32];
#if MBEDTLS_VERSION_MAJOR >= 3
    mbedtls_sha256(der, derLen, h, 0);
#else
    mbedtls_sha256_ret(der, derLen, h, 0);
#endif
    for (int i = 0; i < 32; i++)
        snprintf(fp + i * 3, 4, i < 31 ? "%02X:" : "%02X", h[i]);
    mbedtls_ssl_config_init(&conf);
    if (mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) ||
        mbedtls_ssl_conf_own_cert(&conf, &crt, &key)) {
        hxlog::line("web page: TLS setup failed");
        return false;
    }
    mbedtls_ssl_conf_rng(&conf, rng, nullptr);
    // P-256 only: the curve the key is on, with mbedTLS's fast reduction for it (X25519, which browsers ask for first,
    // took far longer here)
    static const mbedtls_ecp_group_id curves[] = {MBEDTLS_ECP_DP_SECP256R1, MBEDTLS_ECP_DP_NONE};
#if MBEDTLS_VERSION_MAJOR >= 3
    static const uint16_t groups[] = {MBEDTLS_SSL_IANA_TLS_GROUP_SECP256R1, MBEDTLS_SSL_IANA_TLS_GROUP_NONE};
    mbedtls_ssl_conf_groups(&conf, groups);
    (void)curves;
#else
    mbedtls_ssl_conf_curves(&conf, curves);
#endif
#if MBEDTLS_VERSION_MAJOR < 3
    mbedtls_ssl_conf_min_version(&conf, MBEDTLS_SSL_MAJOR_VERSION_3, MBEDTLS_SSL_MINOR_VERSION_3); // TLS 1.2 and up
#endif
#if defined(MBEDTLS_SSL_TICKET_C)
    mbedtls_ssl_ticket_init(&tickets);
    if (!mbedtls_ssl_ticket_setup(&tickets, rng, nullptr, MBEDTLS_CIPHER_AES_128_GCM, 12 * 3600))
        mbedtls_ssl_conf_session_tickets_cb(&conf, mbedtls_ssl_ticket_write, mbedtls_ssl_ticket_parse, &tickets);
#elif defined(MBEDTLS_SSL_CACHE_C)
    mbedtls_ssl_cache_init(&cache);
    mbedtls_ssl_conf_session_cache(&conf, &cache, mbedtls_ssl_cache_get, mbedtls_ssl_cache_set);
#endif
    ok = true;
    hxlog::line("web page: HTTPS, fingerprint %.23s...", fp);
    return true;
}

bool ready() { return ok; }
const char *fingerprint() { return fp; }
const uint8_t *certDer(size_t &len)
{
    len = ok ? derLen : 0;
    return ok ? der : nullptr;
}

void forget()
{
    Preferences nv;
    if (nv.begin("hxtls", false)) {
        nv.clear();
        nv.end();
    }
}

Stats stats() { return st; }

int onOpen(void *server, int fd)
{
    httpd_handle_t hd = (httpd_handle_t)server;
    if (!ok)
        return ESP_FAIL;
    Sess *s = (Sess *)calloc(1, sizeof(Sess));
    if (!s)
        return ESP_ERR_NO_MEM;
    s->fd = fd;
    mbedtls_ssl_init(&s->ssl);
    int r = mbedtls_ssl_setup(&s->ssl, &conf);
    if (!r) {
        mbedtls_ssl_set_bio(&s->ssl, s, bioSend, bioRecv, nullptr);
        const uint32_t t0 = millis();
        // one step at a time, with a tick for other tasks between steps: a whole handshake is over a second of key maths
        // (several at 80 MHz, and a browser opens a few connections in a row), and without a break the idle task on
        // this core starved and the task watchdog restarted the node. The socket has the server's timeouts; a browser
        // that stalls is dropped after 10 s
        for (;;) {
            r = mbedtls_ssl_handshake_step(&s->ssl);
            if (!r && handshakeOver(s->ssl))
                break;
            if (r && r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE)
                break;
            if (millis() - t0 > 10000) {
                r = MBEDTLS_ERR_SSL_TIMEOUT;
                break;
            }
            vTaskDelay(1);
        }
        const uint32_t ms = millis() - t0;
        if (!r) {
            st.handshakes++;
            st.lastMs = ms;
            if (ms > st.maxMs)
                st.maxMs = ms;
        }
    }
    if (r) {
        st.failed++;
        st.lastErr = r;
        mbedtls_ssl_free(&s->ssl);
        free(s);
        return ESP_FAIL; // the server closes the socket
    }
    st.open++;
    httpd_sess_set_transport_ctx(hd, fd, s, freeSess);
    httpd_sess_set_recv_override(hd, fd, onRecv);
    httpd_sess_set_send_override(hd, fd, onSend);
    httpd_sess_set_pending_override(hd, fd, onPending);
    return ESP_OK;
}
} // namespace hxtls

#else // a PC: no server

namespace hxtls
{
bool begin(const char *) { return false; }
bool ready() { return false; }
const char *fingerprint() { return ""; }
const uint8_t *certDer(size_t &len)
{
    len = 0;
    return nullptr;
}
void forget() {}
Stats stats() { return Stats(); }
int onOpen(void *, int) { return -1; }
} // namespace hxtls
#endif
