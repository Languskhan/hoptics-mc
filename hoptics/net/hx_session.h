// Hoptics-MC: how long a web sign-in has left, kept free of the Arduino core so the PC tests run the same code
// (harness/parse). millis() starts over at every boot, so a session saved to flash carries the time it has left, not
// the moment it was last used. MIT licence.
#pragma once
#include <stdint.h>

namespace hxsession
{
// ms a session last used at `last` still has at `now`, of `life`: 0 once it has run out. The subtraction is unsigned,
// so it stays right when millis() wraps
inline uint32_t remaining(uint32_t now, uint32_t last, uint32_t life)
{
    const uint32_t age = now - last;
    return age < life ? life - age : 0;
}

// a session saved with `left` ms to go, brought back at `now`: its `last`, so that it has `left` ms again. False: nothing
// left (or more than `life`, which a save never writes), so it is not brought back
inline bool restoredLast(uint32_t now, uint32_t left, uint32_t life, uint32_t &last)
{
    if (!left || left > life)
        return false;
    last = now - (life - left);
    return true;
}
} // namespace hxsession
