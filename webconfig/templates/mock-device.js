/* ---------------------------------------------------------------- emulated device

   Test page only. config-test.htm is the same markup and the same script.js as the
   page that ships, with this file appended - render.py includes it only when it is
   asked for the test build, so nothing here can reach config.htm or the disk image.

   It stands in for navigator.hid: a device that answers the management reports the
   page sends, holds the values it is told to hold, and reports them back. That makes
   the whole page reachable without hardware - Connect, Read, Save, Export, Import,
   Blink and Reboot all do what they would do against a board.

   It is also a protocol check. Every report the page sends is decoded here the way
   the firmware decodes it and written to a log with its raw bytes, and the trailing
   XOR checksum is verified rather than assumed, so a packet the firmware would drop
   shows up as a red row instead of as a button that silently does nothing. */

(function () {
  'use strict';

  /* key -> {name, default}, straight from form.py, so this file never carries a second
     copy of the field map. */
  const FIELDS = {{ api_fields|tojson }};

  const PACKET_LEN = 12;      /* 0xaa 0x55, type, 8 data bytes, checksum */
  const KEY_OFFSET = 3;       /* where data[0] lands in a direct report */

  /* This tree's version, as the uint16 the boards exchange. The checksum is invented -
     the page only ever prints it - but it is fixed, so it does not churn screenshots. */
  const FW_VERSION = {{ build.raw }};
  const CHECKSUM = 0x7d3ed9ba;

  /* What the emulated boards start out holding: a plausible saved configuration, not a
     factory-fresh one. The seed is form.py's default column, which is the page's own
     metadata and does not everywhere agree with the firmware's real defaults in
     src/defaults.c - where they differ, or where form.py has nothing to say, these win. */
  const SAMPLE = {
    15: 32767, 45: 32767,          /* both outputs aligned over their whole edge */
    41: 2, 47: 2,                  /* output B: two screens, sitting on the right */
    46: 3,                         /* and running Windows */
    87: 0,                         /* output A drawn on the left, as src/defaults.c has it */
    /* Edge double-tap is left alone: SWITCH_DOUBLE_TAP_ENABLE is 0, so a board arrives
       with it off and so does this one. Its preview sits still until you turn it on,
       which is the gating doing its job rather than a page that failed to animate. */
    78: FW_VERSION, 86: FW_VERSION, 79: CHECKSUM,
  };

  /* The two boards. B is only ever written through proxy packets, which is exactly
     what the real second board sees. */
  const boards = { A: new Map(), B: new Map() };
  const saved = { A: new Map(), B: new Map() };

  const WRITE = {
    uint8:  (v, view, at) => view.setUint8(at, v),
    int8:   (v, view, at) => view.setInt8(at, v),
    uint16: (v, view, at) => view.setUint16(at, v, true),
    int16:  (v, view, at) => view.setInt16(at, v, true),
    uint32: (v, view, at) => view.setUint32(at, v, true),
    int32:  (v, view, at) => view.setInt32(at, v, true),
    uint64: (v, view, at) => view.setUint32(at, v, true),   /* as the page sends it */
  };

  const READ = {
    uint8:  (view, at) => view.getUint8(at),
    int8:   (view, at) => view.getInt8(at),
    uint16: (view, at) => view.getUint16(at, true),
    int16:  (view, at) => view.getInt16(at, true),
    uint32: (view, at) => view.getUint32(at, true),
    int32:  (view, at) => view.getInt32(at, true),
    uint64: (view, at) => view.getUint32(at, true),
  };

  /* The page is the authority on how a field is encoded - reading data-type back off
     the control is what keeps this in step with form.py. */
  function typeOf(key) {
    const input = document.querySelector(`[data-key="${key}"]`);

    return input && input.dataset.type in WRITE ? input.dataset.type : 'uint32';
  }

  function nameOf(key) {
    return FIELDS[key] ? FIELDS[key].name : 'unknown key';
  }

  function typeName(value) {
    for (const [name, number] of Object.entries(packetType))
      if (number === value)
        return name.replace('Msg', '');

    return `type ${value}`;
  }

  /* Both boards back to a known state. The page starts on the sample; a wipe is a wipe,
     so it drops to the bare defaults and the difference is visible on a Read. */
  function reset(sample) {
    for (const side of Object.keys(boards)) {
      boards[side].clear();

      for (const [key, field] of Object.entries(FIELDS))
        boards[side].set(Number(key), Number(field.default) || 0);

      if (sample)
        for (const [key, value] of Object.entries(SAMPLE))
          boards[side].set(Number(key), value);

      saved[side] = new Map(boards[side]);
    }
  }

  /* ------------------------------------------------------------------- the log */

  var logBody, logCount, packets = 0;

  function hex(bytes) {
    return [...bytes].map(b => b.toString(16).padStart(2, '0')).join(' ');
  }

  function log(direction, text, bytes, bad) {
    if (!logBody)
      return;

    const row = document.createElement('div');

    row.className = 'mock-row' + (bad ? ' mock-bad' : '');
    row.innerHTML = `<span class="mock-dir">${direction}</span><span class="mock-txt"></span>`;
    row.querySelector('.mock-txt').textContent = text;

    if (bytes) {
      const raw = document.createElement('div');

      raw.className = 'mock-raw';
      raw.textContent = hex(bytes);
      row.appendChild(raw);
    }

    logBody.appendChild(row);
    logBody.scrollTop = logBody.scrollHeight;

    while (logBody.children.length > 300)
      logBody.removeChild(logBody.firstChild);

    logCount.textContent = `${++packets} packets`;
  }

  /* ------------------------------------------------------------------ the device */

  const listeners = [];

  /* Reply the way handle_api_msgs() does: a GET_VAL packet carrying the key in data[0]
     and the value straight after it. */
  function reply(key) {
    const buffer = new ArrayBuffer(PACKET_LEN);
    const view = new DataView(buffer);
    const value = boards.A.has(key) ? boards.A.get(key) : 0;

    view.setUint8(0, 0xaa);
    view.setUint8(1, 0x55);
    view.setUint8(2, packetType.getValMsg);
    view.setUint8(KEY_OFFSET, key);
    WRITE[typeOf(key)](value, view, KEY_OFFSET + 1);

    log('&larr;', `GET_VAL  ${key} ${nameOf(key)} = ${value}`, new Uint8Array(buffer));
    listeners.forEach(fn => fn({ data: view }));
  }

  function handleReport(bytes) {
    const view = new DataView(bytes.buffer);
    const proxied = bytes[2] === packetType.proxyPacketMsg;
    const type = proxied ? bytes[3] : bytes[2];
    const at = proxied ? KEY_OFFSET + 1 : KEY_OFFSET;
    const board = proxied ? 'B' : 'A';
    const via = proxied ? ' (proxied to board B)' : '';

    /* The firmware checks this before it looks at anything else, so check it first here
       too - a bad one is a packet that would be dropped, not acted on. */
    if (calcChecksum(bytes) !== bytes[PACKET_LEN - 1]) {
      log('&rarr;', `${typeName(type)} REJECTED, checksum is ${bytes[PACKET_LEN - 1]}, ` +
                    `expected ${calcChecksum(bytes)}`, bytes, true);
      return;
    }

    switch (type) {
      case packetType.getValAllMsg:
        log('&rarr;', `GET_VAL_ALL, answering with ${Object.keys(FIELDS).length} fields`, bytes);
        Object.keys(FIELDS).forEach(key => reply(Number(key)));
        break;

      case packetType.getValMsg:
        log('&rarr;', `GET_VAL  ${bytes[at]} ${nameOf(bytes[at])}`, bytes);
        reply(bytes[at]);
        break;

      case packetType.setValMsg: {
        const key = bytes[at];
        const value = READ[typeOf(key)](view, at + 1);

        boards[board].set(key, value);
        log('&rarr;', `SET_VAL  ${key} ${nameOf(key)} = ${value}${via}`, bytes,
            !(key in FIELDS));
        break;
      }

      case packetType.saveConfigMsg: {
        /* Per board: a save arrives once for each, and each has its own unwritten set. */
        const changed = [...boards[board].keys()]
          .filter(key => boards[board].get(key) !== saved[board].get(key));

        saved[board] = new Map(boards[board]);
        log('&rarr;', `SAVE_CONFIG, ${changed.length} value(s) written to flash${via}`, bytes);
        break;
      }

      case packetType.wipeConfigMsg:
        reset(false);
        log('&rarr;', `WIPE_CONFIG, back to the defaults form.py declares - ` +
                      `press Read to pull them in${via}`, bytes);
        break;

      case packetType.flashLedMsg:
        log('&rarr;', `FLASH_LED, the LED would blink${via}`, bytes);
        break;

      case packetType.rebootMsg:
        log('&rarr;', 'REBOOT, the board would drop off the bus', bytes);
        break;

      default:
        log('&rarr;', `${typeName(type)}, accepted and ignored${via}`, bytes);
    }
  }

  const fakeDevice = {
    vendorId: 0x2e8a,
    productId: 0x107c,
    productName: 'DeskHop Extended (emulated)',
    opened: false,

    async open() {
      this.opened = true;
      log('&nbsp;', 'opened DeskHop Extended (emulated)');
    },

    async close() {
      this.opened = false;
      log('&nbsp;', 'closed');
    },

    addEventListener(type, fn) {
      if (type === 'inputreport')
        listeners.push(fn);
    },

    removeEventListener(type, fn) {
      const at = listeners.indexOf(fn);

      if (type === 'inputreport' && at !== -1)
        listeners.splice(at, 1);
    },

    async sendReport(reportId, data) {
      handleReport(new Uint8Array(data.buffer ? data.buffer : data));
    },
  };

  Object.defineProperty(navigator, 'hid', {
    configurable: true,
    value: {
      async requestDevice() { return [fakeDevice]; },
      async getDevices() { return [fakeDevice]; },
      addEventListener() { /* the page listens for unplug; nothing ever unplugs here */ },
      removeEventListener() {},
    },
  });

  /* ------------------------------------------------------------------- the panel */

  const PANEL_CSS = `
    .mock { position: fixed; right: 12px; bottom: 12px; z-index: 99; width: 470px;
      max-width: calc(100vw - 24px); background: #fff; border: 1px solid var(--bd);
      border-radius: 6px; box-shadow: 0 6px 24px rgba(22, 24, 28, .18); overflow: hidden;
      font-family: ui-monospace, Menlo, monospace; font-size: 11px; }
    .mock-head { display: flex; align-items: center; gap: 6px; padding: 7px 9px;
      white-space: nowrap; background: var(--warn-bg, #fff6e5);
      border-bottom: 1px solid var(--bd); }
    /* min-width lets the title give way rather than push the buttons off the edge. */
    .mock-t { flex: 1 1 auto; min-width: 0; overflow: hidden; text-overflow: ellipsis;
      font-weight: 700; letter-spacing: .04rem; color: var(--warn-fg, #8a5a00); }
    .mock-n { flex: 0 0 auto; color: var(--muted); }
    .mock button { font: inherit; white-space: nowrap; padding: 2px 7px;
      border: 1px solid var(--bd); border-radius: 3px; background: #fff; color: var(--fg);
      cursor: pointer; }
    .mock-body { max-height: 34vh; overflow: auto; padding: 4px 0; }
    .mock.shut .mock-body { display: none; }
    .mock-row { padding: 3px 9px; line-height: 1.45; }
    .mock-row:nth-child(even) { background: #f6f9fb; }
    .mock-dir { display: inline-block; width: 12px; color: var(--accent); }
    .mock-txt { color: var(--fg); }
    /* Under the decode rather than beside it, so a long field name cannot squeeze the
       bytes into a column two characters wide. */
    .mock-raw { padding-left: 12px; color: var(--muted); }
    .mock-bad { background: #fdecec; }
    .mock-bad .mock-dir, .mock-bad .mock-txt { color: #a4262c; }
    @media print { .mock { display: none; } }
  `;

  /* The other board's reported version, cycled so the mismatch and the never-reported
     dash are both reachable without a second board. */
  const OTHER = [
    [FW_VERSION, 'matching'],
    [FW_VERSION - 1, 'older, so the mismatch warning shows'],
    [0, 'silent, so it reads as a dash'],
  ];
  var otherAt = 0;

  function buildPanel() {
    const style = document.createElement('style');

    style.textContent = PANEL_CSS;
    document.head.appendChild(style);

    const panel = document.createElement('div');

    panel.className = 'mock';
    panel.innerHTML = `
      <div class="mock-head">
        <span class="mock-t">TEST PAGE &middot; emulated device</span>
        <span class="mock-n">0 packets</span>
        <button type="button" data-mock="other">Other board</button>
        <button type="button" data-mock="clear">Clear</button>
        <button type="button" data-mock="shut">&minus;</button>
      </div>
      <div class="mock-body"></div>`;
    document.body.appendChild(panel);

    logBody = panel.querySelector('.mock-body');
    logCount = panel.querySelector('.mock-n');

    panel.querySelector('[data-mock="clear"]').addEventListener('click', () => {
      logBody.textContent = '';
      packets = 0;
      logCount.textContent = '0 packets';
    });

    panel.querySelector('[data-mock="shut"]').addEventListener('click', event => {
      const shut = panel.classList.toggle('shut');

      event.target.innerHTML = shut ? '+' : '&minus;';
    });

    panel.querySelector('[data-mock="other"]').addEventListener('click', () => {
      otherAt = (otherAt + 1) % OTHER.length;

      const [value, what] = OTHER[otherAt];

      boards.A.set(86, value);
      log('&nbsp;', `other board now reports ${value} - ${what}`);
      reply(86);
    });
  }

  /* script.js sets the page up on window load and finishes there by calling
     setConnected(false). This listener is registered after that one and fires after it,
     so it connects on top of a settled page rather than being undone by it. */
  window.addEventListener('load', async () => {
    document.title += ' (test)';
    reset(true);
    buildPanel();
    log('&nbsp;', 'no hardware attached - every packet below is answered by this page');
    await connectHandler();
  });
})();
