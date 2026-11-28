# Hoptics M7 security

The node is a radio on your home network. Everything it receives (LoRa packets, web requests, MQTT) is treated
as hostile input.

## Rules this code follows

- **Radio input is bounded.** Path copies stop at the packet's path buffer; names are length-capped; packet bytes
  are stored with their real length. Nothing is parsed past what the packet says it holds.
- **Text from the air is never trusted as HTML.** Node names, channel names and decoded messages are escaped
  before they reach the page. The page's Content Security Policy only lets it talk to the node itself
  (`connect-src 'self'`), so even a page bug can't send your data elsewhere.
- **Third-party code is pinned.** The map library is loaded with Subresource Integrity hashes; a changed file
  is refused by the browser.
- **Admin only.** Every data endpoint and the firmware upload need the admin session. The session cookie is
  HttpOnly and SameSite=Lax and expires after 20 minutes unused.
- **Password guessing is slowed down.** 5 wrong passwords lock sign-in for 30 s, and each further lockout
  doubles the wait (up to about an hour). The web page refuses to sign in at all while the admin password is a
  default (`password`, `hello`) or shorter than 6 characters.
- **No DNS rebinding.** On your network the node only answers requests addressed to its IP or its own
  `meshcore-…` name, so a website can't point its own domain at the node and talk to it.
- **Firmware updates fail closed.** Only the signed-in admin, from the node's own page (Origin checked), one
  upload at a time; the file must be an ESP32-S3 app image; it goes into the spare slot and the node switches
  only if the whole image arrived and its checksum verified. A dropped upload is discarded.
- **Replies are size-capped.** Packet and history lists come in pages so no request can make the node build a
  huge reply in RAM.
- **Responses** carry `X-Frame-Options: DENY`, `X-Content-Type-Options: nosniff` and `Referrer-Policy: no-referrer`.

- **Outbound data is verified.** The region node list comes from `https://scope.sacmesh.org` over TLS checked
  against the built-in CA bundle; redirects are refused, each page is parsed through a filter with a nesting
  limit, keys and coordinates are validated, names are cut to fit, and the number of pages is capped. A failed
  refresh keeps the last good list.

- **Discord alerts can't be turned against you.** Only `https://discord.com/api/webhooks/…` links are
  accepted (the node can't be pointed at another server). The link is a secret: the page only ever sees its
  last 4 characters. Sending is TLS-verified with redirects refused, limited to one message a minute and 40 a
  day, and node names are stripped of Discord markup with mentions disabled, so a name can't ping anyone.

- **Chat is admin only and paced.** Sending needs the admin session from the node's own page; messages are
  1-140 bytes with no control characters, one at a time and at least 5 s apart. Channel keys are added in the
  browser and sent only with the message; the node doesn't store them.

- **Direct messages are off by default and kept apart from the repeater.** The chat identity has its own key (in
  NVS, never sent to the page, never the repeater's first key byte). It only decrypts packets addressed to it from
  contacts whose adverts it heard or the owner saved, answers with the companion firmware's ACK and path replies,
  and never changes forwarding. Incoming text is stripped of control characters and escaped by the page. Sending is
  admin-only, from the node's own page, one message at a time and at least 5 s apart; adverts at most once a minute.
  Contacts heard over the air are kept in memory only; saved contacts (16 at most) are written to flash only when
  the owner changes them.
- **Bad updates roll back.** After an update the node notes the slot it came from; if the new firmware can't
  stay up and online for 3 minutes three starts in a row, it switches back to the previous one.
- **The stats feed is read-only and separate.** It is off until you turn it on; it uses a random 32-character
  token (compared in constant time) that can only read numbers, never change anything, and can be replaced or
  turned off at any time. It carries no names or messages.

## Limits to know

- The page is plain HTTP on your LAN, like most device setup pages: someone on the same network could see the
  password when you sign in. Use it on a network you trust, and **never forward port 80 to the internet**.
- Firmware images are not cryptographically signed; the admin password is what protects updates.

Report problems through GitHub issues.
- **The setup hotspot gets none of the Hoptics data.** Holding the button 5 s starts the base firmware's setup
  hotspot, which is open and skips sign-in (hands on the box, like USB; it ends after 10 idle minutes). Every
  Hoptics route answers only in LAN mode, so messages, packets and alert settings are never served there.
- **Backups hold no secrets.** The page reads an allowlist of settings and leaves out passwords, MQTT tokens, the
  SNMP community and keys; a restore goes through the base firmware's allowlisted settings API.
- **Trace route and room posts are admin only and paced** like chat: one trace at a time (10 s apart), one post at
  a time (5 s apart).
