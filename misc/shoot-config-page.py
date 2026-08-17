#!/usr/bin/env python3
"""Regenerate img/config-page-extended.png, the README screenshot of the config page.

Loads webconfig/config.htm - the shipped self-extracting artifact, not the unpacked
source - in headless Chromium and drives it into its connected state with a sample
configuration, since nothing can pair over WebHID from a build machine. The firmware
version comes from CMakeLists.txt and the checksum from build/deskhop.crc, so both are
the real values for this tree rather than invented ones. Run `cmake --build build`
first if you want the checksum to be current.

Setup, from misc/:

    python3 -m venv venv
    ./venv/bin/pip install -r requirements.txt
    ./venv/bin/playwright install chromium

On Debian/Ubuntu the browser also needs system libraries that pip does not carry:

    sudo apt install libnss3 libnspr4 libasound2t64

Usage: misc/shoot-config-page.py [output.png]
"""
import binascii
import re
import struct
import subprocess
import sys
from pathlib import Path

from PIL import Image
from playwright.sync_api import sync_playwright

REPO = Path(subprocess.run(["git", "rev-parse", "--show-toplevel"], cwd=Path(__file__).parent,
                           capture_output=True, text=True, check=True).stdout.strip())
OUT = Path(sys.argv[1]) if len(sys.argv) > 1 else REPO / "img/config-page-extended.png"

WIDTH = 1140          # a little over the 1020px .page, leaving the card some margin
FINAL_WIDTH = 1520    # matches img/config-page-big.png; rendering at 2x then downscaling

# A plausible desk, not a capture of any particular device. Output A base 10,
# output B base 40 - see webconfig/form.py for what each offset means.
VALUES = {
    # Output A - Linux, single screen on the left.
    11: 1, 12: 16, 13: 16, 14: 0, 15: 0, 16: 1, 17: 1, 18: 0, 19: 0, 20: 0, 21: 0, 22: 0,
    # Output B - Windows, two screens on the right, Pong screensaver once idle.
    41: 2, 42: 20, 43: 20, 44: 0, 45: 0, 46: 3, 47: 2, 48: 3, 49: 1, 50: 1,
    51: 300_000_000, 52: 600_000_000,
    # Shared. 72 (boot-protocol keyboard) and 83 (edge double-tap) are this fork's additions.
    71: 0, 72: 1, 73: 1, 75: 0, 76: 0, 77: 0, 83: 1, 84: 300, 85: 1000,
}

DRIVE = """
([values, fw, sum]) => {
  setConnected(true);
  for (const [key, v] of Object.entries(values)) {
    const input = document.querySelector(`[data-key="${key}"]`);
    if (input) setValue(input, v);
  }
  document.querySelector('[data-fw-ver]').value = fw;
  document.querySelector('[data-hex]').value = sum;
  refreshOutput('A'); refreshOutput('B'); refreshSwitching(); markClean();
  /* Settings this script left at their default, so drift in form.py gets noticed.
     The firmware version and checksum are set above, not from values. */
  return [...document.querySelectorAll('[data-key]')]
    .filter(e => !e.hasAttribute('data-fw-ver') && !e.hasAttribute('data-hex'))
    .map(e => e.dataset.key)
    .filter(k => !(k in values));
}
"""


def firmware_version() -> str:
    """v<major>.<minor>, the way the page formats what the device reports."""
    cmake = (REPO / "CMakeLists.txt").read_text()
    major = re.search(r"set\(VERSION_MAJOR (\d+)\)", cmake).group(1)
    minor = re.search(r"set\(VERSION_MINOR (\d+)\)", cmake).group(1)
    return f"v{major}.{minor}"


def firmware_checksum() -> str:
    """CRC32 from the last build, formatted as the page's data-hex field renders it."""
    crc = REPO / "build/deskhop.crc"
    if not crc.exists():
        return "—" * 8
    _magic, _version, value = struct.unpack("<IHI", crc.read_bytes())
    return f"{value:x}"


def main() -> None:
    page_url = (REPO / "webconfig/config.htm").as_uri()
    with sync_playwright() as p:
        browser = p.chromium.launch()
        page = browser.new_page(viewport={"width": WIDTH, "height": 1200}, device_scale_factor=2)
        errors: list[str] = []
        page.on("pageerror", lambda e: errors.append(str(e)))
        page.goto(page_url)
        page.wait_for_selector("#panel", timeout=15000)
        undriven = page.evaluate(DRIVE, [{str(k): v for k, v in VALUES.items()},
                                         firmware_version(), firmware_checksum()])
        page.wait_for_timeout(400)

        # The Save bar is position:sticky. A full_page capture paints it at the initial
        # viewport's bottom edge, over the Keep awake section. Growing the viewport to the
        # whole document puts it back where it sits naturally, at the foot of the card.
        page.set_viewport_size({"width": WIDTH,
                                "height": page.evaluate("document.documentElement.scrollHeight")})
        page.wait_for_timeout(200)
        shot = page.screenshot()
        browser.close()

    if errors:
        sys.exit(f"page raised errors, refusing to write a broken screenshot: {errors}")
    if undriven:
        print(f"warning: settings {', '.join(undriven)} are on the page but not in VALUES, so "
              "they render at their default - add them from webconfig/form.py", file=sys.stderr)

    tmp = OUT.with_suffix(".raw.png")
    tmp.write_bytes(shot)
    image = Image.open(tmp)
    image = image.resize((FINAL_WIDTH, round(image.height * FINAL_WIDTH / image.width)),
                         Image.LANCZOS).convert("P", palette=Image.ADAPTIVE, colors=256)
    image.save(OUT, optimize=True)
    tmp.unlink()
    print(f"wrote {OUT.relative_to(REPO)}: {image.width}x{image.height}, "
          f"{OUT.stat().st_size // 1024} KB")


if __name__ == "__main__":
    main()
