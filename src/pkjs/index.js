var Clay = require('@rebble/clay');
var clayConfig = require('./config.json');

// Events are handled here rather than by Clay so the select's string value
// and the toggle's boolean reach the watch as numbers.
var clay = new Clay(clayConfig, null, { autoHandleEvents: false });

function sendSettings(settings) {
  var seconds = parseInt(settings.REVIEW_DELAY, 10);
  if (isNaN(seconds)) {
    seconds = 2;
  }
  var message = { REVIEW_DELAY: seconds };
  if (settings.SENT_SCREEN !== undefined) {
    message.SENT_SCREEN = settings.SENT_SCREEN ? 1 : 0;
  }
  Pebble.sendAppMessage(message);
}

Pebble.addEventListener('showConfiguration', function () {
  Pebble.openURL(clay.generateUrl());
});

Pebble.addEventListener('webviewclosed', function (e) {
  if (!e || !e.response) {
    return;
  }
  // getSettings also stores the values for generateUrl and for the next launch.
  var settings = clay.getSettings(e.response);
  sendSettings(settings);
});

// Also push the stored setting at every launch, so a save that happened
// while the watchapp was closed still reaches the watch. The watch persists
// it and applies it from the next recording.
Pebble.addEventListener('ready', function () {
  var settings = {};
  try {
    settings = JSON.parse(localStorage.getItem('clay-settings')) || {};
  } catch (err) {
    settings = {};
  }
  if (settings.REVIEW_DELAY !== undefined || settings.SENT_SCREEN !== undefined) {
    sendSettings(settings);
  }
});
