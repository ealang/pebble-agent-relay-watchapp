var Clay = require('@rebble/clay');
var clayConfig = require('./config.json');

// Clay handles showConfiguration/webviewclosed and sends new settings to the
// watch when the page is saved.
new Clay(clayConfig);

// Also push the stored settings at every launch, so a save that happened
// while the watchapp was closed still reaches the watch. The watch persists
// them; a change takes effect from the next launch.
Pebble.addEventListener('ready', function () {
  var settings = {};
  try {
    settings = JSON.parse(localStorage.getItem('clay-settings')) || {};
  } catch (e) {
    settings = {};
  }
  Pebble.sendAppMessage({
    CONFIRMATION: settings.CONFIRMATION === false ? 0 : 1,
    ERROR_DIALOGS: settings.ERROR_DIALOGS === false ? 0 : 1
  });
});
