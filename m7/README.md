# Hoptics M7

Firmware for the Elecrow ThinkNode M7 (ESP32-S3, LR1110, Ethernet + PoE): a MeshCore observer and repeater
with the Hoptics dashboard at `http://<node-ip>/hoptics`.

## What it adds

- Dashboard: overview with repeater health (0-100), graphs you can click into, live map, neighbours,
  packets with full decoding, insights, MQTT status, settings advisor and console
- Mesh-impact warnings based on what the node hears at its spot
- 30 days of 5-minute history, kept on flash so it survives restarts and updates
- Role switching (observer, repeater, room server, companion, KISS) and one-click updates
- Discord alerts (optional): quiet repeaters, daily summary, new nodes
- Chat tab: read and send Public and hashtag channel messages as this node; tap a name or type @ to mention someone (`@[Name]`, like the apps)
- Direct messages (off until you turn them on): a second chat identity beside the repeater that people add from its advert; DMs with delivery receipts, direct paths and retries, up to 16 saved contacts
- Regional map: point it at your area's CoreScope analyzer on the System tab (off by default) to name repeaters on the map and in packet paths
- Overview links to this node on your CoreScope and the MeshCore map (LetsMesh and MeshMapper open their main pages)
- Discord alert when the spot changes from one day to the next: more repeaters heard well, a busier channel, or a louder noise floor
- Hold the button 5 s for the setup hotspot (open Wi-Fi, ends after 10 idle minutes); Ethernet first with Wi-Fi as backup comes from the base firmware
- Status LED (green): short blip every 3 s online, slow blink offline, fast blink while the setup hotspot is on, solid while the button is held
- Trace route on the Neighbours tab: out through the repeaters you list and back, with the signal at every hop
- Settings backup and restore on the System tab (no passwords, tokens or keys in the file)
- Room posts on the Chat tab in the room roles: read the last 32 posts and post as the room
- Remote admin on the Neighbours tab: sign in to another repeater or room with its password (sent once, never stored) and run commands, with quick buttons for status and stats
- Path ID size on the System tab: 1, 2 or 3-byte IDs in this node's adverts (MeshCore 1.14+), kept in settings backups
- Insights: "Who carries this node", the repeaters that pick up this node's own floods, from the echoes it hears back
- Mesh check: warns when this node's short ID is shared by a repeater on the regional map
- Graphs: download the 30-day history as CSV
- Discord alert when the chip runs hot (80 C) and when it cools down
- Viewer access: a second, read-only password (System tab) so others can watch stats, graphs, map, packets and public channels
- Update channel: Stable (main) or Beta (the beta branch) in System → Updates
- Alerts can go to an ntfy topic (ntfy.sh or your own server) as well as a Discord webhook
- Scheduled weekly restart, set in local time on the System tab (never within 2 hours of starting)
- Stats feed for Home Assistant / Prometheus (read-only token); automatic rollback after a bad update
- Web page always on while the network is up; advert at boot

## How it is built

`.github/workflows/m7.yml` fetches the observer firmware at the commit in `OBSERVER_PIN`, applies
`patches/`, copies `src/` and gzips `web/hoptics.html` into the firmware. Images land on the `m7-builds` branch.

## Credits

Built on [agessaman's MeshCore observer firmware](https://github.com/agessaman/MeshCore) (MIT): the MQTT
uploader, broker presets, Ethernet support and settings page are theirs. MeshCore itself is by Scott Powell
and contributors (MIT).
