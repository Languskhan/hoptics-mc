// Hoptics-MC log keeper. Every "hoptics:" line goes to USB as before and into a log kept on the radio, so a tester can
// send what happened even when nobody was watching: the radio keeps this session and the two before it, prints the
// last one at each start, reports a crash it finds (task, address, backtrace) and prints all of it on request
// (Settings, Device log). No messages, names, positions, network names or passwords go in. MIT licence.
#pragma once
#include <string>
#include <stdint.h>

namespace hxlog
{
void line(const char *fmt, ...) __attribute__((format(printf, 1, 2))); // USB and the kept log ("hoptics: " is added)
void usb(const char *fmt, ...) __attribute__((format(printf, 1, 2)));  // USB only (the 10-second heartbeat)
void tick(void);  // main loop: the first call turns over the kept logs and reports a crash; then writes to flash each minute
void save(void);  // write what is new to flash now (before a restart)
void dump(void);  // print the kept logs to USB
void text(std::string &out); // the same, as text (the web page's "Download the device log")
} // namespace hxlog
