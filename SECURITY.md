# Security

## Reporting a problem

Please report security problems privately: on GitHub, **Security → Report a vulnerability** on this repository.
For something minor, or already public, an ordinary issue is fine. Say what you found, how to reproduce it and
which board and version you used.

## Who can reach the radio, and how far we trust them

| From | What it can send | Trusted? |
| --- | --- | --- |
| Anyone in radio range | any LoRa packet: adverts, channel and direct messages, paths | no |
| The live feed (a CoreScope server, or anyone between it and the radio) | packets and JSON | no |
| Anyone on the same Wi-Fi | HTTP requests to the web page, sniffed traffic | no, until signed in with the PIN |
| Any website the owner visits | requests from the owner's browser to the radio's address | no |
| A signed-in browser | everything the page can do | yes, except installing firmware |
| A person holding the radio | everything | yes |

## Rules the code follows

1. **Every byte from outside is hostile.** Radio packets, feed messages, HTTP requests, backup files, files on
   the SD card and map tiles. Every length is checked against the buffer before a copy, and counted so that it
   can't wrap (`len > MAX - have`, never `have + len > MAX`).
2. **No undefined behaviour on bad input.** Numbers from JSON are range-checked before they're cast
   (`hxparse::integer`); hex digits are checked before they're shifted; a float is checked before it's cast to an
   integer.
3. **Every allocation is checked.** When memory runs out the feature stays off and says why; nothing writes
   through a null pointer.
4. **Signatures before trust.** An advert's name and position are shown only after its Ed25519 signature checks
   out; a check that is too costly to make (a flood of adverts) shows the advert without them.
5. **Encryption is never switched off from outside.** The feed never follows a redirect from `wss://` to `ws://`;
   TLS connections check certificates against the roots built in (`hx_feed_roots.h`, `hx_map_roots.h`).
6. **The web page:** every endpoint that reads or changes anything checks the session; every one that changes
   something also needs the `X-Hoptics` header, which other websites can't send. Sign-in checks the `Host`
   header (DNS rebinding), counts wrong PINs per address, and compares PINs and tokens in constant time. The
   page renders everything from the radio or the internet with `textContent`, never as HTML, under a strict
   Content-Security-Policy that lets it connect only to the node, the configured feed server and the terrain
   service.
7. **Secrets stay on the radio.** The node's private key never leaves it; private channels' keys leave only in a
   backup the owner asked for with keys included. Logs hold no names, positions or messages.
8. **Firmware installs only after a tap on the radio's screen**, starts on trial and goes back by itself if it
   doesn't start properly.
9. **Fail closed.** When a check can't be made, the answer is no.

## Known limits

- The web page is plain HTTP: anyone on the same Wi-Fi can read the PIN as it's sent, and a session cookie
  after it (issue #5; an HTTPS mode is being worked on). Use it only on a network you trust.
- Packets from the live feed that only the web page receives (straight from the feed server to the browser) are
  marked "unverified": the page can't check signatures without HTTPS.
- Firmware images aren't signed: the tap on the screen is what stops an unwanted update.

## Checks on every pull request

cppcheck; the parsers under ASan/UBSan (`harness/parse`, `harness/feed`); the same parsers fuzzed with libFuzzer
(`harness/fuzz/run.sh`); the web page's crypto against Node's (`harness/web`). New code that reads outside input
gets a test there, including the hostile cases.
