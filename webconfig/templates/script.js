const mgmtReportId = 6;
var device;

const packetType = {
  keyboardReportMsg: 1, mouseReportMsg: 2, outputSelectMsg: 3, firmwareUpgradeMsg: 4, switchLockMsg: 7,
  syncBordersMsg: 8, flashLedMsg: 9, wipeConfigMsg: 10, readConfigMsg: 16, writeConfigMsg: 17, saveConfigMsg: 18,
  rebootMsg: 19, getValMsg: 20, setValMsg: 21, getValAllMsg: 22, proxyPacketMsg: 23
};

/* Matches MAX_SCREEN_COORD in src/include/screen.h */
const MAX_SCREEN_COORD = 32767;

function calcChecksum(report) {
  let checksum = 0;
  for (let i = 3; i < 11; i++)
    checksum ^= report[i];

  return checksum;
}

async function sendReport(type, payload = [], sendBoth = false) {
  if (!device || !device.opened)
    return;

  /* First send this one, if the first one gets e.g. rebooted */
  if (sendBoth) {
    var reportProxy = makeReport(type, payload, true);
    await device.sendReport(mgmtReportId, reportProxy);
    }

    var report = makeReport(type, payload, false);
    await device.sendReport(mgmtReportId, report);
}

function makeReport(type, payload, proxy=false) {
  var dataOffset = proxy ? 4 : 3;
  report = new Uint8Array([0xaa, 0x55, type, ...new Array(9).fill(0)]);

  if (proxy)
    report = new Uint8Array([0xaa, 0x55, packetType.proxyPacketMsg, type, ...new Array(7).fill(0), type]);

  if (payload) {
    report.set([...payload], dataOffset);
    report[report.length - 1] = calcChecksum(report);
  }
  return report;
}

function packValue(element, key, dataType, buffer) {
  const dataOffset = 1;
  var buffer = new ArrayBuffer(8);
  var view = new DataView(buffer);

  const methods = {
    "uint32": view.setUint32,
    "uint64": view.setUint32, /* Yes, I know. :-| */
    "int32": view.setInt32,
    "uint16": view.setUint16,
    "uint8": view.setUint8,
    "int16": view.setInt16,
    "int8": view.setInt8
  };

  if (dataType in methods) {
    const method = methods[dataType];
    if (element.type === 'checkbox')
      view.setUint8(dataOffset, element.checked ? 1 : 0, true);
    else
      method.call(view, dataOffset, element.value, true);
  }

  view.setUint8(0, key);
  return new Uint8Array(buffer);
}

/* u16 version = major * 1000 + minor + 100. Minor prints to two digits, so releases read
   v1.00, v1.01, v1.02. Zero means nothing has been heard from that board - for the other
   one that is an unpowered board or a link that is down. */
function formatFwVersion(value) {
  if (!value)
    return '—';

  const minor = String((value - 100) % 1000).padStart(2, '0');

  return `v${Math.floor((value - 100) / 1000)}.${minor}`;
}

function getValue(element) {
  if (element.type === 'checkbox')
    return element.checked ? 1 : 0;
  else
    return element.value;
}

/* Set while a value is being pushed in from the device, so that redrawing the
   controls does not count as an edit. */
var applying = false;

function setValue(element, value) {
  element.setAttribute('fetched-value', value);

  if (element.type === 'checkbox')
    element.checked = value;
  else
    element.value = value;

  applying = true;
  element.dispatchEvent(new Event('input', { bubbles: true }));
  applying = false;
}

function updateElement(key, event) {
  var dataOffset = 4;
  var element = document.querySelector(`[data-key="${key}"]`);

  if (!element)
    return;

  const methods = {
    "uint32": event.data.getUint32,
    "uint64": event.data.getUint32, /* Yes, I know. :-| */
    "int32": event.data.getInt32,
    "uint16": event.data.getUint16,
    "uint8": event.data.getUint8,
    "int16": event.data.getInt16,
    "int8": event.data.getInt8
  };

  dataType = element.getAttribute('data-type');

  if (dataType in methods) {
    var value = methods[dataType].call(event.data, dataOffset, true);
    setValue(element, value);

    if (element.hasAttribute('data-hex'))
      setValue(element, parseInt(value).toString(16));

    if (element.hasAttribute('data-fw-ver'))
      setValue(element, formatFwVersion(value));
  }
}

async function handleInputReport(event) {
  var data = new Uint8Array(event.data.buffer);
  var key = data[3];

  updateElement(key, event);
}

async function valueChangedHandler(element) {
  var key = element.getAttribute('data-key');
  var dataType = element.getAttribute('data-type');

  var origValue = element.getAttribute('fetched-value');
  var newValue = getValue(element);

  if (origValue != newValue) {
    uintBuffer = packValue(element, key, dataType);

    /* Send to both devices */
    await sendReport(packetType.setValMsg, uintBuffer, true);

    /* Set this as the current value */
    element.setAttribute('fetched-value', newValue);
  }
}

/* ---------------------------------------------------------------- device */

async function connectHandler() {
  if (device && device.opened)
    return;

  var devices = await navigator.hid.requestDevice({
    filters: [{ vendorId: 0x1209, productId: 0xc000, usagePage: 0xff00, usage: 0x10 }]
  });

  if (!devices || !devices.length)
    return;

  try {
    device = devices[0];
    await device.open();
  } catch (e) {
    /* Someone else has it open, or it went away between picking and opening. */
    device = undefined;
    return;
  }

  device.addEventListener('inputreport', handleInputReport);

  setConnected(true);
  await reloadFromDevice();
}

async function saveHandler() {
  const elements = document.querySelectorAll('.api');

  if (!device || !device.opened)
    return;

  for (const element of elements) {
    var origValue = element.getAttribute('fetched-value')

    if (element.hasAttribute('readonly'))
      continue;

    if (origValue != getValue(element))
      await valueChangedHandler(element);
  }
  await sendReport(packetType.saveConfigMsg, [], true);
  markClean();
}

async function blinkHandler() {
  await sendReport(packetType.flashLedMsg, []);
}

async function blinkBothHandler() {
  await sendReport(packetType.flashLedMsg, [], true);
}

async function wipeConfigHandler() {
  await sendReport(packetType.wipeConfigMsg, [], true);
}

async function rebootHandler() {
  await sendReport(packetType.rebootMsg);
  await closeDevice();
}

async function closeDevice() {
  try {
    if (device && device.opened)
      await device.close();
  } catch (e) { /* already gone */ }

  device = undefined;
  setConnected(false);
}

/* ------------------------------------------------------- connection gate */

var dirty = false;
var pendingExit = null;

const DISCARD_MSG = "Disconnecting discards the edits you haven't written to the device.";

function el(id) {
  return document.getElementById(id);
}

function setConnected(on) {
  const panel = el('panel');

  if (on)
    panel.removeAttribute('inert');
  else
    panel.setAttribute('inert', '');

  document.body.classList.toggle('offline', !on);
  el('not-connected').hidden = on;

  const connect = el('btn-connect');
  connect.textContent = on ? 'Disconnect' : 'Connect';
  connect.classList.toggle('btn-call', !on && ("hid" in navigator));
  connect.dataset.handler = on ? 'disconnectHandler' : 'connectHandler';

  document.querySelectorAll('.online').forEach(element => { element.style.opacity = on ? 1.0 : 0.5; });
  document.querySelectorAll('#menu-buttons .online').forEach(element => { element.disabled = !on; });

  if (!on) {
    setPlaceholders();
    markClean();
    hideGuard();
  }
}

function setPlaceholders() {
  const sum = document.querySelector('[data-hex]');

  document.querySelectorAll('[data-fw-ver]').forEach(fw => { fw.value = '—'; });
  if (sum) sum.value = '————————';
  el('ver-differ').hidden = true;
}

/* ------------------------------------------------------- export / import

   Settings are keyed by the numbers in api_field_map (src/protocol.c), which name
   a field rather than a position in the config struct. An export therefore stays
   readable across firmware versions that add, drop or reorder fields - unknown keys
   are reported and skipped rather than corrupting anything. */

const BACKUP_TAG = 'deskhop-extended-settings';

function backupFields() {
  return document.querySelectorAll('.api:not([readonly])');
}

function showBackup(title, message, text, editable) {
  el('backup-t').textContent = title;
  el('backup-m').textContent = message;
  el('backup-apply').hidden = !editable;
  el('backup-file').hidden = !editable;

  const box = el('backup-text');
  box.value = text;
  box.readOnly = !editable;
  el('backup').hidden = false;

  /* Only the import box wants the caret; an export is there to be read, not replaced. */
  if (editable)
    box.focus();
}

function closeBackupHandler() {
  el('backup').hidden = true;
  el('backup-text').value = '';
  el('backup-input').value = '';
}

function backupFilename() {
  return `deskhop-extended-settings-${new Date().toISOString().slice(0, 10)}.txt`;
}

/* Hand the text over as a file. The page is opened from the device's own USB drive over
   file://, where a download is not guaranteed to be allowed, so this is best-effort and
   the same text always lands in the textarea to copy either way. */
function offerDownload(text, filename) {
  try {
    const url = URL.createObjectURL(new Blob([text], { type: 'text/plain' }));
    const link = document.createElement('a');

    link.href = url;
    link.download = filename;
    document.body.appendChild(link);
    link.click();
    link.remove();

    /* Give the download a moment to start before the blob goes away. */
    setTimeout(() => URL.revokeObjectURL(url), 30000);
    return true;
  } catch (e) {
    return false;
  }
}

function exportHandler() {
  const settings = {};

  for (const element of backupFields())
    settings[element.dataset.key] = Number(getValue(element));

  const payload = {
    [BACKUP_TAG]: 1,
    firmware: document.querySelector('[data-fw-self]').value,
    exported: new Date().toISOString().replace(/\.\d+Z$/, 'Z'),
    settings: settings,
  };

  const text = JSON.stringify(payload, null, 2);
  const filename = backupFilename();
  const saved = offerDownload(text, filename);

  showBackup('Settings exported',
             saved ? `Saved as ${filename}. The same text is below if you would rather copy it.`
                   : 'Your browser would not save a file, so copy the text below and keep it.',
             text, false);
}

function importHandler() {
  showBackup('Import settings',
             'Choose a file you exported earlier, or paste one below, then press Apply. ' +
             'Nothing reaches the device until you Save.',
             '', true);
}

function chooseFileHandler() {
  el('backup-input').click();
}

async function backupFileChosen(event) {
  const file = event.target.files && event.target.files[0];

  if (!file)
    return;

  try {
    el('backup-text').value = await file.text();
    el('backup-m').textContent = `Loaded ${file.name}. Press Apply to put it into the page.`;
  } catch (e) {
    el('backup-m').textContent = `Could not read ${file.name}: ${e.message}`;
  }

  /* Let the same file be picked again after a Close. */
  event.target.value = '';
}

/* Set a control from imported text. Deliberately does not touch fetched-value:
   saveHandler decides what to write by comparing against it, so an imported value
   has to stay visibly different from what the device last reported. Dispatching
   input (not change) redraws the proxy controls and marks the page dirty without
   pushing anything to the device. */
function applyImported(element, value) {
  if (element.type === 'checkbox')
    element.checked = Number(value) !== 0;
  else
    element.value = value;

  element.dispatchEvent(new Event('input', { bubbles: true }));
}

function applyImportHandler() {
  let payload;

  try {
    payload = JSON.parse(el('backup-text').value);
  } catch (e) {
    el('backup-m').textContent = 'That is not valid JSON - paste the whole exported block.';
    return;
  }

  if (!payload || typeof payload.settings !== 'object' || payload.settings === null) {
    el('backup-m').textContent = 'No settings found in that block - paste the whole export.';
    return;
  }

  const unknown = [];
  let applied = 0;

  for (const [key, value] of Object.entries(payload.settings)) {
    const element = document.querySelector(`.api:not([readonly])[data-key="${CSS.escape(key)}"]`);

    if (!element) {
      unknown.push(key);
      continue;
    }

    applyImported(element, value);
    applied++;
  }

  el('backup-apply').hidden = true;
  el('backup-text').readOnly = true;
  el('backup-t').textContent = 'Import applied';
  el('backup-m').textContent =
    `${applied} setting${applied === 1 ? '' : 's'} loaded into the page` +
    (unknown.length ? `, ${unknown.length} unknown and skipped (${unknown.join(', ')})` : '') +
    '. Review them, then press Save to device.';
}

/* ------------------------------------------------------- dirty behaviour */

function markDirty() {
  if (dirty)
    return;

  dirty = true;
  updateDirty();
}

function markClean() {
  dirty = false;
  updateDirty();
}

function updateDirty() {
  const indicator = el('dirty-ind');

  indicator.textContent = dirty ? 'Unsaved changes' : 'No changes';
  indicator.classList.toggle('dirty', dirty);
  el('btn-save').disabled = !dirty;

  /* Saving answers the question the guard was asking. */
  if (!dirty)
    hideGuard();
}

/* Anything that throws away unsaved edits asks first. */
function confirmDiscard(action, message, label) {
  if (dirty && device && device.opened) {
    pendingExit = action;
    el('guard-msg').textContent = message;
    el('guard-confirm').textContent = label;
    el('confirm-disconnect').hidden = false;
    el('confirm-disconnect').scrollIntoView({ block: 'nearest' });
    return;
  }
  action();
}

function hideGuard() {
  pendingExit = null;
  el('confirm-disconnect').hidden = true;
}

async function disconnectHandler() {
  confirmDiscard(closeDevice, DISCARD_MSG, 'Discard & disconnect');
}

async function exitHandler() {
  confirmDiscard(rebootHandler, DISCARD_MSG, 'Discard & exit');
}

async function readHandler() {
  confirmDiscard(reloadFromDevice,
    'Reading replaces the form with the configuration stored on the device.',
    'Discard & reload');
}

async function reloadFromDevice() {
  if (!device || !device.opened)
    return;

  await sendReport(packetType.getValAllMsg);
  markClean();
}

function keepEditingHandler() {
  hideGuard();
}

function discardHandler() {
  const action = pendingExit;

  hideGuard();
  markClean();

  if (action)
    action();
}

/* ------------------------------------------------- custom control proxies */

/* Segmented pickers, steppers and toggles own no state: they read and write the
   hidden .api input that carries data-key / data-type / fetched-value. Plain
   fields — speeds, coordinates, checkboxes — are the .api element themselves and
   need none of this. */

function apiValue(output, name) {
  return document.querySelector(`[data-o="${output}"][data-n="${name}"]`);
}

function apiNumber(output, name, fallback) {
  const element = apiValue(output, name);
  const value = element ? parseInt(element.value, 10) : NaN;

  return isNaN(value) ? fallback : value;
}

function setApi(element, value) {
  if (!element)
    return;

  if (element.type === 'checkbox') {
    if (element.checked === value)
      return;
    element.checked = value;
  } else {
    if (String(element.value) === String(value))
      return;
    element.value = value;
  }

  /* Marks the form dirty and redraws through the shared input listener. */
  element.dispatchEvent(new Event('input', { bubbles: true }));
  valueChangedHandler(element);
}

function syncControl(element) {
  if (!element.id)
    return;

  document.querySelectorAll(`[data-for="${element.id}"]`)
    .forEach(view => renderView(view, element));

  if (element.dataset.o)
    refreshOutput(element.dataset.o);
  else if (element.dataset.n === 'dtap')
    refreshSwitching();
  else if (element.dataset.n === 'ledmode')
    refreshLed();

  if (element.hasAttribute('data-fw-ver'))
    refreshVersions();

  if (element.dataset.key === SWAP_COLUMNS_KEY)
    refreshColumnOrder();
}

function renderView(view, element) {
  const value = element.value;
  const list = view.classList;

  if (list.contains('seg')) {
    view.querySelectorAll('button').forEach(button => {
      button.setAttribute('aria-checked', button.dataset.v === String(value));
    });
  } else if (list.contains('step')) {
    const count = parseInt(value, 10) || 1;

    view.querySelector('.step-v').textContent = count;
    view.querySelector('[data-d="-1"]').disabled = count <= 1;
    view.querySelector('[data-d="1"]').disabled = count >= 3;
  } else if (list.contains('sw')) {
    view.setAttribute('aria-checked', element.checked);
  } else if (list.contains('sec')) {
    view.value = Math.round((parseInt(value, 10) || 0) / (Number(view.dataset.scale) || 1));
  } else if (list.contains('hk')) {
    /* Nothing stored means the combo the firmware was built with, which the page knows
       from form.py rather than from the device - the device only reports the zero. */
    const stored = parseInt(value, 10) || 0;

    view.textContent = comboLabel(stored || (parseInt(view.dataset.def, 10) || 0));
    view.classList.toggle('hk-set', !!stored);
  } else if (list.contains('hk-x')) {
    view.disabled = !(parseInt(value, 10) || 0);
  } else if (list.contains('swap')) {
    view.setAttribute('aria-pressed', value != 0);
  } else if (list.contains('pctv')) {
    /* Raw screen coordinates mean little on their own, so the share of the screen
       is spelled out beside them. One decimal, because the bottom of the range
       would otherwise read a flat 0%. */
    const raw = parseInt(value, 10) || 0;

    view.textContent = `${raw} (${(raw / MAX_SCREEN_COORD * 100).toFixed(1)}% of screen width)`;
  } else {
    view.textContent = value;
  }
}

/* --------------------------------------------------- per-output redrawing */

/* Key 87, config.swap_columns - which output is drawn on the left. */
const SWAP_COLUMNS_KEY = '87';

function refreshColumnOrder() {
  const input = document.querySelector(`[data-key="${SWAP_COLUMNS_KEY}"]`);

  /* One class on the container; .duo children carry order, and every lookup below is by
     data-o rather than position, so nothing else needs to know which way round it is. */
  el('panel').classList.toggle('swapped', !!(input && getValue(input) != 0));
}

/* Flag the boards running different firmware. Only meaningful once the other board has
   actually reported in - a dash means nothing has been heard from it. */
function refreshVersions() {
  const self = document.querySelector('[data-fw-self]');
  const other = document.querySelector('[data-fw-ver]:not([data-fw-self])');
  const known = other && other.value && other.value !== '—';
  const tag = el('other-tag');

  el('ver-differ').hidden = !(known && self && self.value !== other.value);

  /* The stage label is baked in from this build, so only claim it for the other board
     once that board has actually reported a version. */
  if (tag)
    tag.hidden = !known;
}

function refreshOutput(output) {
  const count = Math.max(1, Math.min(3, apiNumber(output, 'count', 1)));
  const left = apiNumber(output, 'side', 1) === 1;
  const top = Math.max(0, Math.min(MAX_SCREEN_COORD, apiNumber(output, 'bt', 0)));
  const bottom = Math.max(top, Math.min(MAX_SCREEN_COORD, apiNumber(output, 'bb', MAX_SCREEN_COORD)));
  const park = String(apiNumber(output, 'park', 0));
  const mode = String(apiNumber(output, 'mode', 0));
  const pct = raw => raw / MAX_SCREEN_COORD * 100;

  /* Screens, and the slice of the crossing screen that lines up with the other
     output. The border is stored per output rather than per monitor, so it is
     drawn on the screen at the crossing edge on behalf of the whole output. */
  const diagram = document.querySelector(`.diag[data-o="${output}"]`);
  const bandIndex = left ? count - 1 : 0;

  diagram.className = left ? 'diag diag-l' : 'diag diag-r';
  diagram.querySelectorAll('.mon').forEach((monitor, i) => {
    const band = monitor.querySelector('.aband');

    monitor.hidden = i >= count;
    monitor.querySelector('.scr').classList.toggle('scr-main', i === bandIndex);

    band.hidden = i !== bandIndex;
    band.style.top = pct(top) + '%';
    band.style.height = Math.max(2, pct(bottom - top)) + '%';
  });

  const edge = left ? 'right' : 'left';

  document.querySelector(`.cap[data-o="${output}"]`).textContent =
    `${count} ${count === 1 ? 'screen' : 'screens'} on the ${left ? 'left' : 'right'}` +
    `, cursor crosses off the ${edge} edge.`;

  /* Cursor park. src/mouse.c parks the hidden pointer at x = MAX_SCREEN_COORD,
     so the preview shows the right edge for both outputs. */
  const cursor = document.querySelector(`.cur[data-o="${output}"][data-pv="park"]`);

  cursor.style.top = (park === '0' ? 0 : park === '1' ? 83 : 41) + '%';
  cursor.classList.toggle('dim', park === '3');

  /* Keep awake */
  const preview = document.querySelector(`.pv-scr[data-o="${output}"][data-pv="awake"]`);

  preview.className = 'pv-scr' + (mode === '1' ? ' pong' : mode === '2' ? ' jitter' : '');
  preview.querySelector('.cur').classList.toggle('dimmer', mode === '0');

  note(output, 'mode', mode === '1'
    ? 'Pong bounces the pointer around the screen in absolute coordinates, moving every 5 ms.'
    : mode === '2'
      ? 'Jitter nudges the pointer a few pixels up and down once every 10 seconds.'
      : 'Nothing is sent. The computer may sleep or lock as usual.');

  const asleep = mode === '0';

  document.querySelectorAll(`.awake-part[data-o="${output}"]`).forEach(part => {
    part.classList.toggle('off', asleep);
    part.querySelectorAll('button, input').forEach(control => { control.disabled = asleep; });
  });

  note(output, 'times', asleep
    ? 'Timers apply once a keep-awake mode is selected.'
    : 'Stops after 0 means it keeps going until you come back. Stored as Idle Time ' +
      `${apiNumber(output, 'idle', 0)} μs / Max Time ${apiNumber(output, 'maxt', 0)} μs.`);
}

function note(output, name, text) {
  document.querySelector(`[data-o="${output}"][data-note="${name}"]`).textContent = text;
}

/* ----------------------------------------------- shared-setting redrawing */

/* The window and the pull-back distance only mean anything while the double-tap
   requirement is on, so they follow it. They keep their values while dimmed, so
   saveHandler still writes them and the stored config matches what is shown. */
function refreshSwitching() {
  const master = document.querySelector('.api[data-n="dtap"]');
  const off = !master || !master.checked;

  document.querySelectorAll('.dtap-part').forEach(part => {
    part.classList.toggle('off', off);
    part.querySelectorAll('button, input').forEach(control => { control.disabled = off; });
  });
}

/* A timer only means anything once the mode has given the LED a reason to go dark, so
   each one follows the mode - the same way the double-tap knobs follow their switch. */
const LED_TIMERS = {'0': [], '1': ['idle'], '2': ['switch'], '3': ['idle', 'switch']};

const LED_NOTES = {
  '0': 'The board driving the computer you are on keeps its LED lit.',
  '1': 'The LED goes dark once that computer has gone this long without a keypress or a ' +
       'mouse move, and comes back on with the next one. Switching to it starts the clock too.',
  '2': 'The LED is lit for this long after the output changes, then goes dark until the next ' +
       'switch, whatever you type in between.',
  '3': 'Either timer keeps the LED lit, so it goes dark once the computer has been quiet for ' +
       'the first and the switch is older than the second.',
};

function refreshLed() {
  const master = document.querySelector('.api[data-n="ledmode"]');
  const mode = master ? String(getValue(master)) : '0';
  const shown = LED_TIMERS[mode] || [];

  el('led-note').textContent = LED_NOTES[mode] || LED_NOTES['0'];

  document.querySelectorAll('.led-part').forEach(part => {
    const off = shown.indexOf(part.dataset.led) === -1;

    part.classList.toggle('off', off);
    part.querySelectorAll('button, input').forEach(control => { control.disabled = off; });
  });
}

function useFullEdge(output) {
  setApi(apiValue(output, 'bt'), 0);
  setApi(apiValue(output, 'bb'), MAX_SCREEN_COORD);
}

/* --------------------------------------------------------------- shortcuts */

/* event.code -> HID usage. The firmware matches the usage the keyboard sends, so what
   counts is the physical key and not what the host layout makes of it - which is the
   point for anyone typing on a custom one. A key missing from here cannot go into a
   shortcut, because there would be no number to store for it. */
const KEY_CODES = {
  Enter: 0x28, Escape: 0x29, Backspace: 0x2a, Tab: 0x2b, Space: 0x2c, Minus: 0x2d,
  Equal: 0x2e, BracketLeft: 0x2f, BracketRight: 0x30, Backslash: 0x31, Semicolon: 0x33,
  Quote: 0x34, Backquote: 0x35, Comma: 0x36, Period: 0x37, Slash: 0x38, CapsLock: 0x39,
  PrintScreen: 0x46, ScrollLock: 0x47, Pause: 0x48, Insert: 0x49, Home: 0x4a,
  PageUp: 0x4b, Delete: 0x4c, End: 0x4d, PageDown: 0x4e, ArrowRight: 0x4f,
  ArrowLeft: 0x50, ArrowDown: 0x51, ArrowUp: 0x52, NumLock: 0x53,
};

/* The runs the HID tables lay out consecutively, rather than 60 more lines of them. */
for (let i = 0; i < 26; i++)
  KEY_CODES['Key' + String.fromCharCode(65 + i)] = 0x04 + i;

for (let i = 1; i <= 9; i++)
  KEY_CODES['Digit' + i] = 0x1d + i;

KEY_CODES.Digit0 = 0x27;

for (let i = 1; i <= 12; i++) {
  KEY_CODES['F' + i] = 0x39 + i;         /* F1 - F12  are 0x3a - 0x45 */
  KEY_CODES['F' + (i + 12)] = 0x67 + i;  /* F13 - F24 are 0x68 - 0x73 */
}

/* Modifier bits, matching KEYBOARD_MODIFIER_* in TinyUSB and the mask config_t stores. */
const MOD_CODES = {
  ControlLeft: 0x01, ShiftLeft: 0x02, AltLeft: 0x04, MetaLeft: 0x08,
  ControlRight: 0x10, ShiftRight: 0x20, AltRight: 0x40, MetaRight: 0x80,
};

const MOD_NAMES = ['Left Ctrl', 'Left Shift', 'Left Alt', 'Left Gui',
                   'Right Ctrl', 'Right Shift', 'Right Alt', 'Right Gui'];

const KEY_NAMES = {
  0x28: 'Enter', 0x29: 'Esc', 0x2a: 'Backspace', 0x2b: 'Tab', 0x2c: 'Space', 0x2d: '-',
  0x2e: '=', 0x2f: '[', 0x30: ']', 0x31: '\\', 0x33: ';', 0x34: "'", 0x35: '`',
  0x36: ',', 0x37: '.', 0x38: '/', 0x39: 'Caps Lock', 0x46: 'Print Screen',
  0x47: 'Scroll Lock', 0x48: 'Pause', 0x49: 'Insert', 0x4a: 'Home', 0x4b: 'Page Up',
  0x4c: 'Delete', 0x4d: 'End', 0x4e: 'Page Down', 0x4f: 'Right', 0x50: 'Left',
  0x51: 'Down', 0x52: 'Up', 0x53: 'Num Lock',
};

function keyLabel(usage) {
  if (KEY_NAMES[usage])
    return KEY_NAMES[usage];

  if (usage >= 0x04 && usage <= 0x1d)
    return String.fromCharCode(65 + usage - 0x04);

  if (usage >= 0x1e && usage <= 0x26)
    return String(usage - 0x1d);

  if (usage === 0x27)
    return '0';

  if (usage >= 0x3a && usage <= 0x45)
    return 'F' + (usage - 0x39);

  if (usage >= 0x68 && usage <= 0x73)
    return 'F' + (usage - 0x67 + 12);

  /* Something the device holds that this page has no name for - show it as it is
     stored rather than pretending the shortcut is unset. */
  return '0x' + usage.toString(16);
}

/* A combo packed the way config_t stores it: modifier mask, then up to two keys.
   See HOTKEY_PACK in src/include/keyboard.h. */
function comboLabel(packed) {
  const parts = [];

  MOD_NAMES.forEach((name, bit) => {
    if (packed & (1 << bit))
      parts.push(name);
  });

  [(packed >> 8) & 0xff, (packed >> 16) & 0xff].forEach(usage => {
    if (usage)
      parts.push(keyLabel(usage));
  });

  return parts.length ? parts.join(' + ') : 'Not set';
}

var capturing = null;
var captureMods = 0;
var captureKeys = [];

function packCapture() {
  return captureMods | ((captureKeys[0] || 0) << 8) | ((captureKeys[1] || 0) << 16);
}

function startCapture(button) {
  stopCapture(false);
  closeHotkeyEditor();

  capturing = button;
  captureMods = 0;
  captureKeys = [];

  button.classList.add('hk-cap');
  button.textContent = 'Press the combination';
}

function stopCapture(commit) {
  const button = capturing;

  if (!button)
    return;

  capturing = null;
  button.classList.remove('hk-cap');

  if (commit && (captureMods || captureKeys.length))
    setApi(el(button.dataset.for), packCapture());

  /* Unconditionally, because setApi says nothing when the combo has not changed and
     the button would be left reading "Press the combination". */
  renderView(button, el(button.dataset.for));
}

function captureKeydown(event) {
  if (!capturing)
    return;

  event.preventDefault();

  /* Esc on its own backs out. Held with anything else it is just another key. */
  if (event.code === 'Escape' && !captureMods && !captureKeys.length)
    return stopCapture(false);

  const bit = MOD_CODES[event.code];

  if (bit)
    captureMods |= bit;
  else {
    const usage = KEY_CODES[event.code];

    /* No HID usage to store - ignore it rather than record something the device
       could never match. */
    if (!usage)
      return;

    if (captureKeys.indexOf(usage) === -1 && captureKeys.length < 2)
      captureKeys.push(usage);
  }

  capturing.textContent = comboLabel(packCapture());
}

/* Letting go of anything ends it, so a combo is recorded as it is released rather
   than growing for as long as the keyboard is held down. */
function captureKeyup(event) {
  if (!capturing)
    return;

  event.preventDefault();
  stopCapture(true);
}

/* Setting a combination without pressing it.

   The browser keeps a few combinations for itself - Ctrl+Shift+Tab and Ctrl+W among them -
   and a page never sees the keydown, so recording cannot reach them. Pick opens this on the
   combination as it stands and lets it be assembled instead. */
var editing = null;

function fillKeyOptions(select) {
  const usages = [...new Set(Object.values(KEY_CODES))].sort((a, b) => a - b);

  select.appendChild(new Option('None', '0'));
  usages.forEach(usage => select.appendChild(new Option(keyLabel(usage), String(usage))));
}

function openHotkeyEditor(pick) {
  stopCapture(false);

  editing = el(pick.dataset.hk);

  /* What the row shows: the stored combination, or the compiled-in one it is sitting on. */
  const view = document.querySelector(`.hk[data-for="${editing.id}"]`);
  const packed = (parseInt(editing.value, 10) || 0) || (parseInt(view.dataset.def, 10) || 0);

  el('hk-ed-t').textContent = pick.dataset.name;

  document.querySelectorAll('#hk-mods button').forEach(button => {
    button.setAttribute('aria-pressed', String(!!(packed & Number(button.dataset.m))));
  });

  el('hk-k1').value = String((packed >> 8) & 0xff);
  el('hk-k2').value = String((packed >> 16) & 0xff);
  el('hk-ed').hidden = false;
  el('hk-ed').scrollIntoView({ block: 'nearest' });
}

function applyHotkeyEditor() {
  if (!editing)
    return;

  let packed = (Number(el('hk-k1').value) || 0) << 8 | (Number(el('hk-k2').value) || 0) << 16;

  document.querySelectorAll('#hk-mods button').forEach(button => {
    if (button.getAttribute('aria-pressed') === 'true')
      packed |= Number(button.dataset.m);
  });

  /* Nothing chosen is the same thing Default says: fall back to the compiled-in combo. */
  setApi(editing, packed);
  closeHotkeyEditor();
}

function closeHotkeyEditor() {
  editing = null;
  el('hk-ed').hidden = true;
}

/* ------------------------------------------------------------- listeners */

function menuClick(event) {
  const button = event.target.closest('[data-handler]');

  if (!button || button.disabled)
    return;

  /* Handlers are async, so a throw becomes an unhandled rejection that goes nowhere
     unless devtools happen to be open - the button simply looks inert. Surface it
     instead; that is exactly how the Bootloader button stayed broken. */
  const label = (button.textContent || button.dataset.handler).trim();

  try {
    Promise.resolve(window[button.dataset.handler]())
      .catch(error => reportHandlerError(label, error));
  } catch (error) {
    reportHandlerError(label, error);
  }
}

function reportHandlerError(label, error) {
  const message = (error && error.message) ? error.message : String(error);

  showBackup(`${label} failed`,
             'The page hit an error carrying that out. Details below.',
             String((error && error.stack) ? error.stack : message), false);
}

function panelClick(event) {
  const button = event.target.closest('button');

  if (!button || button.disabled)
    return;

  /* Reaching for anything else abandons a shortcut being recorded. */
  if (capturing && button !== capturing)
    stopCapture(false);

  if (button.classList.contains('hk'))
    return startCapture(button);

  if (button.classList.contains('hk-p'))
    return openHotkeyEditor(button);

  if (button.classList.contains('hk-x'))
    return setApi(el(button.dataset.for), 0);

  if (button.dataset.m)
    return button.setAttribute('aria-pressed',
                               String(button.getAttribute('aria-pressed') !== 'true'));

  if (button.id === 'hk-ed-set')
    return applyHotkeyEditor();

  if (button.id === 'hk-ed-cancel')
    return closeHotkeyEditor();

  const group = button.closest('.seg');
  if (group)
    return setApi(el(group.dataset.for), button.dataset.v);

  const stepper = button.closest('.step');
  if (stepper) {
    const element = el(stepper.dataset.for);
    const count = (parseInt(element.value, 10) || 1) + Number(button.dataset.d);

    return setApi(element, Math.max(1, Math.min(3, count)));
  }

  if (button.classList.contains('sw')) {
    const element = el(button.dataset.for);

    return setApi(element, !element.checked);
  }

  if (button.classList.contains('swap')) {
    const element = el(button.dataset.for);

    return setApi(element, getValue(element) != 0 ? 0 : 1);
  }

  if (button.dataset.full)
    return useFullEdge(button.dataset.full);
}

/* Alignment coordinates are typed straight into the .api field. screen_from_y()
   in src/mouse.c divides by (bottom - top), so the two are never allowed to meet.
   Runs in the capture phase, ahead of the field's own onchange, so the value is
   corrected before it is sent. */
const MIN_ALIGN_GAP = 328; /* ~1% of the screen height */

function coordChanged(event) {
  const field = event.target;

  if (!field.classList.contains('coord'))
    return;

  const output = field.dataset.o;
  const other = field.dataset.n === 'bt' ? 'bb' : 'bt';
  const limit = apiNumber(output, other, field.dataset.n === 'bt' ? MAX_SCREEN_COORD : 0);
  var value = Math.round(Number(field.value) || 0);

  if (field.dataset.n === 'bt')
    value = Math.max(0, Math.min(value, limit - MIN_ALIGN_GAP));
  else
    value = Math.min(MAX_SCREEN_COORD, Math.max(value, limit + MIN_ALIGN_GAP));

  if (String(value) !== field.value) {
    field.value = value;
    field.dispatchEvent(new Event('input', { bubbles: true }));
  }
}

/* Seconds in the form, whatever the field holds on the wire - data-scale says what one
   second is worth there. The screensaver timers are µs in a uint32, which is where the
   4200 second ceiling comes from; the LED timeout is already seconds. */
function secondsChanged(event) {
  const field = event.target;

  if (!field.classList.contains('sec'))
    return;

  const scale = Number(field.dataset.scale) || 1;
  const floor = Number(field.min) || 0;
  const ceiling = Number(field.max) || 4200;
  const seconds = Math.max(floor, Math.min(ceiling, Math.round(Number(field.value) || 0)));

  field.value = seconds;
  setApi(el(field.dataset.for), seconds * scale);
}

window.addEventListener('load', function () {
  if (!("hid" in navigator)) {
    document.getElementById('warning').style.display = 'block';

    /* Nothing to connect to, so stop the button inviting the click. */
    const connect = el('btn-connect');
    connect.disabled = true;
    connect.classList.remove('btn-call');
  }

  document.querySelectorAll('.menu-buttons').forEach(function (container) {
    container.addEventListener('click', menuClick);
  });

  el('backup-input').addEventListener('change', backupFileChosen);

  const panel = el('panel');

  panel.addEventListener('click', panelClick);
  panel.addEventListener('change', coordChanged, true);
  panel.addEventListener('change', secondsChanged);

  /* Redraw on every value change, whether it came from the device or an edit.
     Only edits mark the form dirty. */
  document.addEventListener('input', function (event) {
    const element = event.target;

    if (!element.classList || !element.classList.contains('api'))
      return;

    syncControl(element);

    if (!applying)
      markDirty();
  });

  if ("hid" in navigator)
    navigator.hid.addEventListener('disconnect', function (event) {
      if (event.device === device)
        closeDevice();
    });

  fillKeyOptions(el('hk-k1'));
  fillKeyOptions(el('hk-k2'));

  window.addEventListener('keydown', captureKeydown, true);
  window.addEventListener('keyup', captureKeyup, true);

  /* A shortcut half recorded when the window loses focus is not a shortcut. */
  window.addEventListener('blur', function () { stopCapture(false); });

  document.querySelectorAll('.api').forEach(syncControl);
  refreshColumnOrder();
  refreshVersions();
  refreshOutput('A');
  refreshOutput('B');
  refreshSwitching();
  refreshLed();
  setConnected(false);
});
