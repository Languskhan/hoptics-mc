// host versions of the ESP32 calls the feed uses: real sockets, threads, mutexes
#include "stubs/Arduino.h"
#include "stubs/WiFi.h"
#include "stubs/WiFiClientSecure.h"
#include "stubs/esp_heap_caps.h"
#include "stubs/esp_system.h"
#include "stubs/freertos/semphr.h"
#include "stubs/freertos/task.h"
#include <chrono>
#include <cstdarg>
#include <mutex>
#include <netdb.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
static auto t0 = std::chrono::steady_clock::now();
unsigned long micros() { return (unsigned long)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count(); }
unsigned long millis() { return (unsigned long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count(); }
HardwareSerial Serial;
int HardwareSerial::printf(const char *f, ...) { va_list a; va_start(a, f); int n = vprintf(f, a); va_end(a); return n; }
WiFiClass WiFi;
int WiFiClass::status() { return WL_CONNECTED; }
static thread_local int fd = -1; // one client per thread (the feed task)
int WiFiClient::connect(const char *host, uint16_t port) {
  addrinfo *r; char ps[8]; snprintf(ps, 8, "%u", port);
  if (getaddrinfo(host, ps, nullptr, &r)) return 0;
  fd = socket(r->ai_family, SOCK_STREAM, 0);
  int ok = ::connect(fd, r->ai_addr, r->ai_addrlen) == 0; freeaddrinfo(r); return ok; }
size_t WiFiClient::write(const uint8_t *b, size_t n) { return send(fd, b, n, MSG_NOSIGNAL); }
int WiFiClient::available() { int n = 0; ioctl(fd, FIONREAD, &n); return n; }
int WiFiClient::read(uint8_t *b, size_t n) { return (int)recv(fd, b, n, 0); }
void WiFiClient::stop() { if (fd >= 0) close(fd); fd = -1; }
uint8_t WiFiClient::connected() { if (fd < 0) return 0; char c; int r = recv(fd, &c, 1, MSG_PEEK | MSG_DONTWAIT); return r > 0 || (r < 0 && errno == EAGAIN); }
void WiFiClientSecure::setCACert(const char *) {} void WiFiClientSecure::setHandshakeTimeout(unsigned long) {}
int WiFiClientSecure::lastError(char *b, size_t n) { snprintf(b, n, "tls"); return -1; }
void *heap_caps_malloc(size_t n, unsigned) { return malloc(n); } size_t heap_caps_get_free_size(unsigned) { return 100000; }
uint32_t esp_random(void) { return (uint32_t)rand(); }
void esp_fill_random(void *b, size_t n) { for (size_t i = 0; i < n; i++) ((uint8_t *)b)[i] = rand(); }
SemaphoreHandle_t xSemaphoreCreateMutex() { return new std::mutex; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t m, TickType_t) { ((std::mutex *)m)->lock(); return 1; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t m) { ((std::mutex *)m)->unlock(); return 1; }
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t f, const char *, uint32_t, void *p, UBaseType_t, TaskHandle_t *, BaseType_t) { std::thread(f, p).detach(); return 1; }
void vTaskDelay(TickType_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t) { return 0; }
// the log keeper prints to USB here
#include "hx_log.h"
void hxlog::line(const char *fmt, ...) { va_list a; va_start(a, fmt); printf("hoptics: "); vprintf(fmt, a); printf("\n"); va_end(a); }
