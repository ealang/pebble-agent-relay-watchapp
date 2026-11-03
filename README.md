# Agent Relay

A streamlined Pebble watchapp for sending a dictated message to a single AI
agent over Telegram. Inspired by the [Pebble Index](https://repebble.com/index):
open the app, speak, done.

Requires the companion app:
[Agent Relay for Android](https://github.com/ealang/pebble-agent-relay-android).

## Using it

- Launching the app starts dictation at once. The transcript then shows on a
  review screen with a countdown bar; do nothing and it sends.
- Select sends now. Up, Down or a drag pause the countdown and scroll. Back
  records again. Long-press Back exits without sending.
- One vibe means sent. Two vibes mean a failure, with the reason on screen.
- Launched from the phone, the app waits for a button press before recording.

## Settings

In the Pebble app's settings page for Agent Relay:

- **Send after**: the countdown length (1, 2, 3 or 5 seconds), *Immediately,
  no review*, or *Only when I press Select*.
- **Show what was sent**: keep the transcript on screen after an automatic
  send, or exit as soon as the phone confirms.

## Building

Requires the [Pebble SDK](https://developer.repebble.com/sdk/) 4.33 or later:

```
npm install
pebble build
pebble install --phone <phone-ip>
```

Targets every platform with a microphone (basalt, chalk, diorite, emery,
flint, gabbro).
