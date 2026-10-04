# Voice Hook

A Pebble watchapp that dictates a voice message and hands the transcript to
its Android companion app. Open the app, speak, done.

## What it does

1. On launch, a dictation session starts immediately.
2. The transcript appears on a review screen, in quotes, with a bar along
   the bottom counting down two seconds. Do nothing and it sends on its own.
3. The transcript is sent to the phone via AppMessage (key `TRANSCRIPT`).
4. The Pebble phone app routes the message to the declared companion app,
   [pebble-telegram-webhook](https://github.com/ealang/pebble-telegram-webhook)
   (`io.github.ealang.pebbletelegramagent`), which queues it durably and Acks.
5. On the Ack the watch vibrates once and exits. Failures (`Send failed`,
   `Phone unreachable`, `No speech detected`, ...) stay on screen for two
   seconds, with a double vibe for a failed send.

## The review screen

- **Up / Down** scroll the transcript one line at a time (one page on round
  watches). Any press cancels the countdown; the bar disappears.
- **Select** sends now.
- **Back** discards the transcript and starts a new dictation.
- **Long-press Back** exits without sending.

When the app is launched from the phone (the "Start app" button or an
install), the review opens without a countdown so nothing is sent by
accident.

Dictated text is always shown in quotes; status messages are not.

Text follows the watch's Text Size setting (Settings → Notifications), so
the review matches the system UI.

## Building

Requires the [Pebble SDK](https://developer.repebble.com/sdk/):

```
pebble build
pebble install --phone <phone-ip>
```

Targets every platform with a microphone (basalt, chalk, diorite, emery,
flint, gabbro).
