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

// Push the settings to the watch, which persists them. A failed push is only
// logged: the settings page has already closed, so there is no UI to show an
// error in. The firmware Nacks a push while the watchapp is not running, so
// a save made after backing out of the watchapp's gate screen is lost on the
// watch side; opening the settings page again launches the app and a new
// save reaches it.
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
  // getSettings also stores the values for generateUrl. The second argument
  // asks for the raw, name-keyed values: the default return value is keyed
  // by message key number, which sendSettings cannot read.
  var settings = clay.getSettings(e.response, false);
  sendSettings(settings);
});
