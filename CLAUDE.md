# Hoptics-MC: notes for Claude

Hoptics-MC is a colour UI and web dashboard on top of MeshCore's companion firmware (see README.md for the layout).
Owner: Languskhan. Contributors such as denmojo review for security; take their findings seriously.

## Security comes first

Read [SECURITY.md](SECURITY.md) before changing anything that reads outside input, and follow its rules. In short:

- Treat every byte from the radio, the live feed, Wi-Fi, a browser, a backup file or the SD card as hostile.
- Check every length before a copy, in a form that can't wrap. Check every allocation. Range-check numbers before
  casting them (`hxparse::integer`). No undefined behaviour on any input.
- New web endpoints: check the session (`sessionOf` / `allowed`), require `X-Hoptics` for anything that changes
  state, and never send private keys unless the owner explicitly asks.
- Render outside text in the page with `textContent` only. Keep the CSP strict.
- Verify signatures before trusting names, positions or identities.
- Never weaken TLS: no `setInsecure()`, no following a redirect to plain `ws://`.
- When in doubt, fail closed.

## Before opening a PR

Run what CI runs and add a test for each new input path, including the hostile cases:

```
bash harness/parse/run.sh          # web server text helpers, ASan/UBSan
bash harness/feed/run.sh           # feed parser and WebSocket client
bash harness/fuzz/run.sh 60        # libFuzzer (needs clang with compiler-rt)
node harness/web/core_test.js      # the page's crypto and decoder
for f in hoptics/web/*.js; do node --check "$f"; done
```

Firmware builds run in CI (`.github/workflows/build.yml`) against the MeshCore commit in `MESHCORE_PIN`.

## Commits

Commits are authored by Languskhan only (his GitHub noreply address); no co-author lines.
