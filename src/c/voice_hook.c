#include <pebble.h>

#define TRANSCRIPT_MAX 512
#define STATUS_MAX 96

// How long the review screen waits before sending on its own, in seconds.
// Set from the phone's settings page and persisted on the watch. 0 sends
// without showing the review; REVIEW_DELAY_MANUAL never sends on its own.
#define REVIEW_DELAY_DEFAULT_S 2
#define REVIEW_DELAY_MAX_S 5
#define REVIEW_DELAY_MANUAL 255
// Keys 1 and 2 were used by earlier builds and are not reused.
#define PERSIST_KEY_REVIEW_DELAY 3
#define PERSIST_KEY_SENT_SCREEN 4
#define IDLE_EXIT_MS 30000
// How many times a transcript is sent before the phone's silence is reported.
// The firmware gives each send 10 s for the phone's Ack.
#define SEND_ATTEMPTS 2
// The footer is a black strip along the bottom of the review screen:
// "Not yet sent", "Sending..." or "Sent".
#define FOOTER_HEIGHT 20
// The countdown is a rounded bar in the style of the firmware's progress bar,
// drawn over the text just above the footer. It takes the same share of the
// screen's width at its height on every watch, and the system progress bar's
// height.
#define BAR_TICK_MS 50
#define BAR_GAP 4
#define BAR_WIDTH_PERCENT 60
#define BAR_HEIGHT PBL_IF_COLOR_ELSE(6, 7)
// Round screens leave a blank margin above the text so the first line is not
// cut by the curve. The firmware's dictation review uses the status bar height
// there; without a clock, chalk fits a line more with 8 px less, while gabbro
// gains nothing short of a cramped first line.
#define TOP_MARGIN PBL_PLATFORM_SWITCH(PBL_PLATFORM_TYPE_CURRENT, 0, 0, 16, 0, 0, 0, STATUS_BAR_LAYER_HEIGHT)
// On round screens the footer sits this far above the bottom, where the screen
// is wide enough for its text, and the black runs on down to the edge.
#define FOOTER_LIFT PBL_IF_ROUND_ELSE(12, 0)
// Height of the box the transcript is measured in; comfortably more than
// TRANSCRIPT_MAX characters can fill at the largest font on the narrowest screen.
#define TEXT_MAX_HEIGHT 4000

static Window *s_window;
// Shown first when the phone launches the app; see prv_gate_push.
static Window *s_gate_window;
static TextLayer *s_gate_text;
static TextLayer *s_status_layer;
static ScrollLayer *s_scroll_layer;
static TextLayer *s_transcript_layer;
// Draws the footer strip and the countdown bar; see prv_footer_update.
static Layer *s_footer_layer;
// The countdown bar, and the top of the footer strip, in s_footer_layer's
// coordinates.
static GRect s_bar;
static int16_t s_footer_y;
// The standard action bar, as on the firmware's dictation review, with that
// screen's own icons recoloured black. Its background is clear, so it is
// invisible except for icons, which appear only for what the buttons do.
static ActionBarLayer *s_action_bar;
static GBitmap *s_icon_check;
static GBitmap *s_icon_up;
static GBitmap *s_icon_down;

static char s_status_text[STATUS_MAX];
static char s_transcript[TRANSCRIPT_MAX];

// One line of the chosen font, measured at load. Everything sized to the text
// (status box, descender padding, scroll step) derives from this.
static int16_t s_line_height;

static uint32_t s_review_delay_s = REVIEW_DELAY_DEFAULT_S;

// Every screen is the same window: either centered status text, or the
// review screen, which is the scrollable transcript with the action bar and
// a strip along the bottom. What the window shows is derived in prv_render
// from three pieces of state: the screen, the send, and the countdown.
typedef enum {
  // "Loading..." under the firmware's dictation UI. Back exits.
  ScreenLoading,
  // Dictation heard nothing: a placeholder. Back records again, Select exits.
  ScreenNoSpeech,
  // Why dictation failed. Back exits.
  ScreenDictationFailed,
  // The transcript. The footer shows the send's state; Select sends, or
  // stops a running countdown; Back records again, abandoning any send.
  ScreenReview,
  // Why the send failed. Any button returns to the review, to send again.
  ScreenSendFailed,
} Screen;

static Screen s_screen;

// The send: NotSent until prv_send, InFlight until the phone answers, Sent
// after its Ack. A failure shows ScreenSendFailed and the state goes back to
// NotSent on the way out of it.
typedef enum {
  SendNotSent,
  SendInFlight,
  SendSent,
} SendState;

static SendState s_send;
// How many times the current send has been put in the outbox.
static uint8_t s_send_attempts;
// A send the user chose with Select needs no more confirmation, so the app
// exits on the Ack. An auto-send stays up as the sent screen if that
// setting is on.
static bool s_send_exit_on_ack;
// The outbox callbacks carry nothing that says which send they are for. Each
// send gets a token, and the token of the message in the outbox is kept; a
// callback whose token is stale belongs to a send that was abandoned by Back,
// and only frees the outbox for the current one. A send cannot be cancelled,
// so an abandoned one may still reach the phone.
static uint8_t s_send_token;
static uint8_t s_outbox_token;
// When the transcript was captured, sent with it as the companion's dedupe
// key: every attempt to send the same transcript carries the same value.
static uint32_t s_recorded_at;

// The countdown: a timer that calls prv_send once when the bar fills, and is
// cancelled by any input. It is measured from the clock rather than counted
// in ticks, so timer latency does not stretch it. Its length is fixed when it
// starts, so a setting saved meanwhile cannot change it partway through.
static AppTimer *s_bar_timer;
static uint64_t s_bar_start_ms;
static uint32_t s_bar_total_ms;
// Whether an unconfirmed send (countdown ran out, or no review at all) leaves
// the transcript up. Set from the phone's settings page.
static bool s_sent_screen_enabled = true;
// Closes the app after IDLE_EXIT_MS without input where nothing is about to
// happen: a paused or sent review, or no speech. Nothing is sent. Failures
// stay up until a button so they can be read.
static AppTimer *s_idle_timer;
// Set once the app has asked to close, so a callback that lands during the
// exit animation does not touch layers that are being torn down.
static bool s_exiting;

// The icons currently on the action bar, so an unchanged icon is not
// animated in again.
static const GBitmap *s_bar_icons[NUM_BUTTONS];

static DictationSession *s_dictation;

// The firmware's body text font for the watch's Text Size setting, as used
// by notifications and its own dictation screen (TextStyleFont_Body). Medium
// is bold there too. Extra Large uses Gothic 36, which apps cannot load, so
// it falls back to the Large font.
static GFont prv_content_font(void) {
  switch (preferred_content_size()) {
    case PreferredContentSizeSmall:
      return fonts_get_system_font(FONT_KEY_GOTHIC_18);
    case PreferredContentSizeMedium:
      return fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD);
    default:
      return fonts_get_system_font(FONT_KEY_GOTHIC_28);
  }
}

static int16_t prv_measure_line_height(GFont font) {
  return graphics_text_layout_get_content_size("Ag", font, GRect(0, 0, 1000, 1000),
                                               GTextOverflowModeWordWrap, GTextAlignmentLeft).h;
}

// The current time in milliseconds, for timing the countdown.
static uint64_t prv_now_ms(void) {
  time_t seconds;
  uint16_t ms;
  time_ms(&seconds, &ms);
  return (uint64_t) seconds * 1000 + ms;
}

static void prv_exit(bool animated) {
  s_exiting = true;
  window_stack_pop_all(animated);
}

static void prv_idle_exit(void *context) {
  prv_exit(false);
}

static bool prv_counting(void) {
  return s_bar_timer != NULL;
}

static void prv_countdown_cancel(void) {
  if (s_bar_timer) {
    app_timer_cancel(s_bar_timer);
    s_bar_timer = NULL;
  }
}

static void prv_cancel_idle(void) {
  if (s_idle_timer) {
    app_timer_cancel(s_idle_timer);
    s_idle_timer = NULL;
  }
}

// Starts the idle timeout over; called on every state change and input.
static void prv_restart_idle(void) {
  prv_cancel_idle();
  bool idle_review = s_screen == ScreenReview && !prv_counting() && s_send != SendInFlight;
  if (idle_review || s_screen == ScreenNoSpeech) {
    s_idle_timer = app_timer_register(IDLE_EXIT_MS, prv_idle_exit, NULL);
  }
}

static void prv_set_bar_icon(ButtonId button, const GBitmap *icon) {
  if (s_bar_icons[button] == icon) {
    return;
  }
  s_bar_icons[button] = icon;
  if (icon) {
    action_bar_layer_set_icon_animated(s_action_bar, button, icon, true);
  } else {
    action_bar_layer_clear_icon(s_action_bar, button);
  }
}

// The check appears only when Select would send: a paused review, not the
// countdown or once a send is under way. The countdown shows no icons at
// all, since every button there only stops it. Otherwise Up/Down arrows
// appear when there is more text that way.
static void prv_update_action_bar(void) {
  bool counting = prv_counting();
  bool can_send = s_screen == ScreenReview && !counting && s_send == SendNotSent;
  prv_set_bar_icon(BUTTON_ID_SELECT, can_send ? s_icon_check : NULL);
  GPoint offset = scroll_layer_get_content_offset(s_scroll_layer);
  int16_t visible_h = layer_get_bounds(scroll_layer_get_layer(s_scroll_layer)).size.h;
  int16_t content_h = scroll_layer_get_content_size(s_scroll_layer).h;
  prv_set_bar_icon(BUTTON_ID_UP, !counting && offset.y < 0 ? s_icon_up : NULL);
  prv_set_bar_icon(BUTTON_ID_DOWN,
                   !counting && offset.y + content_h > visible_h ? s_icon_down : NULL);
}

// The strip's text on the review screen; the countdown draws its bar above it.
static const char *prv_footer_text(void) {
  if (s_screen != ScreenReview) {
    return NULL;
  }
  switch (s_send) {
    case SendNotSent:
      return "Not yet sent";
    case SendInFlight:
      return "Sending...";
    case SendSent:
      return "Sent";
  }
  return NULL;
}

// Brings the window into line with the state. Nothing else touches what the
// layers show, so every transition is a state change followed by this.
static void prv_render(void) {
  bool review = s_screen == ScreenReview;
  layer_set_hidden(text_layer_get_layer(s_status_layer), review);
  layer_set_hidden(scroll_layer_get_layer(s_scroll_layer), !review);
  layer_set_hidden(action_bar_layer_get_layer(s_action_bar), !review);
  layer_set_hidden(s_footer_layer, !review);
  layer_mark_dirty(s_footer_layer);
  prv_update_action_bar();
  prv_restart_idle();
}

static void prv_set_screen(Screen screen) {
  s_screen = screen;
  prv_render();
}

static void prv_show_status(Screen screen, const char *text) {
  strncpy(s_status_text, text, STATUS_MAX - 1);
  s_status_text[STATUS_MAX - 1] = '\0';
  text_layer_set_text(s_status_layer, s_status_text);
  prv_set_screen(screen);
}

// Every failure ends here: a double vibe, and the reason stays on screen
// until a button so it can actually be read. Each message names one
// condition.
static void prv_fail(Screen screen, const char *text) {
  vibes_double_pulse();
  prv_show_status(screen, text);
}

// Any interaction cancels the auto-send.
static void prv_pause_review(void) {
  if (prv_counting()) {
    prv_countdown_cancel();
    prv_render();
  }
}

// Also reached by a touch drag, which moves the content without any click:
// that stops a countdown, and resets the idle timeout, like Up/Down do.
static void prv_scroll_offset_changed(ScrollLayer *scroll_layer, void *context) {
  if (scroll_layer_get_content_offset(scroll_layer).y != 0) {
    prv_pause_review();
  }
  prv_restart_idle();
  prv_update_action_bar();
}

// --- Send --------------------------------------------------------------------

static const char *prv_app_message_text(AppMessageResult result) {
  static char buf[32];
  switch (result) {
    case APP_MSG_SEND_TIMEOUT:
      return "No reply from phone";
    case APP_MSG_SEND_REJECTED:
      return "Phone rejected the message";
    case APP_MSG_NOT_CONNECTED:
      return "Phone not connected";
    case APP_MSG_APP_NOT_RUNNING:
      return "Phone app not running";
    case APP_MSG_BUFFER_OVERFLOW:
      return "Message too large";
    default:
      snprintf(buf, sizeof(buf), "AppMessage error %d", (int) result);
      return buf;
  }
}

static void prv_send_failed(const char *text) {
  s_send = SendNotSent;
  prv_fail(ScreenSendFailed, text);
}

// Puts the transcript in the outbox. If an abandoned send is still waiting
// for its answer, the outbox is busy; its callback starts this one.
static void prv_send_now(void) {
  DictionaryIterator *out;
  AppMessageResult result = app_message_outbox_begin(&out);
  if (result == APP_MSG_BUSY) {
    return;
  }
  if (result != APP_MSG_OK) {
    prv_send_failed(prv_app_message_text(result));
    return;
  }
  if (dict_write_cstring(out, MESSAGE_KEY_TRANSCRIPT, s_transcript) != DICT_OK ||
      dict_write_uint32(out, MESSAGE_KEY_RECORDED_AT, s_recorded_at) != DICT_OK) {
    prv_send_failed("Transcript too large for outbox");
    return;
  }
  result = app_message_outbox_send();
  if (result != APP_MSG_OK) {
    prv_send_failed(prv_app_message_text(result));
    return;
  }
  s_outbox_token = s_send_token;
  s_send_attempts++;
}

// Starts sending the transcript on screen. The review stays up, its footer
// reading "Sending...".
static void prv_send(bool chosen_with_select) {
  s_send = SendInFlight;
  s_send_token++;
  s_send_attempts = 0;
  s_send_exit_on_ack = chosen_with_select || !s_sent_screen_enabled;
  prv_countdown_cancel();
  prv_render();
  prv_send_now();
}

// Forgets the send in progress, if any: its outbox callback is then stale.
static void prv_send_abandon(void) {
  s_send = SendNotSent;
  s_send_token++;
}

// The outbox is free again after an abandoned send. If the current one was
// waiting for it, it goes now.
static void prv_stale_outbox_done(void) {
  if (s_send == SendInFlight) {
    prv_send_now();
  }
}

// The companion app Acks once the transcript is durably queued on the phone;
// a short vibe confirms it without looking.
static void prv_outbox_sent(DictionaryIterator *iter, void *context) {
  if (s_exiting) {
    return;
  }
  if (s_outbox_token != s_send_token) {
    prv_stale_outbox_done();
    return;
  }
  vibes_short_pulse();
  if (s_send_exit_on_ack) {
    prv_exit(false);
    return;
  }
  s_send = SendSent;
  prv_render();
}

// The phone's silence is the one failure worth another go: the message or
// its Ack can be lost, or the phone's handlers can take longer than the
// firmware's 10 s to come up. The companion ignores a repeat.
static void prv_outbox_failed(DictionaryIterator *iter, AppMessageResult reason, void *context) {
  if (s_exiting) {
    return;
  }
  if (s_outbox_token != s_send_token) {
    prv_stale_outbox_done();
    return;
  }
  if (reason == APP_MSG_SEND_TIMEOUT && s_send_attempts < SEND_ATTEMPTS) {
    prv_send_now();
    return;
  }
  prv_send_failed(prv_app_message_text(reason));
}

// --- Review screen -----------------------------------------------------------

#if defined(PBL_ROUND)
static int32_t prv_isqrt(int32_t n) {
  int32_t x = 0;
  while ((x + 1) * (x + 1) <= n) {
    x++;
  }
  return x;
}
#endif

// Milliseconds since the countdown started, capped at its length.
static uint32_t prv_bar_elapsed_ms(void) {
  uint64_t elapsed = prv_now_ms() - s_bar_start_ms;
  return elapsed < s_bar_total_ms ? (uint32_t) elapsed : s_bar_total_ms;
}

static void prv_footer_update(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  const char *footer = prv_footer_text();
  if (footer) {
    GRect strip = GRect(0, s_footer_y, bounds.size.w, bounds.size.h - s_footer_y);
    graphics_context_set_fill_color(ctx, GColorBlack);
    graphics_fill_rect(ctx, strip, 0, GCornerNone);
    graphics_context_set_text_color(ctx, GColorWhite);
    strip.origin.y += 1;
    strip.size.h = FOOTER_HEIGHT;
    graphics_draw_text(ctx, footer, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD), strip,
                       GTextOverflowModeFill, GTextAlignmentCenter, NULL);
  }
  if (!prv_counting()) {
    return;
  }
  // Colours as on the firmware's dictation screen: black on dark grey, or an
  // outlined white track on black-and-white watches.
  GRect bar = s_bar;
  const int16_t radius = bar.size.h / 2;
  graphics_context_set_fill_color(ctx, PBL_IF_COLOR_ELSE(GColorDarkGray, GColorWhite));
  graphics_fill_rect(ctx, bar, radius, GCornersAll);
  graphics_context_set_fill_color(ctx, GColorBlack);
  GRect filled = bar;
  filled.size.w = bar.size.w * prv_bar_elapsed_ms() / s_bar_total_ms;
  if (filled.size.w > 0) {
    graphics_fill_rect(ctx, filled, filled.size.w < 2 * radius ? filled.size.w / 2 : radius,
                       GCornersAll);
  }
#if !defined(PBL_COLOR)
  graphics_context_set_stroke_color(ctx, GColorBlack);
  graphics_draw_round_rect(ctx, bar, radius);
#endif
}

static void prv_bar_tick(void *context) {
  if (prv_bar_elapsed_ms() >= s_bar_total_ms) {
    s_bar_timer = NULL;
    prv_send(false);
    return;
  }
  layer_mark_dirty(s_footer_layer);
  s_bar_timer = app_timer_register(BAR_TICK_MS, prv_bar_tick, NULL);
}

static void prv_countdown_start(uint32_t total_ms) {
  s_bar_start_ms = prv_now_ms();
  s_bar_total_ms = total_ms;
  s_bar_timer = app_timer_register(BAR_TICK_MS, prv_bar_tick, NULL);
  prv_render();
}

// Lays out the transcript in the scrollable area and shows the review.
static void prv_show_transcript(void) {
  text_layer_set_text(s_transcript_layer, s_transcript);

  // The firmware measures text within the layer's current bounds, so grow the
  // layer back to its full measuring height first; otherwise a retake that is
  // longer than the previous transcript is clipped to the old size.
  GRect frame = layer_get_frame(text_layer_get_layer(s_transcript_layer));
  text_layer_set_size(s_transcript_layer, GSize(frame.size.w, TEXT_MAX_HEIGHT));
  GSize content = text_layer_get_content_size(s_transcript_layer);
  content.w = frame.size.w;
  // Paging rounds the scroll range up to whole pages, so on round screens
  // any room added under the text can tip it onto a blank extra page; the
  // pages already keep every line whole.
  int16_t scroll_h = frame.origin.y + content.h + PBL_IF_ROUND_ELSE(0, s_line_height / 2 + 4);
  content.h += s_line_height / 2;  // room for the last line's descenders
  text_layer_set_size(s_transcript_layer, content);
  scroll_layer_set_content_size(s_scroll_layer, GSize(content.w, scroll_h));
  scroll_layer_set_content_offset(s_scroll_layer, GPointZero, false);
  prv_set_screen(ScreenReview);
}

static void prv_start_dictation(void);

// Returns from a send failure to the review, to try again or record again.
static void prv_leave_send_failed(void) {
  prv_set_screen(ScreenReview);
}

// Up and Down scroll like the firmware's scrolling screens; during the
// countdown they only stop it.
static void prv_scroll_click(ClickRecognizerRef recognizer, ClickHandler scroll) {
  switch (s_screen) {
    case ScreenReview:
      if (prv_counting()) {
        prv_pause_review();
      } else {
        prv_restart_idle();
        scroll(recognizer, s_scroll_layer);
      }
      return;
    case ScreenSendFailed:
      prv_leave_send_failed();
      return;
    default:
      return;
  }
}

static void prv_up_click(ClickRecognizerRef recognizer, void *context) {
  prv_scroll_click(recognizer, scroll_layer_scroll_up_click_handler);
}

static void prv_down_click(ClickRecognizerRef recognizer, void *context) {
  prv_scroll_click(recognizer, scroll_layer_scroll_down_click_handler);
}

// Select pauses a running countdown like Up and Down; on a paused review it
// sends. Once sent, and on the no-speech screen, it just closes the app.
static void prv_select_click(ClickRecognizerRef recognizer, void *context) {
  switch (s_screen) {
    case ScreenReview:
      if (prv_counting()) {
        prv_pause_review();
      } else if (s_send == SendNotSent) {
        prv_send(true);
      } else if (s_send == SendSent) {
        prv_exit(false);
      }
      break;
    case ScreenSendFailed:
      prv_leave_send_failed();
      break;
    case ScreenNoSpeech:
      prv_exit(false);
      break;
    default:
      break;
  }
}

// Back records again wherever there is a transcript to replace, even while
// it is still being sent, and otherwise exits.
static void prv_back_click(ClickRecognizerRef recognizer, void *context) {
  switch (s_screen) {
    case ScreenReview:
    case ScreenNoSpeech:
      prv_start_dictation();
      break;
    case ScreenSendFailed:
      prv_leave_send_failed();
      break;
    default:
      prv_exit(true);
      break;
  }
}

static void prv_click_config(void *context) {
  window_single_repeating_click_subscribe(BUTTON_ID_UP, 100, prv_up_click);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, 100, prv_down_click);
  window_single_click_subscribe(BUTTON_ID_SELECT, prv_select_click);
  window_single_click_subscribe(BUTTON_ID_BACK, prv_back_click);
}

// --- Dictation and AppMessage ------------------------------------------------

// One message per firmware status, so a report from the field pins down
// which step failed. System error dialogs are off, so the first error ends
// the session instead of the firmware's own retry loop.
static const char *prv_dictation_failure_text(DictationSessionStatus status) {
  static char buf[32];
  switch (status) {
    case DictationSessionStatusFailureTranscriptionRejectedWithError:
      return "Dictation: exited after error";
    case DictationSessionStatusFailureSystemAborted:
      return "Dictation: phone voice session failed";
    case DictationSessionStatusFailureConnectivityError:
      return "Dictation: no phone or internet";
    case DictationSessionStatusFailureDisabled:
      return "Dictation disabled for this account";
    case DictationSessionStatusFailureInternalError:
      return "Dictation: internal error";
    case DictationSessionStatusFailureRecognizerError:
      return "Dictation: recognizer failed";
    default:
      snprintf(buf, sizeof(buf), "Dictation status %d", (int) status);
      return buf;
  }
}

static bool prv_is_blank(const char *text) {
  for (; text && *text; text++) {
    if (*text != ' ' && *text != '\n' && *text != '\t') {
      return false;
    }
  }
  return true;
}

// Nothing to send is not a failure: no vibe, and a placeholder, so Back
// records again and Select closes.
static void prv_no_speech(void) {
  prv_show_status(ScreenNoSpeech, "[No speech detected]");
}

static void prv_dictation_callback(DictationSession *session, DictationSessionStatus status,
                                   char *transcription, void *context) {
  if (s_exiting) {
    return;
  }
  if (status == DictationSessionStatusSuccess) {
    if (prv_is_blank(transcription)) {
      prv_no_speech();
      return;
    }
    strncpy(s_transcript, transcription, TRANSCRIPT_MAX - 1);
    s_transcript[TRANSCRIPT_MAX - 1] = '\0';
    s_recorded_at = (uint32_t) time(NULL);
    // The one place the review delay is read: it decides how the review
    // starts, and nothing after this differs between the settings.
    prv_show_transcript();
    if (s_review_delay_s == 0) {
      prv_send(false);
    } else if (s_review_delay_s != REVIEW_DELAY_MANUAL) {
      prv_countdown_start(s_review_delay_s * 1000);
    }
    return;
  }
  if (status == DictationSessionStatusFailureNoSpeechDetected) {
    prv_no_speech();
    return;
  }
  if (status == DictationSessionStatusFailureTranscriptionRejected) {
    // The user backed out; nothing to report.
    prv_exit(false);
    return;
  }
  prv_fail(ScreenDictationFailed, prv_dictation_failure_text(status));
}

// Shows our placeholder, then asks the firmware to push its voice screen on
// top. If that refuses (Bluetooth down at this instant), no callback ever
// comes, so the refusal is reported here rather than left as "Loading...".
static void prv_start_dictation(void) {
  prv_send_abandon();
  prv_countdown_cancel();
  prv_show_status(ScreenLoading, "Loading...");
  DictationSessionStatus status = dictation_session_start(s_dictation);
  if (status != DictationSessionStatusSuccess) {
    char text[STATUS_MAX];
    snprintf(text, sizeof(text), "Can't start. %s", prv_dictation_failure_text(status));
    prv_fail(ScreenDictationFailed, text);
  }
}

// --- Settings ----------------------------------------------------------------

static bool prv_review_delay_is_valid(int32_t seconds) {
  return (seconds >= 0 && seconds <= REVIEW_DELAY_MAX_S) || seconds == REVIEW_DELAY_MANUAL;
}

static void prv_load_settings(void) {
  if (persist_exists(PERSIST_KEY_SENT_SCREEN)) {
    s_sent_screen_enabled = persist_read_bool(PERSIST_KEY_SENT_SCREEN);
  }
  if (persist_exists(PERSIST_KEY_REVIEW_DELAY)) {
    int32_t stored = persist_read_int(PERSIST_KEY_REVIEW_DELAY);
    if (prv_review_delay_is_valid(stored)) {
      s_review_delay_s = stored;
    }
  }
}

// The phone pushes the settings at every launch and whenever they are saved.
// A value is written only when it changed, or was never stored (so a later
// change of default cannot override the user's choice), sparing the flash a
// write per launch.
static void prv_inbox_received(DictionaryIterator *iter, void *context) {
  Tuple *sent_screen = dict_find(iter, MESSAGE_KEY_SENT_SCREEN);
  if (sent_screen) {
    bool enabled = sent_screen->value->int32 != 0;
    if (enabled != s_sent_screen_enabled || !persist_exists(PERSIST_KEY_SENT_SCREEN)) {
      s_sent_screen_enabled = enabled;
      persist_write_bool(PERSIST_KEY_SENT_SCREEN, enabled);
    }
  }
  Tuple *tuple = dict_find(iter, MESSAGE_KEY_REVIEW_DELAY);
  if (!tuple) {
    return;
  }
  int32_t seconds = tuple->value->int32;
  if (!prv_review_delay_is_valid(seconds)) {
    return;
  }
  if ((uint32_t)seconds != s_review_delay_s || !persist_exists(PERSIST_KEY_REVIEW_DELAY)) {
    s_review_delay_s = seconds;
    persist_write_int(PERSIST_KEY_REVIEW_DELAY, seconds);
  }
}

// --- Window ------------------------------------------------------------------

static void prv_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);
  GFont font = prv_content_font();
  s_line_height = prv_measure_line_height(font);

  // Up to four lines of status text, centered on screen; failure messages
  // can run long. Round screens inset further so the outer lines fit.
  int16_t status_h = s_line_height * 4;
  int16_t status_inset = PBL_IF_ROUND_ELSE(18, 4);
  s_status_layer = text_layer_create(
      GRect(status_inset, (bounds.size.h - status_h) / 2,
            bounds.size.w - 2 * status_inset, status_h));
  text_layer_set_text_alignment(s_status_layer, GTextAlignmentCenter);
  text_layer_set_font(s_status_layer, font);
  text_layer_set_background_color(s_status_layer, GColorClear);
  layer_add_child(root, text_layer_get_layer(s_status_layer));

  // The transcript sits beside the action bar as on the firmware's dictation
  // review: a 4 px margin, left-aligned on rectangular screens and
  // right-aligned against the curved bar on round ones. The bar is invisible,
  // so on rectangular screens the text runs up to just short of its icons.
  int16_t text_w = PBL_IF_ROUND_ELSE(bounds.size.w - 8 - ACTION_BAR_WIDTH - 10,
                                     bounds.size.w - 4 - (ACTION_BAR_WIDTH - 6));
  // The text area runs from the top margin down to the footer strip; the
  // countdown bar floats over its bottom edge.
  int16_t footer_top = bounds.size.h - FOOTER_LIFT - FOOTER_HEIGHT;
  s_scroll_layer = scroll_layer_create(
      GRect(0, TOP_MARGIN, bounds.size.w, footer_top - TOP_MARGIN));
  scroll_layer_set_shadow_hidden(s_scroll_layer, true);
  scroll_layer_set_callbacks(s_scroll_layer, (ScrollLayerCallbacks) {
    .content_offset_changed_handler = prv_scroll_offset_changed,
  });
  s_transcript_layer = text_layer_create(GRect(4, -6, text_w, TEXT_MAX_HEIGHT));
  text_layer_set_font(s_transcript_layer, font);
  text_layer_set_background_color(s_transcript_layer, GColorClear);
  text_layer_set_text_alignment(s_transcript_layer,
                                PBL_IF_ROUND_ELSE(GTextAlignmentRight, GTextAlignmentLeft));
  scroll_layer_add_child(s_scroll_layer, text_layer_get_layer(s_transcript_layer));
  layer_set_hidden(scroll_layer_get_layer(s_scroll_layer), true);
  layer_add_child(root, scroll_layer_get_layer(s_scroll_layer));
#if defined(PBL_ROUND)
  // Text flow needs the layer to be in the window hierarchy already.
  scroll_layer_set_paging(s_scroll_layer, true);
  text_layer_enable_screen_text_flow_and_paging(s_transcript_layer, 8);
#endif

  // The action bar is invisible down there, so the footer spans the screen.
  // On round screens the bar is sized to the circle's width just above the
  // footer.
  int16_t screen_w = bounds.size.w;
#if defined(PBL_ROUND)
  int32_t r = bounds.size.w / 2;
  int32_t dy = footer_top - BAR_GAP - bounds.size.h / 2;
  screen_w = 2 * prv_isqrt(r * r - dy * dy);
#endif
  int16_t bar_w = screen_w * BAR_WIDTH_PERCENT / 100;
  int16_t layer_top = footer_top - BAR_GAP - BAR_HEIGHT;
  s_footer_y = footer_top - layer_top;
  s_bar = GRect((bounds.size.w - bar_w) / 2, 0, bar_w, BAR_HEIGHT);
  s_footer_layer = layer_create(GRect(0, layer_top, bounds.size.w, bounds.size.h - layer_top));
  layer_set_update_proc(s_footer_layer, prv_footer_update);
  layer_set_hidden(s_footer_layer, true);
  layer_add_child(root, s_footer_layer);

  s_icon_check = gbitmap_create_with_resource(RESOURCE_ID_ICON_CHECK);
  s_icon_up = gbitmap_create_with_resource(RESOURCE_ID_ICON_UP);
  s_icon_down = gbitmap_create_with_resource(RESOURCE_ID_ICON_DOWN);
  s_action_bar = action_bar_layer_create();
  action_bar_layer_set_click_config_provider(s_action_bar, prv_click_config);
  action_bar_layer_set_icon_press_animation(s_action_bar, BUTTON_ID_UP,
                                            ActionBarLayerIconPressAnimationMoveUp);
  action_bar_layer_set_icon_press_animation(s_action_bar, BUTTON_ID_DOWN,
                                            ActionBarLayerIconPressAnimationMoveDown);
  action_bar_layer_set_background_color(s_action_bar, GColorClear);
  action_bar_layer_add_to_window(s_action_bar, window);
  layer_set_hidden(action_bar_layer_get_layer(s_action_bar), true);
}

static void prv_window_unload(Window *window) {
  prv_cancel_idle();
  prv_countdown_cancel();
  action_bar_layer_destroy(s_action_bar);
  gbitmap_destroy(s_icon_check);
  gbitmap_destroy(s_icon_up);
  gbitmap_destroy(s_icon_down);
  layer_destroy(s_footer_layer);
  text_layer_destroy(s_transcript_layer);
  scroll_layer_destroy(s_scroll_layer);
  text_layer_destroy(s_status_layer);
}

// Pushes the transcript window and starts recording.
static void prv_start(void) {
  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers){
    .load = prv_window_load,
    .unload = prv_window_unload,
  });
  window_stack_push(s_window, true);

  s_dictation = dictation_session_create(0, prv_dictation_callback, NULL);
  if (s_dictation) {
    // Our review screen replaces the system's confirmation and error dialogs.
    dictation_session_enable_confirmation(s_dictation, false);
    dictation_session_enable_error_dialogs(s_dictation, false);
    prv_start_dictation();
  } else if (!connection_service_peek_pebble_app_connection()) {
    // The firmware refuses to create a session unless a phone app that
    // supports app dictation is connected; this is the common reason.
    prv_fail(ScreenDictationFailed, "Phone not connected");
  } else {
    prv_fail(ScreenDictationFailed,
             "Dictation unavailable: phone app lacks voice support, or out of memory");
  }
}

// --- Phone launch -------------------------------------------------------------

// The phone launches the app to open its settings page, and also from
// "Start app" and on install, with no way to tell these apart. So a phone
// launch waits for a button before recording; settings saved meanwhile
// apply to that recording. On touch watches a tap or swipe arrives as a
// button, since this page has no widgets of its own.

static void prv_gate_continue(ClickRecognizerRef recognizer, void *context) {
  // Push first: removing the only window would end the app.
  prv_start();
  window_stack_remove(s_gate_window, false);
}

static void prv_gate_click_config(void *context) {
  window_single_click_subscribe(BUTTON_ID_UP, prv_gate_continue);
  window_single_click_subscribe(BUTTON_ID_SELECT, prv_gate_continue);
  window_single_click_subscribe(BUTTON_ID_DOWN, prv_gate_continue);
}

static void prv_gate_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);
  GFont font = prv_content_font();
  int16_t h = prv_measure_line_height(font) * 2;
  int16_t inset = PBL_IF_ROUND_ELSE(18, 4);
  s_gate_text = text_layer_create(
      GRect(inset, (bounds.size.h - h) / 2, bounds.size.w - 2 * inset, h));
  text_layer_set_text(s_gate_text, "Press to continue");
  text_layer_set_text_alignment(s_gate_text, GTextAlignmentCenter);
  text_layer_set_font(s_gate_text, font);
  layer_add_child(root, text_layer_get_layer(s_gate_text));
}

static void prv_gate_window_unload(Window *window) {
  text_layer_destroy(s_gate_text);
}

static void prv_gate_push(void) {
  s_gate_window = window_create();
  window_set_click_config_provider(s_gate_window, prv_gate_click_config);
  window_set_window_handlers(s_gate_window, (WindowHandlers){
    .load = prv_gate_window_load,
    .unload = prv_gate_window_unload,
  });
  window_stack_push(s_gate_window, true);
}

static void prv_init(void) {
  // Third-party apps are left out of the firmware's touch navigation unless
  // they ask: dragging scrolls the transcript, and the action bar's icons
  // can be tapped, as on the system screens. Needs SDK 4.33 or later.
  app_touch_navigation_enable(true);
  prv_load_settings();

  app_message_register_inbox_received(prv_inbox_received);
  app_message_register_outbox_sent(prv_outbox_sent);
  app_message_register_outbox_failed(prv_outbox_failed);
  app_message_open(APP_MESSAGE_INBOX_SIZE_MINIMUM, app_message_outbox_size_maximum());

  if (launch_reason() == APP_LAUNCH_PHONE) {
    prv_gate_push();
  } else {
    prv_start();
  }
}

static void prv_deinit(void) {
  if (s_dictation) {
    dictation_session_destroy(s_dictation);
  }
  if (s_window) {
    window_destroy(s_window);
  }
  if (s_gate_window) {
    window_destroy(s_gate_window);
  }
}

int main(void) {
  prv_init();
  app_event_loop();
  prv_deinit();
}
