#pragma once
#include "WiFi.h"
class WiFiClientSecure : public WiFiClient { public: void setCACert(const char *); void setHandshakeTimeout(unsigned long); int lastError(char *buf, const size_t size); };
