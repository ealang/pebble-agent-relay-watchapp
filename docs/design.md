# Design notes

## Constraints that shaped the app

**A watchapp cannot record audio.** The PebbleOS mic driver (`mic_start` etc.)
is firmware-internal — absent from the SDK's exported symbols on every
platform. Even if it weren't, there is no transport: raw 16 kHz PCM_16 is
32 KB/s against roughly 8 KB per AppMessage, and DataLogging is capped at
~640 bytes per packet. The dictation API (`dictation_session_*`) is the only
sanctioned mic path, and it returns transcript text. So the app is text-only
by construction, not by choice.

**PKJS has no `FormData`.** The multipart body is assembled by hand as a plain
string. That is only correct because every part is text — a string-built
multipart body with text parts is byte-identical to what a real encoder
produces.

## Flow

```
open app → dictation_session_start
        → transcript via AppMessage (key TRANSCRIPT)
        → PKJS builds multipart body, XHR POST to configured URL
        → result via AppMessage (keys STATUS, RESPONSE)
        → watch displays outcome ~2s, exits
```

Validation is lazy: dictation starts instantly, and configuration problems
surface in the result ("Set URL in Settings") rather than delaying the
recording. The system's own dictation UI handles voice-level errors (no
speech, no connectivity) before the app sees anything.

## Outcomes shown on the watch

| display | meaning |
|---|---|
| `Sent` | server returned 2xx |
| `HTTP <code>` | server rejected the request (401 = bad auth) |
| `Network error` | connection failed (server down / unreachable) |
| `No response` | connected but no reply within 15 s |
| `Set URL in Settings` | no webhook URL configured |
| `Phone unreachable` / `Send failed` | AppMessage to the phone failed |

`Network error` vs `No response` is deliberate: refused means the service is
down; silence means the network path is broken. Different fixes.

## Configuration

Clay settings page: one URL plus four generic header name/value slots (Clay
has no dynamic rows). Headers are applied verbatim to every request, except
`Content-Type`, which the multipart boundary must own. Auth is therefore not
special-cased — it's just a header, typically `Authorization: Bearer <token>`.
Settings live in PKJS `localStorage`, keyed per app UUID, on the phone.

## Build gotcha

Adding a `messageKey` to `package.json` requires `pebble clean`; otherwise the
generated key header is stale and the build fails on an undeclared
`MESSAGE_KEY_`.
