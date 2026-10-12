var Clay = require('@rebble/clay');
var clayConfig = require('./config.json');

// Events are handled here rather than by Clay so the select's string value
// and the toggle's boolean reach the watch as numbers.
var clay = new Clay(clayConfig, configurePage, { autoHandleEvents: false });

// Runs inside the settings page (Clay passes it as source, so it must not
// use anything from this file).
//
// Without a countdown every send is a Select press, so the sent screen
// option would do nothing; hide it.
//
// The Save button is hidden until a value differs from what the page was
// opened with, so an unsaved change is visible as a button that has
// appeared. Saving closes the page, so the button never needs hiding again.
function configurePage() {
  var clayConfig = this;
  clayConfig.on(clayConfig.EVENTS.AFTER_BUILD, function () {
    var delay = clayConfig.getItemByMessageKey('REVIEW_DELAY');
    var sentScreen = clayConfig.getItemByMessageKey('SENT_SCREEN');
    var save = clayConfig.getItemsByType('submit')[0];
    var opened = JSON.stringify(clayConfig.serialize());

    function updateSentScreen() {
      if (delay.get() === '255') {
        sentScreen.hide();
      } else {
        sentScreen.show();
      }
    }
    function updateSave() {
      if (JSON.stringify(clayConfig.serialize()) === opened) {
        save.hide();
      } else {
        save.show();
      }
    }

    delay.on('change', updateSentScreen);
    delay.on('change', updateSave);
    sentScreen.on('change', updateSave);
    updateSentScreen();
    updateSave();
  });
}

// A failed push is only logged: the settings page has already closed, and
// the stored values are pushed again at the next launch anyway.
function sendSettings(settings) {
  var seconds = parseInt(settings.REVIEW_DELAY, 10);
  if (isNaN(seconds)) {
    seconds = 2;
  }
  var message = { REVIEW_DELAY: seconds };
  if (settings.SENT_SCREEN !== undefined) {
    message.SENT_SCREEN = settings.SENT_SCREEN ? 1 : 0;
  }
  Pebble.sendAppMessage(message, null, function (e) {
    console.log('Settings did not reach the watch: ' + JSON.stringify(e && e.error));
  });
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
