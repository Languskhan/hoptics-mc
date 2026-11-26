// Hoptics-MC message sounds: a buzzer (M9, Heltec V4 kits) or the T-Deck's speaker, at a set volume. Notes play on a
// small background task, so nothing waits for them. MIT licence.
#pragma once
#include <stdint.h>

namespace hxsound
{
enum Tone : uint8_t { OFF = 0, CHIRP, CHIME, PING, TONE_COUNT };
void begin();                        // at start: holds a buzzer pin low (a floating one can draw current)
bool present();                       // this board can make sounds
void play(uint8_t tone, uint8_t vol); // vol 0-100; returns at once (a sound already playing is replaced)
} // namespace hxsound
