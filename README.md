# Voice Hook

A Pebble watchapp that dictates a voice message and hands the transcript to
its Android companion app. Open the app, speak, done.

## What it does

1. On launch, a dictation session starts immediately.
2. The transcript is sent to the phone via AppMessage (key `TRANSCRIPT`).
3. The Pebble phone app routes the message to the declared companion app,
   [pebble-telegram-webhook](https://github.com/ealang/pebble-telegram-webhook)
   (`io.github.ealang.pebbletelegramagent`), which queues it durably and Acks.
4. The watch shows `Queued` on the Ack and exits, or `Send failed` if the
   message could not be delivered to the companion.

## Settings

The phone app's settings page offers two toggles, both on by default:

- **Confirm transcript** — review the dictated text on the watch before
  sending. Turn off for eyes-free use: speak and drop your wrist; a short
  vibe means queued, a double vibe means the send failed.
- **Show error dialogs** — the system dialog when dictation fails.

Settings are persisted on the watch, so they add no startup delay; a change
takes effect from the next launch.

## Building

Requires the [Pebble SDK](https://developer.repebble.com/sdk/):

```
npm install
pebble build
pebble install --phone <phone-ip>
```

Targets every platform with a microphone (basalt, chalk, diorite, emery,
flint, gabbro).
