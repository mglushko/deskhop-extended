"""Exercise the shipped config.htm in headless Chromium.

Covers settings export/import (including the file paths) and the device buttons that
send raw reports, which are otherwise only testable with hardware attached."""
import json, os, sys, tempfile
from playwright.sync_api import sync_playwright

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
fails = []
def check(name, cond, detail=""):
    print(f"  {'PASS' if cond else 'FAIL'}  {name}{'' if cond else '  <- ' + str(detail)}")
    if not cond: fails.append(name)


def check_packet_allowlist():
    """Every packet type the page sends has to be one validate_packet() accepts.

    The Bootloader button shipped for years in upstream sending FIRMWARE_UPGRADE_MSG,
    which validate_packet drops - the page offered an action the firmware is designed to
    refuse, and the only symptom was a button that did nothing. Compared by number, not
    by name, since the two sides spell several of these differently."""
    import re
    js = open(f"{REPO}/webconfig/templates/script.js", encoding="utf-8").read()
    proto = open(f"{REPO}/src/include/protocol.h", encoding="utf-8").read()
    utils = open(f"{REPO}/src/utils.c", encoding="utf-8").read()

    js_types = dict(re.findall(r"(\w+):\s*(\d+)", re.search(r"packetType = \{(.*?)\};", js, re.S).group(1)))
    c_types = dict((n, int(v)) for n, v in re.findall(r"(\w+_MSG)\s*=\s*(\d+)", proto))
    allowed_names = re.findall(r"(\w+_MSG)",
                               re.search(r"ALLOWED_PACKETS\[\] = \{(.*?)\};", utils, re.S).group(1))
    allowed = {c_types[n] for n in allowed_names if n in c_types}

    used = set(re.findall(r"sendReport\(packetType\.(\w+)", js))
    bad = sorted(u for u in used if int(js_types.get(u, -1)) not in allowed)

    check("every packet the page sends is one the firmware accepts", not bad,
          f"rejected by validate_packet: {bad}")


check_packet_allowlist()

with sync_playwright() as p:
    b = p.chromium.launch()
    ctx = b.new_context(accept_downloads=True)
    page = ctx.new_page()
    errs = []; page.on("pageerror", lambda e: errs.append(str(e)))
    page.goto(f"file://{REPO}/webconfig/config.htm")
    page.wait_for_selector("#panel", timeout=15000)

    page.evaluate("() => { setConnected(true); "
                  "document.querySelectorAll('.api:not([readonly])').forEach((e,i) => "
                  "  setValue(e, e.type === 'checkbox' ? (i % 2) : (i + 3))); markClean(); }")

    writable = page.evaluate("() => document.querySelectorAll('.api:not([readonly])').length")
    # 34 of the firmware's 38 writable fields; the page deliberately does not expose
    # output[].number (x2), config.version or hotkey_toggle.
    check("page exposes 34 writable fields", writable == 34, writable)

    page.evaluate("() => exportHandler()")
    exported = json.loads(page.eval_on_selector("#backup-text", "e => e.value"))
    check("export carries every writable field",
          len(exported["settings"]) == writable, len(exported["settings"]))
    check("export is tagged", exported.get("deskhop-extended-settings") == 1)

    before = page.evaluate("() => { const o = {}; document.querySelectorAll('.api:not([readonly])')"
                           ".forEach(e => o[e.dataset.key] = getValue(e)); return o; }")

    # Scramble every control away from the exported state, then import it back.
    # Scramble to a value guaranteed to differ from what was exported, so every
    # control genuinely has to be restored.
    page.evaluate("() => document.querySelectorAll('.api:not([readonly])')"
                  ".forEach(e => setValue(e, e.type === 'checkbox' ? (getValue(e) ? 0 : 1)"
                  "                                               : Number(getValue(e)) + 1000))")
    page.evaluate("() => { markClean(); importHandler(); }")
    page.eval_on_selector("#backup-text", "(e, v) => e.value = v", json.dumps(exported))
    page.evaluate("() => applyImportHandler()")

    after = page.evaluate("() => { const o = {}; document.querySelectorAll('.api:not([readonly])')"
                          ".forEach(e => o[e.dataset.key] = getValue(e)); return o; }")
    check("import restores every value", after == before,
          {k: (before[k], after[k]) for k in before if before[k] != after[k]})

    check("page is left dirty so Save is enabled", page.evaluate("() => dirty") is True)
    check("Save button enabled", page.eval_on_selector("#btn-save", "e => !e.disabled"))

    # saveHandler writes only where the value differs from fetched-value.
    would_write = page.evaluate(
        "() => [...document.querySelectorAll('.api:not([readonly])')]"
        ".filter(e => e.getAttribute('fetched-value') != getValue(e)).length")
    check("saveHandler would push all imported values", would_write == writable, would_write)

    # Unknown keys are reported, not fatal.
    page.evaluate("() => importHandler()")
    page.eval_on_selector("#backup-text", "(e, v) => e.value = v",
                          json.dumps({"settings": {"11": 2, "999": 7}}))
    page.evaluate("() => applyImportHandler()")
    msg = page.eval_on_selector("#backup-m", "e => e.textContent")
    check("unknown key reported and skipped", "999" in msg and "unknown" in msg, msg)

    page.evaluate("() => importHandler()")
    page.eval_on_selector("#backup-text", "(e, v) => e.value = v", "not json {")
    page.evaluate("() => applyImportHandler()")
    check("malformed input rejected cleanly",
          "not valid JSON" in page.eval_on_selector("#backup-m", "e => e.textContent"))

    # ---- export hands over a file -------------------------------------------
    page.evaluate("() => closeBackupHandler()")
    try:
        with page.expect_download(timeout=8000) as info:
            page.evaluate("() => exportHandler()")
        download = info.value
        saved = open(download.path(), encoding="utf-8").read()
        check("export downloads a file",
              download.suggested_filename.startswith("deskhop-extended-settings-")
              and download.suggested_filename.endswith(".txt"),
              download.suggested_filename)
        check("downloaded file holds the same settings as the panel",
              json.loads(saved) == json.loads(page.eval_on_selector("#backup-text", "e => e.value")))
    except Exception as e:
        check("export downloads a file", False, f"{type(e).__name__}: {str(e)[:90]}")
        check("fallback: panel still shows the text to copy",
              "deskhop-extended-settings" in page.eval_on_selector("#backup-text", "e => e.value"))

    # ---- import reads a chosen file ------------------------------------------
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False, encoding="utf-8") as fh:
        json.dump(exported, fh)
        chosen = fh.name

    page.evaluate("() => document.querySelectorAll('.api:not([readonly])')"
                  ".forEach(e => setValue(e, e.type === 'checkbox' ? 0 : 1))")
    page.evaluate("() => { markClean(); importHandler(); }")
    page.set_input_files("#backup-input", chosen)
    page.wait_for_function("() => el('backup-text').value.length > 0", timeout=5000)
    page.evaluate("() => applyImportHandler()")
    from_file = page.evaluate("() => { const o = {}; document.querySelectorAll('.api:not([readonly])')"
                              ".forEach(e => o[e.dataset.key] = getValue(e)); return o; }")
    check("import from a chosen file restores every value", from_file == before,
          {k: (before[k], from_file[k]) for k in before if before[k] != from_file[k]})
    os.unlink(chosen)

    # ---- both board versions -------------------------------------------------
    def show_versions(local, other):
        # Through the page's own formatter, the one handleInputReport uses on the
        # values the device reports.
        page.evaluate("""([a, b]) => {
            setValue(document.querySelector('[data-fw-self]'), formatFwVersion(a));
            setValue(document.querySelector('[data-fw-ver]:not([data-fw-self])'), formatFwVersion(b));
        }""", [local, other])

    show_versions(1100, 1100)
    shown = page.evaluate("() => [...document.querySelectorAll('[data-fw-ver]')].map(e => e.value)")
    check("both version fields format from the raw uint16", shown == ["v1.00", "v1.00"], shown)
    check("matching versions raise no warning",
          page.eval_on_selector("#ver-differ", "e => e.hidden"))

    show_versions(1100, 1101)
    check("differing versions are flagged",
          page.eval_on_selector("#ver-differ", "e => !e.hidden"))

    show_versions(1100, 0)
    other_val = page.eval_on_selector("[data-fw-ver]:not([data-fw-self])", "e => e.value")
    check("a board that never reported shows a dash", other_val == "—", other_val)
    check("a board that never reported is not a mismatch",
          page.eval_on_selector("#ver-differ", "e => e.hidden"))

    # ---- column order ---------------------------------------------------------
    def set_swap(on):
        page.evaluate("v => setValue(document.querySelector('[data-key=\"87\"]'), v)", 1 if on else 0)

    set_swap(True)
    check("swap on puts output B in the left-hand column",
          page.evaluate("() => { const d = document.querySelector('.obar .duo');"
                        " return el('panel').classList.contains('swapped')"
                        " && getComputedStyle(d.firstElementChild).order === '2'; }"))
    set_swap(False)
    check("swap off restores output A to the left",
          page.evaluate("() => !el('panel').classList.contains('swapped')"))

    # Screen side is correct as shipped and has been mistaken for a bug once; pin it so a
    # later change cannot quietly invert it.
    page.evaluate("() => { setValue(document.querySelector('[data-key=\"17\"]'), 1);"
                  " refreshOutput('A'); }")
    cap = page.eval_on_selector(".cap[data-o='A']", "e => e.textContent")
    check("Left pairs with crossing off the right edge",
          "on the left" in cap and "right edge" in cap, cap)

    # ---- a throwing handler must not fail silently ---------------------------
    # A handler that throws must surface rather than leaving the button looking inert.
    page.evaluate("""() => {
        window.__throwingHandler = async () => { throw new Error('boom'); };
        const b = document.createElement('button');
        b.dataset.handler = '__throwingHandler';
        b.textContent = 'Explode';
        document.getElementById('menu-buttons').appendChild(b);
        b.click();
    }""")
    page.wait_for_function("() => !el('backup').hidden && /Explode/.test(el('backup-t').textContent)",
                           timeout=5000)
    check("a throwing handler is reported instead of failing silently",
          "boom" in page.eval_on_selector("#backup-text", "e => e.value"))

    b.close()

check("no page errors", not errs, errs)
print("\n" + ("ALL PASS" if not fails else f"{len(fails)} FAILED: {fails}"))
sys.exit(1 if fails else 0)
