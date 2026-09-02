# Voice Hook

A Pebble watchapp that dictates a voice message and posts the transcript to a
webhook. Open the app, speak, done.

## What it does

1. On launch, a dictation session starts immediately.
2. The transcript is sent to the phone via AppMessage.
3. PebbleKit JS posts it to your configured webhook URL as
   `multipart/form-data`:

   | part | value |
   |---|---|
   | `transcription` | the dictated text |
   | `recordedAt` | epoch milliseconds |
   | `client` | `watch` |

4. The watch shows the outcome and exits: `Sent` on a 2xx, `HTTP <code>` on a
   rejection, `Network error` if the connection fails, `No response` if the
   server doesn't answer within 15 seconds.

## Configuration

In the phone app, open the watchapp's settings page:

- **Webhook URL** — where to POST.
- **Headers** — up to four name/value pairs sent with every request. Use these
  for authorization, e.g. name `Authorization`, value `Bearer <token>`.

Settings are stored on the phone. Nothing is hardcoded in the app.

## Why text only

Third-party Pebble watchapps cannot access the microphone directly — the
dictation API is the only sanctioned voice input, and it returns text. That
keeps this app small: the platform does the hard part.

## Building

Requires the [Pebble SDK](https://developer.repebble.com/sdk/):

```
npm install
pebble build
pebble install --phone <phone-ip>
```

Targets every platform with a microphone (basalt, chalk, diorite, emery,
flint, gabbro).
