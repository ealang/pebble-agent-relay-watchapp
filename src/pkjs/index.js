var Clay = require('@rebble/clay');
var clayConfig = require('./config.json');

// Events are handled here rather than by Clay so the select's string value
// reaches the watch as a number.
var clay = new Clay(clayConfig, null, { autoHandleEvents: false });

function sendReviewDelay(value) {
  var seconds = parseInt(value, 10);
  if (isNaN(seconds)) {
    seconds = 2;
  }
  Pebble.sendAppMessage({ REVIEW_DELAY: seconds });
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
  sendReviewDelay(settings.REVIEW_DELAY);
});

// Also push the stored setting at every launch, so a save that happened
// while the watchapp was closed still reaches the watch. The watch persists
// it; a change takes effect from the next launch.
Pebble.addEventListener('ready', function () {
  var settings = {};
  try {
    settings = JSON.parse(localStorage.getItem('clay-settings')) || {};
  } catch (err) {
    settings = {};
  }
  if (settings.REVIEW_DELAY !== undefined) {
    sendReviewDelay(settings.REVIEW_DELAY);
  }
});
