#pragma once
#include "Arduino.h"
enum { WL_CONNECTED = 3 };
struct WiFiClass { int status(); }; extern WiFiClass WiFi;
class WiFiClient : public Client { public:
  int connect(const char *host, uint16_t port) override; size_t write(const uint8_t *buf, size_t size) override;
  int available() override; int read(uint8_t *buf, size_t size) override; void stop() override; uint8_t connected() override; };
