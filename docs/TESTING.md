# Testing Hoptics-MC

Hoptics-MC is in public beta. Thank you for trying it. This page covers installing it, updating it, going back to stock
MeshCore, what to try, and how to tell us when something is wrong.

## Boards

| Board | Screen | Typing |
|---|---|---|
| LilyGO T-Deck / T-Deck Plus | landscape, touch | keyboard and trackball |
| Elecrow ThinkNode M9 | landscape | keys (see below) |
| Heltec V4 Expansion Kit | portrait, touch | on-screen keyboard (tap a text field) |
| Heltec V4-R8 Expansion Kit V2 | portrait, touch | on-screen keyboard (tap a text field) |

Use Chrome or Edge on a computer, and a USB data cable (some cables only charge).

## First install

Open the Hoptics-MC page of the [Hoptics website](https://languskhan.github.io/hoptics-mt/mc-test/) in Chrome or
Edge, plug the radio in, pick your board, choose **Install** and let it **erase** the radio. (Without the website:
flash the `.factory.bin` for your board from the [latest release](https://github.com/Languskhan/hoptics-mc/releases/latest)
at address 0x0 with the [ESP web tool](https://espressif.github.io/esptool-js/), erasing first.) Erasing removes its MeshCore identity (keys), contacts, channels
and settings, and it starts as a new node. To keep an existing MeshCore identity, export its private key in the
MeshCore app first and import it afterwards.

If the browser doesn't see the radio, put it in download mode: T-Deck, hold the trackball down while you plug it
in; Heltec, hold the BOOT button while you plug it in.

After it starts:

1. **More → Radio**: pick your country's radio band, then your mesh's preset (Sacramento / Foothills is listed as
   **SMF**). Your mesh not listed? Ask its people for frequency, bandwidth and SF, and enter them under Custom radio.
2. **More → Screen**: set your time zone.
3. **More → Phone app**: pair the MeshCore app with the PIN shown there, if you use it.

## Updating

On the website choose **Update** (it writes the app only), or, with the web page on, use Web page → Device →
Firmware update with the `.update.bin` from the release. Your keys, contacts, channels, chat history, saved repeater passwords
and screen settings all stay.

## Going back to stock MeshCore

Flash any firmware from [flasher.meshcore.io](https://flasher.meshcore.io), and let it erase when it asks, so the
radio starts clean.

## ThinkNode M9 keys

Arrows move, OK opens, Back goes back. From the left on the top row:

- **Message bubble**: Chats.
- **House**: Home.
- **Second bubble**: the Public channel, ready to type.
- **Map**: the live map.
- **GPS pin**: send an advert to your neighbours. Hold it to send one across the whole mesh.

On the T-Deck and the M9, Left from a page moves onto the menu down the left side; Up and Down pick a page.
On the Heltec kits, the user button is Back.

## What to try

- Send and receive in a channel and a direct message, with emoji. Restart the radio and check that the chats
  are still there.
- Tap each number on Home for its detail screen.
- **Map**: with a GPS fix (or a position set in the app), watch packets travel along their routes.
- **Nodes → a repeater you run → Manage**: log in with its admin password. Check its status, the nodes it hears,
  read its settings, and send `ver` from the console. Change something harmless, such as the advert interval,
  and change it back.
- **More → Wi-Fi**: find your network and connect. Then look at the map: the streets fill in as you move around.
- **More → Live feed** (needs Wi-Fi): turn it on and watch the map draw routes across the region. Messages on
  your channels that only the observers heard show "via internet". Send on a channel and the bubble says how
  many observers heard it.
- **More → Web page** (needs Wi-Fi): turn it on, open the address it shows on a phone or computer on the same
  Wi-Fi, and sign in with the PIN. Try each tab, the line-of-sight tool between your radio and a repeater, and
  "Download the device log". Press **New PIN** on the radio and check the browser has to sign in again.
- **Web page → Messages**: send on a channel from the browser. It goes out from the radio and shows in the
  radio's own chat too.
- **Web page → Device → Firmware update**: choose the newest `.update.bin` for your board and install it. The
  radio restarts into it "on trial" and keeps it after a healthy minute; if it doesn't start properly it goes
  back to the build it had. Updating over USB afterwards works as before.
- **Web page → Device → Offline map tiles**: send a folder of `zoom/x/y.png` tiles (for example from Mobile
  Atlas Creator, from a tile source that allows keeping tiles offline) and check the radio's map shows them
  with Wi-Fi off.
- **More → Sound**: pick a message sound and set the volume (each change plays it). Have someone send you a
  message: it should chirp. The T-Deck plays through its speaker; the M9 and the Heltec kits use their buzzer.
- **More → Screen**: try a wallpaper, the lock screen (it shows when the screen comes back on; a key or a long
  press opens it) and Large chat text.
- **More**: change the theme, brightness and screen timeout. Restart and check they stay.
- Leave it running overnight on USB.

## Reporting a problem

Say which board, what you did, and what happened. A photo of the screen helps. Then send its log:

1. Plug the radio into a computer and open the install page in Chrome or Edge.
2. Press **Connect**, pick the radio, then **Logs & Console**.
3. On the radio, **More → Device log → Send the log to USB**. (Or press **Reset device** on the page: the radio
   prints the last session's log each time it starts.)
4. Press **Download logs** and send the file.

With the web page on, **Device → Download the device log** in the browser gets the same file without a cable.

The radio keeps its last three sessions on its own flash, so this works even if the problem happened hours ago or
the radio restarted since. After a crash, the next start adds where it crashed (task, address and backtrace).
The log has no messages, names, positions, network names or passwords in it.

What the lines mean:

- `start 8/8 running` means it started; a start that stops early names the step, and the next start says
  `the last start stopped after '...'`.
- Every 10 seconds (every 5 minutes in the kept log) it prints how long it has been up, its free memory and how the
  GPS is doing. If the free memory keeps falling over hours, that is worth reporting.

## Known limits

- Emoji are drawn in one colour (the text colour).
- The Heltec V4 Expansion Kit has 2 MB of PSRAM; very busy meshes are better on the V4-R8, T-Deck or M9.
- The live feed only shows what the region's observers hear, a few seconds late, and only on the radio's screen
  (not in the phone app). It is receive only.
- Only the companion role so far: repeater, room server and observer builds are planned.
