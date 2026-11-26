# Third-party software

Hoptics-MC is MIT-licensed (see `LICENSE`). The firmware it builds also contains:

| Component | Licence | Used for |
|---|---|---|
| [MeshCore](https://github.com/meshcore-dev/MeshCore) (Ripple Radios and contributors) | MIT | The mesh, radio, companion protocol, repeater and room server. Fetched at build time at the commit in `MESHCORE_PIN`, with the small `patches/meshcore.patch` applied. |
| [LVGL](https://github.com/lvgl/lvgl) 9.6 | MIT | The user interface toolkit and its Montserrat fonts (SIL OFL 1.1). |
| [LovyanGFX](https://github.com/lovyan03/LovyanGFX) | FreeBSD (BSD-2-Clause) | Display drivers. |
| [Noto Emoji](https://github.com/googlefonts/noto-emoji) (Google) | SIL OFL 1.1 | The emoji in messages and names (`hoptics/ui/fonts`, licence alongside). |
| [PNGdec](https://github.com/bitbank2/PNGdec) 1.1.7 (Larry Bank) | Apache-2.0 | Decoding the street map's tiles. |
| [OpenStreetMap](https://www.openstreetmap.org/copyright) map data and tiles | ODbL / CC BY-SA | The street map, downloaded on the radio while it's in view; credited on the map. |
| [RadioLib](https://github.com/jgromes/RadioLib), Arduino-ESP32, and MeshCore's own dependencies | MIT / LGPL-2.1 / Apache-2.0 | As pulled in by MeshCore. |

No code from Meshtastic, Meshtastic device-ui or WADAMESH (all GPL-3.0) is included. Hardware facts
(pin numbers, I2C addresses, key codes, the touch controller's register layout) were taken from public board
documentation and the boards' variant files in MeshCore and Meshtastic: facts only, no code.

The Hoptics visual design and the Hoptics UI code are by Brandon Lang (Bluffs), who also wrote them for
Hoptics MT (GPL-3.0); the copies here are licensed MIT by their author.
