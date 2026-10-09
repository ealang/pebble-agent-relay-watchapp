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
5. On the Ack the watch vibrates once. If you pressed Select to send, the
   app exits; if the countdown sent it, the transcript stays up marked
   Sent (see below).

## Failures

Every failure vibrates twice and stays on screen until you press Back, with
a message that names the step that failed so reports can be matched to a
cause:

- Session creation: `Phone not connected`, or `Dictation unavailable` when
  the phone app lacks voice support or the watch is out of memory.
- Starting or running dictation: `Can't start. ...`, `Dictation: phone
  voice session failed`, `Dictation: no phone or internet`, `Dictation:
  recognizer failed`, and so on, one per firmware status.
- Sending: `Phone not connected`, `Phone rejected the message` (the
  companion Nacked it), or `No reply from phone in 10 s (may have sent)`.
  The firmware's Ack wait is fixed at ten seconds and a late Ack is ignored,
  so that last one is ambiguous: the companion may already have queued the
  message.

Not failures: when dictation hears nothing (or returns an empty
transcript) the app shows `[No speech detected]` without a vibe; Back
records again and Select exits. Backing out of the dictation screen exits
silently.

## The transcript screens

The transcript is shown with the system's scrolling text and action bar,
in the body font the firmware uses for its own dictation review. The
action bar is transparent, so only its icons show, and only for what the
buttons do on that screen. On watches with a touchscreen, dragging scrolls
the text and the icons can be tapped, provided touch navigation is on in
the watch's settings.

- **Countdown**: a bar along the bottom fills, then the message sends. No
  icons are shown; Up, Down, Select or a drag stop it and leave it on the
  review. Back records again.
- **Review** (`Not yet sent`): Up / Down scroll, with arrows when there is
  more text that way. Select (check icon) sends; the app shows `Sending...`
  and exits on the Ack. Back discards the transcript and records again.
- **Sent**: after the countdown sends, the transcript stays up marked
  `Sending...` and then `Sent`. Up / Down scroll, Select exits, Back
  records a new message. It closes by itself 30 seconds after the Ack
  unless you scroll it. This screen can be turned off in the settings.
- **Long-press Back** exits from any screen without sending.

When the app is launched from the phone (the "Start app" button, an
install, or opening the settings page), the review opens without a
countdown so nothing is sent by accident, and the app never exits on its
own so the settings page is not closed under you.

Dictated text is always shown in quotes; status messages are not.

## Settings

The phone app's settings page has two options:

- **Send after**: how long the countdown runs before sending. Choose 1, 2
  (default) or 5 seconds, *Immediately, no review* to send as soon as
  dictation finishes, or *Only when I press Select* to always wait for you.
- **Show what was sent** (on by default): whether a message sent without
  Select stays on screen as the Sent screen, or the app exits on the Ack.

The settings are persisted on the watch, so they add no startup delay; a
change takes effect from the next launch. Phone launches always wait for
Select regardless of the setting.

Text follows the watch's Text Size setting (Settings → Notifications), so
the review matches the system UI.

## Building

Requires the [Pebble SDK](https://developer.repebble.com/sdk/) 4.33 or
later, which adds `app_touch_navigation_enable`:

```
npm install
pebble build
pebble install --phone <phone-ip>
```

Targets every platform with a microphone (basalt, chalk, diorite, emery,
flint, gabbro).
