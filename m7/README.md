# Hoptics M7

Firmware for the Elecrow ThinkNode M7 (ESP32-S3, LR1110, Ethernet + PoE): a MeshCore observer and repeater
with the Hoptics dashboard at `http://<node-ip>/hoptics`.

## What it adds

- Dashboard: overview with repeater health (0-100), graphs you can click into, live map, neighbours,
  packets with full decoding, insights, MQTT status, settings advisor and console
- Mesh-impact warnings based on what the node hears at its spot
- 30 days of 5-minute history in PSRAM
- Web page always on while the network is up; advert at boot

## How it is built

`.github/workflows/m7.yml` fetches the observer firmware at the commit in `OBSERVER_PIN`, applies
`patches/`, copies `src/` and gzips `web/hoptics.html` into the firmware. Images land on the `m7-builds` branch.

## Credits

Built on [agessaman's MeshCore observer firmware](https://github.com/agessaman/MeshCore) (MIT): the MQTT
uploader, broker presets, Ethernet support and settings page are theirs. MeshCore itself is by Scott Powell
and contributors (MIT).
