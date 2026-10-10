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
// The countdown is a rounded bar in the style of the firmware's progress bar,
// floating above the footer on a white margin so text scrolled under it does
// not touch it.
#define PILL_HEIGHT PBL_IF_COLOR_ELSE(6, 7)
#define PILL_HALO 4
#define PILL_MARGIN PBL_IF_ROUND_ELSE(49, 10)
#define BOTTOM_HEIGHT (BAR_HEIGHT + PILL_HEIGHT + 2 * PILL_HALO)
// Round screens leave the firmware's status bar height blank above the text,
// as its own dictation review does, so the first line is not cut by the curve.
#define TOP_MARGIN PBL_IF_ROUND_ELSE(STATUS_BAR_LAYER_HEIGHT, 0)
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
static Layer *s_bar_layer;
// The standard action bar, as on the firmware's dictation review, with that
// screen's own icons recoloured black. Its background is clear, so it is
// invisible except for icons, which appear only for what the buttons do.
static ActionBarLayer *s_action_bar;
static GBitmap *s_icon_check;
static GBitmap *s_icon_up;
static GBitmap *s_icon_down;

static char s_status_text[STATUS_MAX];
static char s_transcript[TRANSCRIPT_MAX];
static char s_quoted[TRANSCRIPT_MAX + 8];

// One line of the chosen font, measured at load. Everything sized to the text
// (status box, descender padding, scroll step) derives from this.
static int16_t s_line_height;

static uint32_t s_review_delay_s = REVIEW_DELAY_DEFAULT_S;

// Every screen is the same window: either centered status text, or the
// scrollable transcript with the action bar and a strip along the bottom.
// The screen decides what the strip shows and what each button does.
typedef enum {
  // Centered text: "Loading...", "Sending...", or a failure. Back exits.
  ScreenStatus,
  // The transcript with a filling progress bar; when it fills, it sends. Any
  // button but Back stops it, turning it into ScreenReview.
  ScreenCountdown,
  // The transcript, "Not yet sent". Select sends, Back records again.
  ScreenReview,
  // A countdown ran out and sent: the transcript stays up with "Sending..."
  // and then "Sent", which closes after SENT_DISMISS_MS unless scrolled.
  ScreenSending,
  ScreenSent,
  // Dictation heard nothing: a placeholder, no strip. Back records again.
  ScreenNoSpeech,
} Screen;

static Screen s_screen;
static AppTimer *s_bar_timer;
static uint32_t s_bar_elapsed_ms;
// Fixed when the countdown starts, so a setting saved meanwhile cannot
// change it (or make it zero) partway through.
static uint32_t s_bar_total_ms;
// Whether an unconfirmed send (countdown ran out, or no review at all) leaves
// the transcript up. Set from the phone's settings page.
static bool s_sent_screen_enabled = true;
// Any scroll on the sending/sent screen keeps it up past SENT_DISMISS_MS.
static bool s_sent_touched;
static AppTimer *s_dismiss_timer;

// The icons currently on the action bar, so an unchanged icon is not
// animated in again.
static const GBitmap *s_bar_icons[NUM_BUTTONS];

#if defined(PBL_MICROPHONE)
static DictationSession *s_dictation;
#endif

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

static void prv_exit_callback(void *context) {
  window_stack_pop_all(false);
}

static void prv_stop_bar_timer(void) {
  if (s_bar_timer) {
    app_timer_cancel(s_bar_timer);
    s_bar_timer = NULL;
  }
}

static void prv_cancel_dismiss(void) {
  if (s_dismiss_timer) {
    app_timer_cancel(s_dismiss_timer);
    s_dismiss_timer = NULL;
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

// The check appears only when Select would send: a paused or manual review,
// not the countdown or after a send. The countdown shows no icons at all,
// since every button there only stops it. Otherwise Up/Down arrows appear
// when there is more text that way.
static void prv_update_action_bar(void) {
  bool counting = s_screen == ScreenCountdown;
  prv_set_bar_icon(BUTTON_ID_SELECT, s_screen == ScreenReview ? s_icon_check : NULL);
  GPoint offset = scroll_layer_get_content_offset(s_scroll_layer);
  int16_t visible_h = layer_get_bounds(scroll_layer_get_layer(s_scroll_layer)).size.h;
  int16_t content_h = scroll_layer_get_content_size(s_scroll_layer).h;
  prv_set_bar_icon(BUTTON_ID_UP, !counting && offset.y < 0 ? s_icon_up : NULL);
  prv_set_bar_icon(BUTTON_ID_DOWN,
                   !counting && offset.y + content_h > visible_h ? s_icon_down : NULL);
}

// The strip's text on each screen; the countdown draws its bar instead.
static const char *prv_footer_text(void) {
  switch (s_screen) {
    case ScreenCountdown:
    case ScreenReview:
      return "Not yet sent";
    case ScreenSending:
      return "Sending...";
    case ScreenSent:
      return "Sent";
    default:
      return NULL;
  }
}

// Switches what the window shows. The countdown timer runs only on the
// countdown, and the sent screen's dismiss timer is started by its caller.
static void prv_set_screen(Screen screen) {
  s_screen = screen;
  prv_cancel_dismiss();
  if (screen != ScreenCountdown) {
    prv_stop_bar_timer();
  }
  bool status = screen == ScreenStatus || screen == ScreenNoSpeech;
  layer_set_hidden(text_layer_get_layer(s_status_layer), !status);
  layer_set_hidden(scroll_layer_get_layer(s_scroll_layer), status);
  layer_set_hidden(action_bar_layer_get_layer(s_action_bar), status);
  layer_set_hidden(s_bar_layer, screen != ScreenCountdown && !prv_footer_text());
  layer_mark_dirty(s_bar_layer);
  prv_update_action_bar();
}

// Any interaction cancels the auto-send.
static void prv_pause_review(void) {
  if (s_screen == ScreenCountdown) {
    prv_set_screen(ScreenReview);
  }
}

// Scrolling the sending/sent screen keeps it up until Select or Back.
static void prv_keep_sent_screen(void) {
  if (s_screen == ScreenSending || s_screen == ScreenSent) {
    s_sent_touched = true;
    prv_cancel_dismiss();
  }
}

// Also reached by a touch drag, which moves the content without any click:
// that stops a countdown, and keeps the sent screen up, like Up/Down do.
static void prv_scroll_offset_changed(ScrollLayer *scroll_layer, void *context) {
  if (scroll_layer_get_content_offset(scroll_layer).y != 0) {
    prv_pause_review();
  }
  prv_keep_sent_screen();
  prv_update_action_bar();
}

// Status text is unquoted; only dictated speech is shown in quotes.
static void prv_show_status(const char *text) {
  strncpy(s_status_text, text, STATUS_MAX - 1);
  s_status_text[STATUS_MAX - 1] = '\0';
  text_layer_set_text(s_status_layer, s_status_text);
  prv_set_screen(ScreenStatus);
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

// An auto-send keeps the transcript on screen; any other send shows a status.
static void prv_send_transcript(bool linger) {
  if (linger) {
    s_sent_touched = false;
    prv_set_screen(ScreenSending);
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
  const char *footer = prv_footer_text();
  if (footer) {
    GRect strip = GRect(0, BOTTOM_HEIGHT - BAR_HEIGHT, bounds.size.w,
                        bounds.size.h - (BOTTOM_HEIGHT - BAR_HEIGHT));
    graphics_context_set_fill_color(ctx, GColorBlack);
    graphics_fill_rect(ctx, strip, 0, GCornerNone);
    graphics_context_set_text_color(ctx, GColorWhite);
    strip.origin.y += 1;
    strip.size.h = BAR_HEIGHT;
    graphics_draw_text(ctx, footer, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD), strip,
                       GTextOverflowModeFill, GTextAlignmentCenter, NULL);
  }
  if (s_screen != ScreenCountdown) {
    return;
  }
  // Colours as on the firmware's dictation screen: black on dark grey, or an
  // outlined white track on black-and-white watches.
  const int16_t radius = PILL_HEIGHT / 2;
  GRect pill = GRect(PILL_MARGIN, PILL_HALO, bounds.size.w - 2 * PILL_MARGIN, PILL_HEIGHT);
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, grect_inset(pill, GEdgeInsets(-PILL_HALO)), radius + PILL_HALO,
                     GCornersAll);
  graphics_context_set_fill_color(ctx, PBL_IF_COLOR_ELSE(GColorDarkGray, GColorWhite));
  graphics_fill_rect(ctx, pill, radius, GCornersAll);
  graphics_context_set_fill_color(ctx, GColorBlack);
  GRect filled = pill;
  filled.size.w = pill.size.w * s_bar_elapsed_ms / s_bar_total_ms;
  if (filled.size.w > 0) {
    graphics_fill_rect(ctx, filled, filled.size.w < 2 * radius ? filled.size.w / 2 : radius,
                       GCornersAll);
  }
#if !defined(PBL_COLOR)
  graphics_context_set_stroke_color(ctx, GColorBlack);
  graphics_draw_round_rect(ctx, pill, radius);
#endif
}

static void prv_bar_tick(void *context) {
  s_bar_elapsed_ms += BAR_TICK_MS;
  if (s_bar_elapsed_ms >= s_bar_total_ms) {
    s_bar_timer = NULL;
    prv_send_transcript(s_sent_screen_enabled);
    return;
  }
  layer_mark_dirty(s_bar_layer);
  s_bar_timer = app_timer_register(BAR_TICK_MS, prv_bar_tick, NULL);
}

// Lays out text in the scrollable area; the caller then picks the screen.
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
  // The scroll layer ends at the footer, so the text needs no room under it.
  scroll_layer_set_content_size(s_scroll_layer,
                                GSize(content.w, frame.origin.y + content.h + 4));
  scroll_layer_set_content_offset(s_scroll_layer, GPointZero, false);
}

static void prv_show_transcript(void) {
  snprintf(s_quoted, sizeof(s_quoted), "“%s”", s_transcript);
  prv_show_text(s_quoted);
}

static void prv_show_review(void) {
  prv_show_transcript();
  if (s_review_delay_s == REVIEW_DELAY_MANUAL) {
    prv_set_screen(ScreenReview);
  } else {
    s_bar_elapsed_ms = 0;
    s_bar_total_ms = s_review_delay_s * 1000;
    prv_set_screen(ScreenCountdown);
    s_bar_timer = app_timer_register(BAR_TICK_MS, prv_bar_tick, NULL);
  }
}

static void prv_start_dictation(void);

// Up and Down scroll like the firmware's scrolling screens; during the
// countdown they only stop it.
static void prv_scroll_click(ClickRecognizerRef recognizer, ClickHandler scroll) {
  switch (s_screen) {
    case ScreenStatus:
      return;
    case ScreenCountdown:
      prv_pause_review();
      return;
    default:
      prv_keep_sent_screen();
      scroll(recognizer, s_scroll_layer);
  }
}

static void prv_up_click(ClickRecognizerRef recognizer, void *context) {
  prv_scroll_click(recognizer, scroll_layer_scroll_up_click_handler);
}

static void prv_down_click(ClickRecognizerRef recognizer, void *context) {
  prv_scroll_click(recognizer, scroll_layer_scroll_down_click_handler);
}

// Select pauses a running countdown like Up and Down; on a paused review it
// sends. On the sent and no-speech screens it just closes the app.
static void prv_select_click(ClickRecognizerRef recognizer, void *context) {
  switch (s_screen) {
    case ScreenCountdown:
      prv_pause_review();
      break;
    case ScreenReview:
      prv_send_transcript(false);
      break;
    case ScreenSent:
    case ScreenNoSpeech:
      window_stack_pop_all(false);
      break;
    default:
      break;
  }
}

// Back records again wherever there is a transcript to replace, and
// otherwise exits, including while a send is still in flight.
static void prv_back_click(ClickRecognizerRef recognizer, void *context) {
  switch (s_screen) {
    case ScreenCountdown:
    case ScreenReview:
    case ScreenSent:
    case ScreenNoSpeech:
      prv_start_dictation();
      break;
    default:
      window_stack_pop_all(true);
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
  text_layer_set_text(s_status_layer, "[No speech detected]");
  prv_set_screen(ScreenNoSpeech);
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
    if (s_review_delay_s == 0) {
      if (s_sent_screen_enabled) {
        prv_show_transcript();
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
    // The user backed out; nothing to report.
    window_stack_pop_all(false);
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

// The companion app Acks once the transcript is durably queued on the phone;
// a short vibe confirms it without looking. A send the user chose with Select
// needs no more confirmation, so the app exits at once. An auto-send keeps the
// transcript up as the sent screen, so it can be checked and redone.
static void prv_outbox_sent(DictionaryIterator *iter, void *context) {
  vibes_short_pulse();
  if (s_screen == ScreenSending) {
    prv_set_screen(ScreenSent);
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
// The phone resends the settings at every launch, so a value is written only
// when it changed (or was never stored, so a later change of default cannot
// override the user's choice), sparing the flash a write per launch.
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
  int16_t footer_top = bounds.size.h - FOOTER_LIFT - BAR_HEIGHT;
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
  // On round screens the countdown bar is inset to fit the narrower chord.
  int16_t bar_top = footer_top - (BOTTOM_HEIGHT - BAR_HEIGHT);
  s_bar_layer = layer_create(GRect(0, bar_top, bounds.size.w, bounds.size.h - bar_top));
  layer_set_update_proc(s_bar_layer, prv_bar_update);
  layer_set_hidden(s_bar_layer, true);
  layer_add_child(root, s_bar_layer);

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
  prv_cancel_dismiss();
  prv_stop_bar_timer();
  action_bar_layer_destroy(s_action_bar);
  gbitmap_destroy(s_icon_check);
  gbitmap_destroy(s_icon_up);
  gbitmap_destroy(s_icon_down);
  layer_destroy(s_bar_layer);
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
  prv_load_review_delay();

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
#if defined(PBL_MICROPHONE)
  if (s_dictation) {
    dictation_session_destroy(s_dictation);
  }
#endif
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
