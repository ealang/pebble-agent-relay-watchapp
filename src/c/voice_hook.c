#include <pebble.h>

#define TRANSCRIPT_MAX 512
#define STATUS_MAX 64

// Persisted settings, pushed from the config page via AppMessage. Both
// default to enabled until the user says otherwise.
#define PERSIST_KEY_CONFIRMATION 1
#define PERSIST_KEY_ERROR_DIALOGS 2

static Window *s_window;
static TextLayer *s_status_layer;
static char s_status_text[STATUS_MAX];
static char s_transcript[TRANSCRIPT_MAX];

#if defined(PBL_MICROPHONE)
static DictationSession *s_dictation;
#endif

static void prv_set_status(const char *text) {
  strncpy(s_status_text, text, STATUS_MAX - 1);
  s_status_text[STATUS_MAX - 1] = '\0';
  text_layer_set_text(s_status_layer, s_status_text);
}

static void prv_exit_soon_callback(void *context) {
  window_stack_pop_all(false);
}

static void prv_exit_soon(void) {
  app_timer_register(2000, prv_exit_soon_callback, NULL);
}

static void prv_send_transcript(void) {
  DictionaryIterator *out;
  AppMessageResult result = app_message_outbox_begin(&out);
  if (result != APP_MSG_OK) {
    prv_set_status("Phone unreachable");
    prv_exit_soon();
    return;
  }

  dict_write_cstring(out, MESSAGE_KEY_TRANSCRIPT, s_transcript);
  if (app_message_outbox_send() != APP_MSG_OK) {
    prv_set_status("Phone unreachable");
    prv_exit_soon();
    return;
  }

  prv_set_status("Sending...");
}

#if defined(PBL_MICROPHONE)
static void prv_dictation_callback(DictationSession *session, DictationSessionStatus status,
                                   char *transcription, void *context) {
  if (status != DictationSessionStatusSuccess) {
    // The system already showed its own error dialog for this.
    window_stack_pop_all(false);
    return;
  }

  strncpy(s_transcript, transcription, TRANSCRIPT_MAX - 1);
  s_transcript[TRANSCRIPT_MAX - 1] = '\0';
  prv_send_transcript();
}
#endif

static bool prv_setting(uint32_t key) {
  return persist_exists(key) ? persist_read_bool(key) : true;
}

// Settings arrive from the config page; persist them for the next launch.
static void prv_inbox_received(DictionaryIterator *iter, void *context) {
  Tuple *tuple = dict_find(iter, MESSAGE_KEY_CONFIRMATION);
  if (tuple) {
    persist_write_bool(PERSIST_KEY_CONFIRMATION, tuple->value->int32 != 0);
  }
  tuple = dict_find(iter, MESSAGE_KEY_ERROR_DIALOGS);
  if (tuple) {
    persist_write_bool(PERSIST_KEY_ERROR_DIALOGS, tuple->value->int32 != 0);
  }
}

// The companion app Acks once the transcript is durably queued on the phone.
// Vibes mirror the on-screen status so the outcome is clear without looking.
static void prv_outbox_sent(DictionaryIterator *iter, void *context) {
  vibes_short_pulse();
  prv_set_status("Queued");
  prv_exit_soon();
}

static void prv_outbox_failed(DictionaryIterator *iter, AppMessageResult reason, void *context) {
  vibes_double_pulse();
  prv_set_status("Send failed");
  prv_exit_soon();
}

static void prv_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);

  s_status_layer = text_layer_create(GRect(4, bounds.size.h / 2 - 24, bounds.size.w - 8, 48));
  text_layer_set_text_alignment(s_status_layer, GTextAlignmentCenter);
  text_layer_set_font(s_status_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_background_color(s_status_layer, GColorClear);
  layer_add_child(root, text_layer_get_layer(s_status_layer));

  prv_set_status("Listening...");
}

static void prv_window_unload(Window *window) {
  text_layer_destroy(s_status_layer);
}

static void prv_init(void) {
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
    // A phone launch (settings page or CoreApp's "Start app") is always
    // interactive: confirmation and error dialogs stay on so the app never
    // sends-and-exits on its own — an exit would tear down PKJS and close
    // the settings page with it. Eyes-free mode is for wrist launches only.
    bool phone_launched = launch_reason() == APP_LAUNCH_PHONE;
    dictation_session_enable_confirmation(
        s_dictation, phone_launched || prv_setting(PERSIST_KEY_CONFIRMATION));
    dictation_session_enable_error_dialogs(
        s_dictation, phone_launched || prv_setting(PERSIST_KEY_ERROR_DIALOGS));
    dictation_session_start(s_dictation);
  } else {
    prv_set_status("No microphone");
    prv_exit_soon();
  }
#else
  prv_set_status("No microphone");
  prv_exit_soon();
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
