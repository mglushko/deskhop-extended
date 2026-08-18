"""Exercise export/import against the real shipped config.htm in headless Chromium."""
import json, sys
from playwright.sync_api import sync_playwright

REPO = "/home/dgurion/deskhop-extended"
fails = []
def check(name, cond, detail=""):
    print(f"  {'PASS' if cond else 'FAIL'}  {name}{'' if cond else '  <- ' + str(detail)}")
    if not cond: fails.append(name)

with sync_playwright() as p:
    b = p.chromium.launch(); page = b.new_page()
    errs = []; page.on("pageerror", lambda e: errs.append(str(e)))
    page.goto(f"file://{REPO}/webconfig/config.htm")
    page.wait_for_selector("#panel", timeout=15000)

    page.evaluate("() => { setConnected(true); "
                  "document.querySelectorAll('.api:not([readonly])').forEach((e,i) => "
                  "  setValue(e, e.type === 'checkbox' ? (i % 2) : (i + 3))); markClean(); }")

    writable = page.evaluate("() => document.querySelectorAll('.api:not([readonly])').length")
    # 33 of the firmware's 37 writable fields; the page deliberately does not expose
    # output[].number (x2), config.version or hotkey_toggle.
    check("page exposes 33 writable fields", writable == 33, writable)

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

    # ---- device buttons that send raw reports --------------------------------
    page.evaluate("""() => {
        window.__sent = [];
        window.device = { opened: true,
            sendReport: async (id, data) => { window.__sent.push([...data]); } };
    }""")
    boot = page.evaluate("""async () => {
        window.__sent = [];
        try { await enterBootloaderHandler(); return {ok: true, sent: window.__sent}; }
        catch (e) { return {ok: false, err: e.constructor.name + ': ' + e.message}; }
    }""")
    check("Bootloader sends without throwing", boot["ok"], boot.get("err"))
    if boot["ok"]:
        sent = boot["sent"]
        check("Bootloader reaches both boards", len(sent) == 2, len(sent))
        # proxied first, then local; byte 2 is the message type
        check("proxied packet wraps the firmware-upgrade message",
              len(sent) == 2 and sent[0][2] == 23 and sent[0][3] == 4, sent[0][:5] if sent else None)
        check("local packet is the firmware-upgrade message",
              len(sent) == 2 and sent[1][2] == 4, sent[1][:5] if len(sent) > 1 else None)

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
