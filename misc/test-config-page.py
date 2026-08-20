#!/usr/bin/env python3
"""Drive webconfig/config.htm in a headless browser and check the controls that carry
their own logic - the ones a rendered page cannot be eyeballed for.

Runs against the shipped self-extracting artifact, not the unpacked source, so it also
proves the page survives being packed. Nothing pairs over WebHID from a build machine,
so the page is put into its connected state by hand and values are pushed in the way an
incoming report would.

Setup, from misc/:

    python3 -m venv venv
    ./venv/bin/pip install -r requirements.txt
    ./venv/bin/playwright install chromium

Usage: misc/venv/bin/python misc/test-config-page.py  (after `make` in webconfig/)
"""
import subprocess
import sys
from pathlib import Path

from playwright.sync_api import sync_playwright

REPO = Path(subprocess.run(["git", "rev-parse", "--show-toplevel"], cwd=Path(__file__).parent,
                           capture_output=True, text=True, check=True).stdout.strip())

fails = []


def check(name, ok, detail=""):
    print(f"  {'PASS' if ok else 'FAIL'}  {name}{'' if ok else '  <- ' + str(detail)}")
    if not ok:
        fails.append(name)

with sync_playwright() as p:
    browser = p.chromium.launch()
    page = browser.new_context(viewport={"width": 1140, "height": 1200}).new_page()
    errors = []
    page.on("pageerror", lambda e: errors.append(str(e)))
    page.goto((REPO / "webconfig/config.htm").as_uri())
    page.wait_for_selector("#panel", timeout=15000)
    page.evaluate("() => setConnected(true)")

    check("page loads without errors", not errors, errors)

    # ---- status LED --------------------------------------------------------
    check("LED group is present", page.locator("#led-note").count() == 1)
    page.evaluate("() => setValue(document.querySelector('[data-key=\"88\"]'), 0)")
    check("seconds field is disabled while the LED is always on",
          page.eval_on_selector('.led-part input.sec', "e => e.disabled"))

    def timers_on():
        return page.evaluate('''() => [...document.querySelectorAll('.led-part')]
            .filter(p => !p.querySelector('input').disabled).map(p => p.dataset.led)''')

    page.evaluate("() => setValue(document.querySelector('[data-key=\"88\"]'), 1)")
    check("when idle brings up the idle timer alone", timers_on() == ["idle"], timers_on())

    page.evaluate("() => setValue(document.querySelector('[data-key=\"88\"]'), 2)")
    check("after switching brings up the switch timer alone", timers_on() == ["switch"], timers_on())

    page.evaluate("() => setValue(document.querySelector('[data-key=\"88\"]'), 3)")
    check("idle + switch brings up both", timers_on() == ["idle", "switch"], timers_on())

    idle, switch = '[data-led="idle"] input.sec', '[data-led="switch"] input.sec'

    page.evaluate("() => setValue(document.querySelector('[data-key=\"89\"]'), 45)")
    page.evaluate("() => setValue(document.querySelector('[data-key=\"103\"]'), 8)")
    check("both timers render 1:1 as seconds",
          [page.eval_on_selector(idle, "e => e.value"), page.eval_on_selector(switch, "e => e.value")]
          == ["45", "8"],
          [page.eval_on_selector(idle, "e => e.value"), page.eval_on_selector(switch, "e => e.value")])

    page.fill(idle, '90')
    page.dispatch_event(idle, 'change')
    page.fill(switch, '5')
    page.dispatch_event(switch, 'change')
    check("typing seconds writes seconds to the field it belongs to",
          [page.eval_on_selector('[data-key="89"]', "e => e.value"),
           page.eval_on_selector('[data-key="103"]', "e => e.value")] == ["90", "5"],
          [page.eval_on_selector('[data-key="89"]', "e => e.value"),
           page.eval_on_selector('[data-key="103"]', "e => e.value")])

    # ---- screensaver seconds still scale by a million ----------------------
    # The timers follow the keep-awake mode, so give output A one first.
    page.evaluate("() => setValue(document.querySelector('[data-key=\"19\"]'), 1)")
    page.evaluate("() => setValue(document.querySelector('[data-key=\"21\"]'), 300000000)")
    check("screensaver idle still renders as seconds",
          page.eval_on_selector('[data-for="k21"]', "e => e.value") == "300",
          page.eval_on_selector('[data-for="k21"]', "e => e.value"))

    page.fill('[data-for="k21"]', '120')
    page.dispatch_event('[data-for="k21"]', 'change')
    check("screensaver idle still writes microseconds",
          page.eval_on_selector('[data-key="21"]', "e => e.value") == "120000000",
          page.eval_on_selector('[data-key="21"]', "e => e.value"))

    # ---- shortcuts ---------------------------------------------------------
    rows = page.locator(".hk-row")
    check("one row per hotkey", rows.count() == 13, rows.count())

    first = page.locator('.hk[data-for="k90"]')
    check("an unset shortcut shows the compiled-in default",
          first.text_content() == "Left Ctrl + Caps Lock", first.text_content())
    check("its Default button is disabled while nothing is stored",
          page.eval_on_selector('.hk-x[data-for="k90"]', "e => e.disabled"))

    wipe = page.locator('.hk[data-for="k98"]')
    check("a two-key default reads back whole",
          wipe.text_content() == "Right Shift + F12 + D", wipe.text_content())

    slow = page.locator('.hk[data-for="k91"]')
    # Listed in bit order, which is not the order src/keyboard.c happens to write them.
    check("a modifier-only default reads back whole",
          slow.text_content() == "Right Ctrl + Right Alt", slow.text_content())

    # Record Left Ctrl + F5 into the switch shortcut.
    first.click()
    check("clicking a shortcut starts recording", "hk-cap" in (first.get_attribute("class") or ""))
    page.keyboard.down("Control")
    page.keyboard.down("F5")
    page.keyboard.up("F5")
    page.keyboard.up("Control")

    packed = page.eval_on_selector('[data-key="90"]', "e => e.value")
    check("the combination is stored packed", packed == str(0x01 | (0x3e << 8)), packed)
    check("and shown back", first.text_content() == "Left Ctrl + F5", first.text_content())
    check("recording stops", "hk-cap" not in (first.get_attribute("class") or ""))
    check("the row now reads as set", "hk-set" in (first.get_attribute("class") or ""))
    check("Default is offered once something is stored",
          not page.eval_on_selector('.hk-x[data-for="k90"]', "e => e.disabled"))

    # Two keys, and Escape backing out.
    cfg = page.locator('.hk[data-for="k100"]')
    cfg.click()
    page.keyboard.down("Shift")
    page.keyboard.down("KeyQ")
    page.keyboard.down("KeyW")
    page.keyboard.up("KeyW")
    page.keyboard.up("KeyQ")
    page.keyboard.up("Shift")
    check("two keys and a modifier are recorded",
          page.eval_on_selector('[data-key="100"]', "e => e.value")
          == str(0x02 | (0x14 << 8) | (0x1a << 16)),
          page.eval_on_selector('[data-key="100"]', "e => e.value"))

    lock = page.locator('.hk[data-for="k92"]')
    before = page.eval_on_selector('[data-key="92"]', "e => e.value")
    lock.click()
    page.keyboard.press("Escape")
    check("Escape leaves the shortcut alone",
          page.eval_on_selector('[data-key="92"]', "e => e.value") == before)
    check("and puts the label back", lock.text_content() == "Right Ctrl + K", lock.text_content())

    # Pick: the fallback for combinations the browser keeps to itself, which the recorder
    # can never hear. Ctrl+Shift+Tab is one of them.
    page.click('.hk-p[data-hk="k92"]')
    check("Pick opens the editor on that shortcut",
          page.eval_on_selector('#hk-ed-t', "e => e.textContent") == "Lock switching",
          page.eval_on_selector('#hk-ed-t', "e => e.textContent"))

    prefill = '''() => [[...document.querySelectorAll('#hk-mods button')]
        .filter(b => b.getAttribute('aria-pressed') === 'true').map(b => b.dataset.m),
        el('hk-k1').value, el('hk-k2').value]'''
    check("prefilled from the combination the row is showing",
          page.evaluate(prefill) == [["16"], "14", "0"], page.evaluate(prefill))

    page.click('#hk-mods button[data-m="16"]')   # drop Right Ctrl
    page.click('#hk-mods button[data-m="1"]')    # Left Ctrl
    page.click('#hk-mods button[data-m="2"]')    # Left Shift
    page.select_option('#hk-k1', '43')           # Tab
    page.select_option('#hk-k2', '0')
    page.click('#hk-ed-set')
    check("a combination the browser eats can still be set by hand",
          page.eval_on_selector('[data-key="92"]', "e => e.value") == str(0x01 | 0x02 | (0x2b << 8)),
          page.eval_on_selector('[data-key="92"]', "e => e.value"))
    check("and reads back as itself",
          page.locator('.hk[data-for="k92"]').text_content() == "Left Ctrl + Left Shift + Tab",
          page.locator('.hk[data-for="k92"]').text_content())
    check("the editor closes once it is set", page.eval_on_selector('#hk-ed', "e => e.hidden"))

    page.click('.hk-p[data-hk="k92"]')
    page.click('#hk-mods button[data-m="128"]')
    page.click('#hk-ed-cancel')
    check("Cancel leaves the shortcut as it was",
          page.eval_on_selector('[data-key="92"]', "e => e.value") == str(0x01 | 0x02 | (0x2b << 8)))

    page.click('.hk-x[data-for="k90"]')
    check("Default clears what was stored",
          page.eval_on_selector('[data-key="90"]', "e => e.value") == "0")
    check("and the label goes back to the compiled-in one",
          first.text_content() == "Left Ctrl + Caps Lock", first.text_content())

    # ---- export carries the new fields -------------------------------------
    keys = page.evaluate("() => [...backupFields()].map(e => e.dataset.key)")
    missing = [k for k in ["88", "89", "90", "96", "102", "103"] if k not in keys]
    check("export covers the new settings", not missing, missing)

    check("no errors raised while driving it", not errors, errors)
    browser.close()

print("\n" + ("FAILURES: " + ", ".join(fails) if fails else "ALL PASS"))
sys.exit(1 if fails else 0)
