# Hoptics-MC

The Hoptics touch interface on **MeshCore**: the same look as [Hoptics MT](https://github.com/Languskhan/hoptics-mt)
(Meshtastic), with a MeshCore-style layout. Works with the MeshCore phone and web apps.

**Status: public beta (0.2.0-beta). The companion runs on all four boards below.** Testing it? See
[docs/TESTING.md](docs/TESTING.md): installing, updating, what to try, and how to send a log when something goes wrong.

| Board | Companion | Repeater | Room server | Observer |
|---|---|---|---|---|
| LilyGO T-Deck / T-Deck Plus | beta | planned | planned | planned |
| Elecrow ThinkNode M9 | beta | planned | planned | planned |
| Heltec V4 + touch kit | beta | planned | planned | planned |
| Heltec V4-R8 + Expansion Kit V2 | beta | planned | planned | planned |

Works anywhere MeshCore does: pick your country's radio band and your mesh's settings under More → Radio.
Download the firmware for your board from the [latest release](https://github.com/Languskhan/hoptics-mc/releases/latest).

## What it does (companion)

- **Home**: what the mesh is doing at a glance; tap any number for the full detail (heard, repeaters, packets
  per minute, noise floor, this radio).
- **Map**: nodes at their positions, each packet animated along its route, links learned from the paths.
- **Live**: every packet heard or sent. **Nodes** and **Chats**: contacts, channels and direct messages, with emoji. Tap or hold a message for its details (time, repeaters, signal, delivery) and to reply, message the sender privately, see their node, send again or delete.
- **Manage a repeater or room server**: log in with its admin password (kept on the radio if you like), then
  its status and telemetry, the nodes it hears, every setting its command line has (name, position, radio, a
  10-minute radio trial, TX power, repeating and hop limits, loop detection, advert intervals, timing,
  passwords, who has access), the actions (adverts, clock sync, reset counters, restart) and its command line.
- **Wi-Fi** (optional): a street map under the live map (OpenStreetMap, saved for offline use) and the clock.
- **Live feed** (optional, off by default): over Wi-Fi, what your region's observers hear, from a CoreScope
  server's public live feed. The radio decodes those packets with its own channel keys, so your channels keep
  coming in when the radio can't hear the mesh, the map draws routes across the whole region, and a channel
  message you send says how many observers heard it. Receive only: nothing is sent over the internet.
- **Web page** (optional, off by default): over Wi-Fi the radio serves a dashboard to phones and computers on the
  same network, signed in with a 6-digit PIN shown on the radio. Live traffic, a street map with live routes,
  channel messages, charts (traffic, signal, packet types, hops, busiest repeaters, noise, battery, memory), a
  line-of-sight tool with the terrain profile, Fresnel zone and link margin between any two points, and the
  device log. The browser connects to the CoreScope feed itself, so that costs the radio nothing. Direct
  messages and private channels' keys never go to the page. From the page you can also send channel
  messages, install a firmware update (it starts on trial and goes back by itself if it doesn't start
  properly), and send a folder of offline map tiles for the radio's map.
  **The page is plain HTTP, not encrypted:** use it only on a network you trust. On shared Wi-Fi (a club
  meeting, a hamfest, a café, a hotel) anyone nearby could read the PIN as you sign in. See [Security](#security).
- **microSD card** (T-Deck Plus, ThinkNode M9, V4-R8 Kit V2; FAT32): the street map keeps its tiles on the card
  (up to 1 GB) and shows ready-made map packs from it (Meshtastic's `maps/<style>/z/x/y.png` or `map/`, MeshCore's
  `tiles/`); the web page gets a **Files** tab (browse, download, upload, new folder, delete) and a **Backup** of
  the settings, channels and contacts (kept on the card, and brought back from there or from a file); the device
  log is kept on the card too. Without a card everything works as before.
- **Settings** (the More tab): quick switches for Wi-Fi, Bluetooth, GPS and sound (tap to flip, hold for
  their page), then six categories, each showing what it's set to: **Radio** (presets or a custom radio, asked
  before they change anything; path hash size; region), **Messages** (auto-add rules, telemetry privacy),
  **Connect** (Bluetooth PIN, Wi-Fi, web page, live feed), **Display** (themes, wallpapers, a lock screen, large
  chat text, message sounds with a volume setting), **Location** (GPS, the street map) and **System** (time zone,
  restart, device log, about). Back, Left, or More again returns to the six.

## Security

What strangers can send the radio (packets over the air, the live feed, a browser on the same Wi-Fi) is treated
as hostile; [SECURITY.md](SECURITY.md) has the rules the code follows and how to report a problem. In short:

- **The web page isn't encrypted.** Anyone on the same Wi-Fi can read what crosses it, including the PIN as you
  sign in. Use it at home or on a network you trust. Backups leave private channels' keys out unless you tick
  "Include private channel keys"; restoring a backup without keys keeps the keys already on the node.
- **Wrong PINs lock out only the device that sent them:** five tries, then a minute, doubling up to 15 minutes.
  If 50 wrong PINs arrive within an hour from any mix of devices, sign-in locks for everyone for 15 minutes.
  **Making a new PIN on the radio clears every lock** and signs every browser out.
- **A firmware update from the page installs only after a tap on the radio's own screen.**
- **Names and positions on the map come from signed adverts.** The radio checks each advert's signature before it
  shows a node's name or place, for packets it hears and for the live feed. The web page can't check the adverts
  it gets straight from the feed, so a node seen only that way is marked "unverified".

## How it's built

MeshCore itself is not copied here. The build fetches MeshCore at the commit in `MESHCORE_PIN`, applies the
small `patches/meshcore.patch` (two packet-log hooks for the Live view, access for the on-screen companion
to the same functions the app commands use, and one ThinkNode M9 pin name that clashed with a library), adds the `hoptics/` folder and builds the environments in
`platformio.local.ini`.

- `hoptics/ui/`: the screens (LVGL 9), written against `hx::Model` so they run with demo data on a PC
  (`harness/`) and on every role.
- `hoptics/hal/`: display, touch, keyboard and backlight per board (LovyanGFX).
- `hoptics/net/`: the live feed's connection (a small WebSocket client) and the web page's server.
- `hoptics/web/`: the web page (plain HTML, CSS and JavaScript, no libraries). `tools/make_web.py` gzips it into
  the firmware (the build does this); `tools/make_web.py --preview demo.html` makes a stand-alone demo of it.
- `hoptics/roles/`: one adapter per MeshCore role (companion first).

Every push also runs quick checks on a PC beside the firmware builds: cppcheck on `hoptics/`, the web page's
SHA-256, HMAC, AES and packet decoder against Node's crypto (`node harness/web/core_test.js`), the feed parser
and WebSocket client under ASan/UBSan (`harness/feed/run.sh`), the web server's card paths and JSON reading
(`harness/parse/run.sh`), both parsers fuzzed with libFuzzer (`harness/fuzz/run.sh`), and every screen in every theme rendered by
`harness/build_and_run.sh` (kept as the run's `screens` artifact), with the web page's JSON checked.

## Licence

MIT, see `LICENSE`. Third-party components are listed in `NOTICE.md`. No Meshtastic or WADAMESH (GPL) code is
included.
