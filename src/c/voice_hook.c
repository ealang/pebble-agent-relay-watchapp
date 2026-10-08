#include <pebble.h>

#define TRANSCRIPT_MAX 512
#define STATUS_MAX 96

// How long the review screen waits before sending on its own, in seconds.
// Set from the phone's settings page and persisted on the watch. 0 sends
// without showing the review; REVIEW_DELAY_MANUAL never sends on its own.
#define REVIEW_DELAY_DEFAULT_S 2
#define REVIEW_DELAY_MAX_S 5
#define REVIEW_DELAY_MANUAL 255
// Keys 1 and 2 held boolean toggles in builds before the review screen; a
// stale false there would read back as 0 ("send immediately"), so they are
// retired and wiped rather than reused.
#define PERSIST_KEY_LEGACY_CONFIRMATION 1
#define PERSIST_KEY_LEGACY_ERROR_DIALOGS 2
#define PERSIST_KEY_REVIEW_DELAY 3
#define PERSIST_KEY_SENT_SCREEN 4
#define SENT_DISMISS_MS 30000
#define BAR_TICK_MS 50
#define BAR_HEIGHT 20
// Height of the box the transcript is measured in; comfortably more than
// TRANSCRIPT_MAX characters can fill at the largest font on the narrowest screen.
#define TEXT_MAX_HEIGHT 4000

static Window *s_window;
static TextLayer *s_status_layer;
static ScrollLayer *s_scroll_layer;
static TextLayer *s_transcript_layer;
static Layer *s_bar_layer;
// Button hints drawn at the screen edges, next to the buttons they describe.
static Layer *s_hints_layer;
static bool s_hint_buttons;

static char s_status_text[STATUS_MAX];
static char s_transcript[TRANSCRIPT_MAX];
static char s_quoted[TRANSCRIPT_MAX + 8];

// One line of the chosen font, measured at load. Everything sized to the text
// (status box, descender padding, scroll step) derives from this.
static int16_t s_line_height;

static uint32_t s_review_delay_s = REVIEW_DELAY_DEFAULT_S;

static AppTimer *s_bar_timer;
static uint32_t s_bar_elapsed_ms;
static bool s_reviewing;
static bool s_phone_launched;
// The countdown ran out and sent on its own: keep the transcript up, with the
// bar region turned into a "Sending..."/"Sent" footer, until Select or Back.
static bool s_auto_sending;
static bool s_sent;
// Dictation heard nothing: the review screen shows a placeholder, no footer.
static bool s_no_speech;
static const char *s_footer_text;
// Whether an unconfirmed send (countdown ran out, or no review at all) leaves
// the transcript up. Set from the phone's settings page.
static bool s_sent_screen_enabled = true;
// Any scroll on the sending/sent screen keeps it up past SENT_DISMISS_MS.
static bool s_sent_touched;
static AppTimer *s_dismiss_timer;

#if defined(PBL_MICROPHONE)
static DictationSession *s_dictation;
#endif

// Follow the watch's Text Size setting so our text matches the system UI.
static GFont prv_content_font(void) {
  switch (preferred_content_size()) {
    case PreferredContentSizeSmall:
      return fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
    case PreferredContentSizeMedium:
      return fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD);
    default:
      return fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD);
  }
}

static int16_t prv_measure_line_height(GFont font) {
  return graphics_text_layout_get_content_size("Ag", font, GRect(0, 0, 1000, 1000),
                                               GTextOverflowModeWordWrap, GTextAlignmentLeft).h;
}

static void prv_exit_callback(void *context) {
  window_stack_pop_all(false);
}

static void prv_stop_bar(void) {
  if (s_bar_timer) {
    app_timer_cancel(s_bar_timer);
    s_bar_timer = NULL;
  }
  s_footer_text = NULL;
  layer_set_hidden(s_bar_layer, true);
}

// Status text is unquoted; only dictated speech is shown in quotes.
static void prv_cancel_dismiss(void) {
  if (s_dismiss_timer) {
    app_timer_cancel(s_dismiss_timer);
    s_dismiss_timer = NULL;
  }
}

static void prv_show_status(const char *text) {
  prv_cancel_dismiss();
  s_no_speech = false;
  s_hint_buttons = false;
  layer_set_hidden(s_hints_layer, true);
  s_reviewing = false;
  s_auto_sending = false;
  s_sent = false;
  prv_stop_bar();
  strncpy(s_status_text, text, STATUS_MAX - 1);
  s_status_text[STATUS_MAX - 1] = '\0';
  text_layer_set_text(s_status_layer, s_status_text);
  layer_set_hidden(scroll_layer_get_layer(s_scroll_layer), true);
  layer_set_hidden(text_layer_get_layer(s_status_layer), false);
}

// Every failure ends here: a double vibe, and the reason stays on screen
// until Back so it can actually be read. Each message names one condition.
static void prv_fail(const char *text) {
  vibes_double_pulse();
  prv_show_status(text);
}

static const char *prv_app_message_text(AppMessageResult result) {
  static char buf[32];
  switch (result) {
    case APP_MSG_SEND_TIMEOUT:
      // The firmware waits 10 s for the phone's Ack and ignores a late one,
      // so the companion may well have queued the message.
      return "No reply from phone in 10 s (may have sent)";
    case APP_MSG_SEND_REJECTED:
      return "Phone rejected the message";
    case APP_MSG_NOT_CONNECTED:
      return "Phone not connected";
    case APP_MSG_APP_NOT_RUNNING:
      return "Phone app not running";
    case APP_MSG_BUSY:
      return "Previous send still pending";
    case APP_MSG_BUFFER_OVERFLOW:
      return "Message too large";
    default:
      snprintf(buf, sizeof(buf), "AppMessage error %d", (int) result);
      return buf;
  }
}

static void prv_show_footer(const char *text) {
  s_footer_text = text;
  layer_set_hidden(s_bar_layer, false);
  layer_mark_dirty(s_bar_layer);
}

// An auto-send keeps the transcript on screen; any other send shows a status.
static void prv_send_transcript(bool linger) {
  if (linger) {
    if (s_bar_timer) {
      app_timer_cancel(s_bar_timer);
      s_bar_timer = NULL;
    }
    s_reviewing = false;
    s_auto_sending = true;
    s_sent_touched = false;
    s_hint_buttons = false;
    layer_mark_dirty(s_hints_layer);
    prv_show_footer("Sending...");
  } else {
    prv_show_status("Sending...");
  }

  DictionaryIterator *out;
  AppMessageResult result = app_message_outbox_begin(&out);
  if (result != APP_MSG_OK) {
    prv_fail(prv_app_message_text(result));
    return;
  }
  if (dict_write_cstring(out, MESSAGE_KEY_TRANSCRIPT, s_transcript) != DICT_OK) {
    prv_fail("Transcript too large for outbox");
    return;
  }
  result = app_message_outbox_send();
  if (result != APP_MSG_OK) {
    prv_fail(prv_app_message_text(result));
  }
}

// --- Review screen -----------------------------------------------------------

static void prv_bar_update(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, GColorBlack);
  if (s_footer_text) {
    graphics_fill_rect(ctx, bounds, 0, GCornerNone);
    graphics_context_set_text_color(ctx, GColorWhite);
    graphics_draw_text(ctx, s_footer_text, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                       GRect(0, 1, bounds.size.w, bounds.size.h), GTextOverflowModeFill,
                       GTextAlignmentCenter, NULL);
    return;
  }
  int16_t width = s_review_delay_s ? bounds.size.w * s_bar_elapsed_ms / (s_review_delay_s * 1000) : 0;
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_context_set_stroke_color(ctx, GColorBlack);
  graphics_draw_rect(ctx, bounds);
  graphics_fill_rect(ctx, GRect(0, 0, width, bounds.size.h), 0, GCornerNone);
}

static void prv_bar_tick(void *context) {
  s_bar_elapsed_ms += BAR_TICK_MS;
  if (s_bar_elapsed_ms >= s_review_delay_s * 1000) {
    s_bar_timer = NULL;
    prv_send_transcript(s_sent_screen_enabled);
    return;
  }
  layer_mark_dirty(s_bar_layer);
  s_bar_timer = app_timer_register(BAR_TICK_MS, prv_bar_tick, NULL);
}

// Any interaction cancels the auto-send; the bar becomes a "Not sent" footer.
static void prv_pause_review(void) {
  if (s_footer_text) {
    return;
  }
  prv_stop_bar();
  prv_show_footer("Not sent");
}

// Lays out text in the scrollable review area and shows it with button hints.
static void prv_show_text(const char *text) {
  text_layer_set_text(s_transcript_layer, text);

  // The firmware measures text within the layer's current bounds, so grow the
  // layer back to its full measuring height first; otherwise a retake that is
  // longer than the previous transcript is clipped to the old size.
  GRect frame = layer_get_frame(text_layer_get_layer(s_transcript_layer));
  text_layer_set_size(s_transcript_layer, GSize(frame.size.w, TEXT_MAX_HEIGHT));
  GSize content = text_layer_get_content_size(s_transcript_layer);
  content.w = frame.size.w;
  content.h += s_line_height / 2;  // room for the last line's descenders
  text_layer_set_size(s_transcript_layer, content);
  // Leave room under the text so the bar never covers the last line.
  scroll_layer_set_content_size(s_scroll_layer,
      GSize(content.w, frame.origin.y + content.h + PBL_IF_ROUND_ELSE(40, BAR_HEIGHT + 4)));
  scroll_layer_set_content_offset(s_scroll_layer, GPointZero, false);

  layer_set_hidden(text_layer_get_layer(s_status_layer), true);
  layer_set_hidden(scroll_layer_get_layer(s_scroll_layer), false);
  prv_cancel_dismiss();
  s_hint_buttons = true;
  layer_set_hidden(s_hints_layer, false);
  layer_mark_dirty(s_hints_layer);
}

static void prv_show_review(void) {
  snprintf(s_quoted, sizeof(s_quoted), "“%s”", s_transcript);
  prv_show_text(s_quoted);
  s_no_speech = false;
  s_reviewing = true;
  s_auto_sending = false;
  s_sent = false;
  s_footer_text = NULL;

  s_bar_elapsed_ms = 0;
  if (s_phone_launched || s_review_delay_s == REVIEW_DELAY_MANUAL) {
    // Launched from the phone, or the user asked to always review: never
    // send without an explicit Select.
    prv_show_footer("Not sent");
  } else {
    layer_set_hidden(s_bar_layer, false);
    layer_mark_dirty(s_bar_layer);
    s_bar_timer = app_timer_register(BAR_TICK_MS, prv_bar_tick, NULL);
  }
}

static void prv_start_dictation(void);

static void prv_retake(void) {
  prv_start_dictation();
}

// Step scrolling, no animation: one text line on rectangular screens, one
// page on round ones (where the text is laid out per page).
static int16_t prv_scroll_min_y(void) {
  GRect frame = layer_get_frame(scroll_layer_get_layer(s_scroll_layer));
  GSize content = scroll_layer_get_content_size(s_scroll_layer);
#if defined(PBL_ROUND)
  // Pages must stay aligned, so the last page starts on a page boundary.
  int16_t min_y = -((content.h - 1) / frame.size.h) * frame.size.h;
#else
  int16_t min_y = frame.size.h - content.h;
  if (min_y > 0) {
    min_y = 0;
  }
#endif
  return min_y;
}

static void prv_scroll_by(int16_t dy) {
  int16_t min_y = prv_scroll_min_y();
  GPoint offset = scroll_layer_get_content_offset(s_scroll_layer);
  offset.y -= dy;
  if (offset.y < min_y) {
    offset.y = min_y;
  }
  if (offset.y > 0) {
    offset.y = 0;
  }
  scroll_layer_set_content_offset(s_scroll_layer, offset, false);
  layer_mark_dirty(s_hints_layer);
}

// Small solid triangle with its tip at (x, y), pointing dx/dy (one of them 0).
static void prv_fill_triangle(GContext *ctx, int16_t x, int16_t y, int16_t dx, int16_t dy) {
  const int16_t len = 5;
  for (int16_t i = 0; i < len; i++) {
    if (dx) {
      int16_t cx = x - dx * i;
      graphics_draw_line(ctx, GPoint(cx, y - i), GPoint(cx, y + i));
    } else {
      int16_t cy = y - dy * i;
      graphics_draw_line(ctx, GPoint(x - i, cy), GPoint(x + i, cy));
    }
  }
}

// Back records again (a record dot), Select sends or closes (a play-style
// triangle), and Up/Down arrows appear only when there is more text that way.
static void prv_hints_update(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  int16_t w = b.size.w;
  int16_t h = b.size.h;
  int16_t up_y = PBL_IF_ROUND_ELSE(50, h / 6);
  int16_t down_y = PBL_IF_ROUND_ELSE(h - 50, h * 5 / 6 - 8);
  int16_t edge_x = PBL_IF_ROUND_ELSE(w - 14, w - 3);
  graphics_context_set_stroke_color(ctx, GColorBlack);

  if (s_hint_buttons) {
    graphics_context_set_fill_color(ctx, PBL_IF_COLOR_ELSE(GColorRed, GColorBlack));
    graphics_fill_circle(ctx, GPoint(PBL_IF_ROUND_ELSE(16, 5), up_y), 3);
    prv_fill_triangle(ctx, PBL_IF_ROUND_ELSE(w - 4, w - 2), h / 2, 1, 0);
  }

  int16_t offset_y = scroll_layer_get_content_offset(s_scroll_layer).y;
  if (offset_y < 0) {
    prv_fill_triangle(ctx, edge_x, up_y - 3, 0, -1);
  }
  if (offset_y > prv_scroll_min_y()) {
    prv_fill_triangle(ctx, edge_x, down_y + 3, 0, 1);
  }
}

static int16_t prv_scroll_step(void) {
  return PBL_IF_ROUND_ELSE(layer_get_frame(scroll_layer_get_layer(s_scroll_layer)).size.h,
                           s_line_height);
}

static void prv_up_click(ClickRecognizerRef recognizer, void *context) {
  if (s_reviewing) {
    prv_pause_review();
  } else if (s_auto_sending || s_sent) {
    s_sent_touched = true;
    prv_cancel_dismiss();
  }
  prv_scroll_by(-prv_scroll_step());
}

static void prv_down_click(ClickRecognizerRef recognizer, void *context) {
  if (s_reviewing) {
    prv_pause_review();
  } else if (s_auto_sending || s_sent) {
    s_sent_touched = true;
    prv_cancel_dismiss();
  }
  prv_scroll_by(prv_scroll_step());
}

// Select pauses a running countdown like Up and Down; on a paused review it
// sends. On the sent and no-speech screens it just closes the app.
static void prv_select_click(ClickRecognizerRef recognizer, void *context) {
  if (s_bar_timer) {
    prv_pause_review();
    return;
  }
  if (s_sent || s_no_speech) {
    window_stack_pop_all(false);
    return;
  }
  if (!s_reviewing) {
    return;
  }
  prv_send_transcript(false);
}

static void prv_back_click(ClickRecognizerRef recognizer, void *context) {
  if (s_reviewing || s_sent || s_no_speech) {
    prv_retake();
  } else {
    window_stack_pop_all(true);
  }
}

static void prv_click_config(void *context) {
  window_single_repeating_click_subscribe(BUTTON_ID_UP, 150, prv_up_click);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, 150, prv_down_click);
  window_single_click_subscribe(BUTTON_ID_SELECT, prv_select_click);
  window_single_click_subscribe(BUTTON_ID_BACK, prv_back_click);
}

// --- Dictation and AppMessage ------------------------------------------------

#if defined(PBL_MICROPHONE)
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

// Nothing to send is not a failure: no vibe, and the review screen without a
// footer, so Back records again and Select closes.
static void prv_no_speech(void) {
  prv_stop_bar();
  s_reviewing = false;
  s_auto_sending = false;
  s_sent = false;
  prv_show_text("[No speech detected]");
  s_no_speech = true;
}

static void prv_dictation_callback(DictationSession *session, DictationSessionStatus status,
                                   char *transcription, void *context) {
  if (status == DictationSessionStatusSuccess) {
    if (prv_is_blank(transcription)) {
      prv_no_speech();
      return;
    }
    strncpy(s_transcript, transcription, TRANSCRIPT_MAX - 1);
    s_transcript[TRANSCRIPT_MAX - 1] = '\0';
    if (s_review_delay_s == 0 && !s_phone_launched) {
      if (s_sent_screen_enabled) {
        prv_show_review();
        prv_send_transcript(true);
      } else {
        prv_send_transcript(false);
      }
    } else {
      prv_show_review();
    }
    return;
  }
  if (status == DictationSessionStatusFailureNoSpeechDetected) {
    prv_no_speech();
    return;
  }
  if (status == DictationSessionStatusFailureTranscriptionRejected) {
    // The user backed out; nothing to report. Stay up only so a phone
    // launch does not close the settings page.
    if (s_phone_launched) {
      prv_show_status("Dictation cancelled");
    } else {
      window_stack_pop_all(false);
    }
    return;
  }
  prv_fail(prv_dictation_failure_text(status));
}
#endif

// Shows our placeholder, then asks the firmware to push its voice screen on
// top. If that refuses (Bluetooth down at this instant), no callback ever
// comes, so the refusal is reported here rather than left as "Loading...".
static void prv_start_dictation(void) {
  prv_show_status("Loading...");
#if defined(PBL_MICROPHONE)
  DictationSessionStatus status = dictation_session_start(s_dictation);
  if (status != DictationSessionStatusSuccess) {
    char text[STATUS_MAX];
    snprintf(text, sizeof(text), "Can't start. %s", prv_dictation_failure_text(status));
    prv_fail(text);
  }
#endif
}

// The companion app Acks once the transcript is durably queued on the phone.
// On success there is nothing left to say, so the app exits at once; a short
// vibe confirms it without looking. Only failures linger on screen.
static void prv_outbox_sent(DictionaryIterator *iter, void *context) {
  vibes_short_pulse();
  if (s_auto_sending) {
    s_auto_sending = false;
    s_sent = true;
    s_hint_buttons = true;
    layer_mark_dirty(s_hints_layer);
    prv_show_footer("Sent");
    if (!s_sent_touched) {
      s_dismiss_timer = app_timer_register(SENT_DISMISS_MS, prv_exit_callback, NULL);
    }
    return;
  }
  window_stack_pop_all(false);
}

static void prv_outbox_failed(DictionaryIterator *iter, AppMessageResult reason, void *context) {
  prv_fail(prv_app_message_text(reason));
}

// --- Settings ----------------------------------------------------------------

static bool prv_review_delay_is_valid(int32_t seconds) {
  return (seconds >= 0 && seconds <= REVIEW_DELAY_MAX_S) || seconds == REVIEW_DELAY_MANUAL;
}

static void prv_load_review_delay(void) {
  persist_delete(PERSIST_KEY_LEGACY_CONFIRMATION);
  persist_delete(PERSIST_KEY_LEGACY_ERROR_DIALOGS);
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

// The phone pushes the setting at every launch and whenever it is saved.
static void prv_inbox_received(DictionaryIterator *iter, void *context) {
  Tuple *sent_screen = dict_find(iter, MESSAGE_KEY_SENT_SCREEN);
  if (sent_screen) {
    s_sent_screen_enabled = sent_screen->value->int32 != 0;
    persist_write_bool(PERSIST_KEY_SENT_SCREEN, s_sent_screen_enabled);
  }
  Tuple *tuple = dict_find(iter, MESSAGE_KEY_REVIEW_DELAY);
  if (!tuple) {
    return;
  }
  int32_t seconds = tuple->value->int32;
  if (!prv_review_delay_is_valid(seconds)) {
    return;
  }
  s_review_delay_s = seconds;
  persist_write_int(PERSIST_KEY_REVIEW_DELAY, seconds);
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

  GRect text_bounds = grect_inset(bounds, GEdgeInsets(0, PBL_IF_ROUND_ELSE(0, 11)));
  s_scroll_layer = scroll_layer_create(bounds);
  scroll_layer_set_shadow_hidden(s_scroll_layer, true);
  window_set_click_config_provider(window, prv_click_config);
  // On round screens, start lower so the first line lands on a wide chord.
  s_transcript_layer = text_layer_create(
      GRect(text_bounds.origin.x, PBL_IF_ROUND_ELSE(26, 0), text_bounds.size.w, TEXT_MAX_HEIGHT));
  text_layer_set_font(s_transcript_layer, font);
  text_layer_set_background_color(s_transcript_layer, GColorClear);
  text_layer_set_text_alignment(s_transcript_layer, GTextAlignmentCenter);
  scroll_layer_add_child(s_scroll_layer, text_layer_get_layer(s_transcript_layer));
  layer_set_hidden(scroll_layer_get_layer(s_scroll_layer), true);
  layer_add_child(root, scroll_layer_get_layer(s_scroll_layer));
#if defined(PBL_ROUND)
  // Text flow needs the layer to be in the window hierarchy already.
  scroll_layer_set_paging(s_scroll_layer, true);
  text_layer_enable_screen_text_flow_and_paging(s_transcript_layer, 14);
#endif

  // On round screens the very bottom is too narrow, so the bar sits on a
  // wider chord a little higher up.
  GRect bar_frame = PBL_IF_ROUND_ELSE(
      GRect(45, bounds.size.h - 32, bounds.size.w - 90, BAR_HEIGHT),
      GRect(0, bounds.size.h - BAR_HEIGHT, bounds.size.w, BAR_HEIGHT));
  s_bar_layer = layer_create(bar_frame);
  layer_set_update_proc(s_bar_layer, prv_bar_update);
  layer_set_hidden(s_bar_layer, true);
  layer_add_child(root, s_bar_layer);

  s_hints_layer = layer_create(bounds);
  layer_set_update_proc(s_hints_layer, prv_hints_update);
  layer_set_hidden(s_hints_layer, true);
  layer_add_child(root, s_hints_layer);

}

static void prv_window_unload(Window *window) {
  prv_cancel_dismiss();
  prv_stop_bar();
  layer_destroy(s_hints_layer);
  layer_destroy(s_bar_layer);
  text_layer_destroy(s_transcript_layer);
  scroll_layer_destroy(s_scroll_layer);
  text_layer_destroy(s_status_layer);
}

static void prv_init(void) {
  s_phone_launched = launch_reason() == APP_LAUNCH_PHONE;
  prv_load_review_delay();

  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers){
    .load = prv_window_load,
    .unload = prv_window_unload,
  });
  window_stack_push(s_window, true);

  app_message_register_inbox_received(prv_inbox_received);
  app_message_register_outbox_sent(prv_outbox_sent);
  app_message_register_outbox_failed(prv_outbox_failed);
  app_message_open(APP_MESSAGE_INBOX_SIZE_MINIMUM, app_message_outbox_size_maximum());

#if defined(PBL_MICROPHONE)
  s_dictation = dictation_session_create(0, prv_dictation_callback, NULL);
  if (s_dictation) {
    // Our review screen replaces the system's confirmation and error dialogs.
    dictation_session_enable_confirmation(s_dictation, false);
    dictation_session_enable_error_dialogs(s_dictation, false);
    prv_start_dictation();
  } else if (!connection_service_peek_pebble_app_connection()) {
    // The firmware refuses to create a session unless a phone app that
    // supports app dictation is connected; this is the common reason.
    prv_fail("Phone not connected");
  } else {
    prv_fail("Dictation unavailable: phone app lacks voice support, or out of memory");
  }
#else
  prv_fail("This watch has no microphone");
#endif
}

static void prv_deinit(void) {
#if defined(PBL_MICROPHONE)
  if (s_dictation) {
    dictation_session_destroy(s_dictation);
  }
#endif
  window_destroy(s_window);
}

int main(void) {
  prv_init();
  app_event_loop();
  prv_deinit();
}
