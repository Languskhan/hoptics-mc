#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
unsigned long millis();
unsigned long micros();
struct HardwareSerial { int printf(const char *, ...); };
extern HardwareSerial Serial;
class Client { public:
  virtual int connect(const char *host, uint16_t port) = 0;
  virtual size_t write(const uint8_t *buf, size_t size) = 0;
  virtual int available() = 0;
  virtual int read(uint8_t *buf, size_t size) = 0;
  virtual void stop() = 0;
  virtual uint8_t connected() = 0;
  virtual ~Client() {}
};
