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

There is nothing to configure on the watch or in the phone app's settings
page; the companion app owns delivery.

## Building

Requires the [Pebble SDK](https://developer.repebble.com/sdk/):

```
pebble build
pebble install --phone <phone-ip>
```

Targets every platform with a microphone (basalt, chalk, diorite, emery,
flint, gabbro).
