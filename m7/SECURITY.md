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
- **Admin only.** Every data endpoint and the firmware upload need the admin session (5 wrong passwords lock
  login for a while; the session cookie is HttpOnly and SameSite=Lax).
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

## Limits to know

- The page is plain HTTP on your LAN, like most device setup pages: someone on the same network could see the
  password when you sign in. Use it on a network you trust, and **never forward port 80 to the internet**.
- Firmware images are not cryptographically signed; the admin password is what protects updates.

Report problems through GitHub issues.
