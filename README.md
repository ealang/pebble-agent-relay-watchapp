# Agent Relay

A streamlined Pebble watchapp for sending a dictated message to a single AI
agent over Telegram. Inspired by the [Pebble Index](https://repebble.com/index).

Install the [pebble app](https://apps.repebble.com/69bcd8669ec641129e098dd3).

Requires the companion app:
[Agent Relay for Android](https://github.com/ealang/pebble-agent-relay-android).

## Using it

Open the app, speak, done. The agent's reply arrives as a standard Pebble
notification from Telegram. The Android app queues messages on the phone, so
a dictation sent while offline is delivered once the phone reconnects.

## Building

Requires the [Pebble SDK](https://developer.repebble.com/sdk/) 4.33 or later:

```
npm install
pebble build
pebble install --phone <phone-ip>
```

Targets every platform with a microphone (basalt, chalk, diorite, emery,
flint, gabbro).
