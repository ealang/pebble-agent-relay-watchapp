# Voice Hook

A Pebble watchapp that dictates a voice message and hands the transcript to
its Android companion app. Open the app, speak, done.

## What it does

1. On launch, a dictation session starts immediately.
2. The transcript appears on a review screen, in quotes, with a bar along
   the bottom counting down (two seconds by default). Do nothing and it
   sends on its own.
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
- **Select** sends right away.
- **Back** discards the transcript and starts a new dictation.
- **Long-press Back** exits without sending.

When the app is launched from the phone (the "Start app" button, an
install, or opening the settings page), the review opens without a
countdown so nothing is sent by accident, and failure messages stay on
screen until you press Back so the settings page is not closed under you.

Dictated text is always shown in quotes; status messages are not.

## Settings

The phone app's settings page has one option, **Send after**: how long the
review screen waits before sending. Choose 1, 2 (default) or 5 seconds,
*Immediately, no review* to send as soon as dictation finishes, or *Only
when I press Select* to always wait for you.

The setting is persisted on the watch, so it adds no startup delay; a
change takes effect from the next launch. Phone launches always wait for
Select regardless of the setting.

Text follows the watch's Text Size setting (Settings → Notifications), so
the review matches the system UI.

## Building

Requires the [Pebble SDK](https://developer.repebble.com/sdk/):

```
npm install
pebble build
pebble install --phone <phone-ip>
```

Targets every platform with a microphone (basalt, chalk, diorite, emery,
flint, gabbro).
