#include "hx_feed.h"
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <thread>
#include <chrono>
int main(int argc, char **argv) {
  hxfeed::start(argv[1], "HopticsMC/test");
  int secs = atoi(argv[2]);
  hxfeed::State last = hxfeed::State::OFF;
  for (int t = 0; t < secs * 10; t++) {
    hxfeed::Item it;
    while (hxfeed::pop(it)) {
      printf("ITEM len=%u raw0=%02x obs=%02x%02x name=%s snr4=%d\n", it.len, it.raw[0], it.obsKey[0], it.obsKey[1], it.obsName, it.snr4);
    }
    if (hxfeed::state() != last) { last = hxfeed::state(); printf("STATE %d err='%s'\n", (int)last, hxfeed::lastError()); }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  printf("PACKETS %u\n", hxfeed::packets());
  fflush(stdout); _exit(0);
}
