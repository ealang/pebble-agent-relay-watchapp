# Agent Relay

A Pebble watchapp that dictates a voice message and hands the transcript to
its Android companion app. Open the app, speak, done.

Agent Relay uses the Telegram API and is part of the Telegram ecosystem. It
is not affiliated with or endorsed by Telegram. Pebble is a trademark of
Core Devices LLC.

## What it does

1. On launch, a dictation session starts immediately.
2. The transcript appears on a review screen, with a bar along the bottom
   counting down (two seconds by default). Do nothing and it sends on its
   own.
3. The transcript is sent to the phone via AppMessage (key `TRANSCRIPT`),
   with the time it was captured (key `RECORDED_AT`, seconds since the
   epoch from the watch clock) for the companion to recognise a repeat.
   The key numbers are pinned in `package.json` (`TRANSCRIPT` 10000,
   `RECORDED_AT` 10003) so a companion built against them keeps working
   if keys are later added or reordered.
4. The Pebble phone app routes the message to the declared companion app,
   Agent Relay (`io.github.ealang.agentrelay`), which queues it durably
   and Acks.
   If the phone does not answer within the firmware's ten seconds, the
   watch sends the same message once more.
5. On the Ack the watch vibrates once. If you pressed Select to send, the
   app exits; if the countdown sent it, the transcript stays up marked
   Sent (see below).

## Failures

Every failure vibrates twice and stays on screen until you press a button,
with a message that names the step that failed so reports can be matched to
a cause:

- Session creation: `Phone not connected`, or `Dictation unavailable`.
- Starting or running dictation: `No connection`, `Dictation error`,
  `Speech not recognized`, `Voice disabled in phone app`, and so on, one
  per firmware status.
- Sending: `Not sent: phone not connected`, `Phone rejected the message`
  (the companion Nacked it), or `No reply from phone` after two attempts. Any
  button returns to the review screen, where Select sends again and Back
  records again. A repeat carries the same `RECORDED_AT`, so the companion
  can drop it if the first attempt did arrive.

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

- **Countdown**: a bar along the bottom fills, then the message sends.
  Select (check icon) sends at once. Up, Down or a drag stop the countdown
  and scroll in the same press, with arrows when there is more text that
  way. Back records again.
- **Review** (`Not yet sent`): Up / Down scroll, with arrows when there is
  more text that way. Select (check icon) sends; the footer reads
  `Sending...` and the app exits on the Ack. Back discards the transcript
  and records again.
- **Sent**: after the countdown sends, the transcript stays up with the
  footer reading `Sending...` and then `Sent`. Up / Down scroll, Select
  exits, Back records a new message. This screen can be turned off in the
  settings.
- Back during `Sending...` records again. The send itself cannot be
  recalled, so the message may still reach the phone.
- **Long-press Back** exits from any screen without sending.

The review, sent and no-speech screens close the app after 30 seconds
without a button press or drag; a review that times out is not sent.

When the app is launched from the phone (the "Start app" button, an
install, or opening the settings page), it shows `Press to continue` and
records nothing until you press Up, Select or Down (or tap or swipe up or
down); Back exits. After that it behaves exactly as a launch from the watch.

## Settings

The phone app's settings page has two options:

- **Send after**: how long the countdown runs before sending. Choose 1, 2
  (default), 3 or 5 seconds, *Immediately, no review* to send as soon as
  dictation finishes, or *Only when I press Select* to always wait for you.
- **Show what was sent** (on by default): whether a message sent without
  Select stays on screen as the Sent screen, or the app exits on the Ack.
  Hidden when sending only on Select, since then every send is a press.

The Save button only appears once a setting differs from what was saved,
so an unsaved change is visible at a glance.

A save is pushed to the watch, which persists it, so the settings add no
startup delay. Opening the settings page launches the app, and the save
applies from the next recording; a countdown already running keeps its
length. If the app is not running when you save (for instance after
backing out of its `Press to continue` screen), the watch does not get the
new values; open the settings page and save again.

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
