var Clay = require('@rebble/clay');
var clayConfig = require('./config.json');
var clay = new Clay(clayConfig, null, { autoHandleEvents: false });

var HEADER_SLOTS = 4;

var settings = loadSettings();

function sendResult(status, response) {
  Pebble.sendAppMessage(
    {
      STATUS: String(status),
      RESPONSE: response
    },
    function() {
      console.log('Sent response to watch');
    },
    function(err) {
      console.log('Failed to send response: ' + JSON.stringify(err));
    }
  );
}

function unwrapClayValue(value) {
  if (value && typeof value === 'object' && Object.prototype.hasOwnProperty.call(value, 'value')) {
    return value.value;
  }
  return value;
}

function normalizeText(value) {
  value = unwrapClayValue(value);
  if (value === null || value === undefined) {
    return '';
  }
  return String(value).trim();
}

function normalizeHeaders(raw) {
  var headers = {};
  if (!raw || typeof raw !== 'object') {
    return headers;
  }
  Object.keys(raw).forEach(function(name) {
    var cleanName = String(name).trim();
    var cleanValue = normalizeText(raw[name]);
    if (cleanName && cleanValue) {
      headers[cleanName] = cleanValue;
    }
  });
  return headers;
}

function loadSettings() {
  try {
    var stored = localStorage.getItem('webhook_settings');
    if (!stored) {
      return { url: '', headers: {} };
    }
    var parsed = JSON.parse(stored);
    var loaded = {
      url: normalizeText(parsed.url),
      headers: normalizeHeaders(parsed.headers)
    };
    // Migrate the pre-headers schema, which stored a bare bearer token.
    if (parsed.token && !Object.keys(loaded.headers).length) {
      loaded.headers.Authorization = 'Bearer ' + normalizeText(parsed.token);
    }
    return loaded;
  } catch (err) {
    console.log('Failed to read settings: ' + err.message);
    return { url: '', headers: {} };
  }
}

function saveSettings(next) {
  settings = {
    url: normalizeText(next.url),
    headers: normalizeHeaders(next.headers)
  };
  localStorage.setItem('webhook_settings', JSON.stringify(settings));
}

function settingsToClayMap(current) {
  var map = { WEBHOOK_URL: current.url };
  var names = Object.keys(current.headers);
  for (var i = 0; i < HEADER_SLOTS; i++) {
    map['HEADER' + (i + 1) + '_NAME'] = names[i] || '';
    map['HEADER' + (i + 1) + '_VALUE'] = names[i] ? current.headers[names[i]] : '';
  }
  return map;
}

function settingsFromClayMap(map) {
  var headers = {};
  for (var i = 0; i < HEADER_SLOTS; i++) {
    var name = normalizeText(map['HEADER' + (i + 1) + '_NAME']);
    var value = normalizeText(map['HEADER' + (i + 1) + '_VALUE']);
    if (name && value) {
      headers[name] = value;
    }
  }
  return {
    url: normalizeText(map.WEBHOOK_URL),
    headers: headers
  };
}

// PKJS has no FormData. Every part here is text, so the body is a plain string
// and is byte-identical to what a real multipart encoder would produce.
function buildMultipartBody(boundary, fields) {
  var body = '';
  Object.keys(fields).forEach(function(name) {
    body += '--' + boundary + '\r\n';
    body += 'Content-Disposition: form-data; name="' + name + '"\r\n\r\n';
    body += fields[name] + '\r\n';
  });
  body += '--' + boundary + '--\r\n';
  return body;
}

function postTranscript(transcript) {
  if (!settings.url) {
    sendResult('CFG', 'Set URL in Settings');
    return;
  }

  var boundary = '----voicehook' + Date.now();
  var body = buildMultipartBody(boundary, {
    transcription: transcript,
    recordedAt: String(Date.now()),
    client: 'watch'
  });

  var req = new XMLHttpRequest();
  req.open('POST', settings.url, true);
  req.timeout = 15000;
  Object.keys(settings.headers).forEach(function(name) {
    if (name.toLowerCase() === 'content-type') {
      return;  // the multipart boundary must win
    }
    req.setRequestHeader(name, settings.headers[name]);
  });
  req.setRequestHeader('Content-Type', 'multipart/form-data; boundary=' + boundary);

  req.onload = function() {
    if (req.readyState !== 4) {
      return;
    }
    if (req.status >= 200 && req.status < 300) {
      sendResult(req.status, 'Sent');
    } else {
      sendResult(req.status, 'HTTP ' + req.status);
    }
  };

  req.onerror = function() {
    sendResult('ERR', 'Network error');
  };

  req.ontimeout = function() {
    sendResult('ERR', 'No response');
  };

  req.send(body);
}

Pebble.addEventListener('ready', function() {
  settings = loadSettings();
  clay.setSettings(settingsToClayMap(settings));
  console.log('PebbleKit JS ready');
});

Pebble.addEventListener('showConfiguration', function() {
  Pebble.openURL(clay.generateUrl());
});

Pebble.addEventListener('webviewclosed', function(e) {
  if (!e || !e.response) {
    return;
  }

  try {
    var config = clay.getSettings(e.response, false);
    saveSettings(settingsFromClayMap(config));
    clay.setSettings(settingsToClayMap(settings));
  } catch (err) {
    console.log('Invalid config response: ' + err.message);
  }
});

Pebble.addEventListener('appmessage', function(e) {
  var payload = e.payload || {};
  if (typeof payload.TRANSCRIPT !== 'string') {
    return;
  }
  postTranscript(payload.TRANSCRIPT);
});
