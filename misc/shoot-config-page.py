#!/usr/bin/env python3
"""Regenerate the README screenshots of the config page.

Writes the page shot itself plus one close-up per setting that the README describes in
prose and is easier shown than told:

    img/config-page-extended.png   the card down to the end of Arrangement
    img/config-swap.png            the output bar and its Swap control
    img/config-dtap.png            the edge double-tap group
    img/config-led.png             the status LED mode and its two timers
    img/config-hotkeys.png         the shortcut list
    img/config-backup.png          the export panel

All six come out of one load of webconfig/config.htm - the shipped self-extracting artifact, not the unpacked
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

Usage: misc/shoot-config-page.py [output-dir]
"""
import io
import re
import struct
import subprocess
import sys
from pathlib import Path

from PIL import Image
from playwright.sync_api import sync_playwright

REPO = Path(subprocess.run(["git", "rev-parse", "--show-toplevel"], cwd=Path(__file__).parent,
                           capture_output=True, text=True, check=True).stdout.strip())
OUT_DIR = Path(sys.argv[1]) if len(sys.argv) > 1 else REPO / "img"

WIDTH = 1140          # a little over the 1020px .page, leaving the card some margin
FINAL_WIDTH = 1520    # matches img/config-page-big.png; rendering at 2x then downscaling

# Export stamps the file name and the JSON with the time of day, which would churn the
# committed PNG on every run. The clock is pinned so the panel poses with a fixed date.
FIXED_TIME = "2026-08-18T12:00:00Z"

# Each close-up is clipped to a box the page measures for itself, so a section moving or
# growing cannot leave a shot framing the wrong thing. Boxes are in CSS pixels; PAD is
# added on every side, or overridden per shot with "pad". "setup" runs first where a shot
# needs the page put into a state - the panel it frames may not exist until then. The
# close-ups come out at their natural width, captured at 2x and halved, which keeps them
# legible without dwarfing the prose they sit in.
PAD = 14

# Run between shots so each one is framed on a page in its resting state, whatever the
# one before it did to get its picture taken. Keeps BOXES order-independent.
RESET = """() => {
    document.querySelectorAll('[data-shot]').forEach(e => e.remove());

    if (!el('backup').hidden)
      closeBackupHandler();
}"""

BOXES = {
    # The card from the top down to the end of Arrangement. That boundary is the closest
    # section break to a third of the page, and it is the part worth showing: the header,
    # both output columns, the monitor diagrams and screen alignment. No padding, so the
    # cut lands in the gap after the band rather than clipping the next section's heading.
    "config-page-extended": {
        "pad": 0,
        "box": """() => {
            const title = [...document.querySelectorAll('.sect-t')]
                .find(e => e.textContent === 'Arrangement');
            const band = title.closest('.sect').nextElementSibling;
            return {x: 0, y: 0, width: window.innerWidth,
                    height: band.getBoundingClientRect().bottom};
        }""",
    },
    # A ring drawn around the Swap button for the shot only. Without it the eye has to
    # hunt a small control at the far end of a wide, otherwise empty bar.
    "config-swap": {
        "setup": """() => {
            const style = document.createElement('style');

            style.dataset.shot = '';
            style.textContent = `.swap {
                outline: 2px solid #d1495b;
                outline-offset: 4px;
                border-radius: 5px;
                box-shadow: 0 0 0 7px rgba(209, 73, 91, .16);
            }`;
            document.head.appendChild(style);
        }""",
        "box": """() => document.querySelector('.obar').getBoundingClientRect()""",
    },
    "config-dtap": """() => {
        const label = [...document.querySelectorAll('.shared-in .lbl')]
            .find(e => e.textContent === 'Edge double-tap');
        const top = label.getBoundingClientRect();
        const bottom = label.nextElementSibling.getBoundingClientRect();
        return {x: top.x, y: top.y, width: bottom.width, height: bottom.bottom - top.y};
    }""",
    # Label through group, the same framing as the double-tap shot above. Mode "Idle +
    # switch" is set in VALUES so both timers are live in the frame; the other three modes
    # grey one or both out, which reads as a broken control in a still.
    "config-led": """() => {
        const label = [...document.querySelectorAll('.shared-in .lbl')]
            .find(e => e.textContent === 'Status LED');
        const top = label.getBoundingClientRect();
        const bottom = label.nextElementSibling.getBoundingClientRect();
        return {x: top.x, y: top.y, width: bottom.width, height: bottom.bottom - top.y};
    }""",
    # The rows on their own, not the whole group: the two paragraphs above them explaining
    # Pick are prose the README already carries, and they run the shot to twice the height.
    # All twelve rows are in the frame, since that is the part worth seeing.
    "config-hotkeys": """() => document.querySelector('.hk-list').getBoundingClientRect()""",
    # The whole header down through the panel, so Export and Import are in the frame next
    # to what pressing Export gets you. From the top of the header rather than the button
    # row, which would saw the wordmark in half. One ring around the pair, drawn as an
    # overlay rather than as an outline on each, which would be two rings.
    "config-backup": {
        "setup": """() => {
            exportHandler();

            const box = ['exportHandler', 'importHandler']
                .map(h => document.querySelector(`[data-handler="${h}"]`).getBoundingClientRect())
                .reduce((a, b) => ({
                    x: Math.min(a.x, b.x), y: Math.min(a.y, b.y),
                    right: Math.max(a.right, b.right), bottom: Math.max(a.bottom, b.bottom),
                }));
            const ring = document.createElement('div');
            const pad = 4;

            ring.dataset.shot = '';
            Object.assign(ring.style, {
                position: 'absolute',
                left: `${window.scrollX + box.x - pad}px`,
                top: `${window.scrollY + box.y - pad}px`,
                width: `${box.right - box.x + 2 * pad}px`,
                height: `${box.bottom - box.y + 2 * pad}px`,
                border: '2px solid #d1495b',
                borderRadius: '7px',
                boxShadow: '0 0 0 5px rgba(209, 73, 91, .16)',
                pointerEvents: 'none',
                zIndex: '50',
            });
            document.body.appendChild(ring);
        }""",
        "box": """() => {
            const header = document.querySelector('.hdr').getBoundingClientRect();
            const panel = el('backup').getBoundingClientRect();
            const left = Math.min(header.x, panel.x);

            return {x: left, y: header.y,
                    width: Math.max(header.right, panel.right) - left,
                    height: panel.bottom - header.y};
        }""",
    },
}

# A plausible desk, not a capture of any particular device. Output A base 10,
# output B base 40 - see webconfig/form.py for what each offset means.
VALUES = {
    # Output A - Linux, single screen on the left. Border top/bottom (14, 15) span the
    # whole edge, what the page's "Same height" link writes, so the diagram draws the
    # accent line down the full crossing edge instead of a stub at the top.
    11: 1, 12: 16, 13: 16, 14: 0, 15: 32767, 16: 1, 17: 1, 18: 0, 19: 0, 20: 0, 21: 0, 22: 0,
    # Output B - Windows, two screens on the right, Pong screensaver once idle.
    41: 2, 42: 20, 43: 20, 44: 0, 45: 32767, 46: 3, 47: 2, 48: 3, 49: 1, 50: 1,
    51: 300_000_000, 52: 600_000_000,
    # Shared. 72 (boot-protocol keyboard) and 83 (edge double-tap) are this fork's additions;
    # 87 is which output the page draws on the left.
    71: 0, 72: 1, 73: 1, 75: 0, 76: 0, 77: 0, 83: 1, 84: 300, 85: 1000, 87: 0,
    # Status LED: both timers, so the LED stays lit while that computer is being used and
    # for a moment after a switch. Mode 3 is also the one that poses the section best -
    # it is the only one with neither timer greyed out.
    88: 3, 89: 60, 103: 10,
    # Shortcuts, one per entry in hotkeys[]; 98 is retired, since wiping the config has no
    # shortcut. Zero is what a device that has never had one changed holds, and the page
    # draws the compiled-in combo for it - which is what the shots should show.
    # 98 was retired with the wipe-config shortcut; 100 is config mode, which is listed
    # on the page but fixed, so it has no field of its own.
    **{key: 0 for key in range(90, 103) if key not in (98, 100)},
}

DRIVE = """
([values, fw, sum]) => {
  setConnected(true);
  for (const [key, v] of Object.entries(values)) {
    const input = document.querySelector(`[data-key="${key}"]`);
    if (input) setValue(input, v);
  }
  document.querySelector('[data-fw-self]').value = fw;
  document.querySelector('[data-fw-ver]:not([data-fw-self])').value = fw;
  document.querySelector('[data-hex]').value = sum;
  refreshVersions();
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
    return f"v{major}.{int(minor):02d}"


def firmware_checksum() -> str:
    """CRC32 from the last build, formatted as the page's data-hex field renders it."""
    crc = REPO / "build/deskhop.crc"
    if not crc.exists():
        return "—" * 8
    _magic, _version, value = struct.unpack("<IHI", crc.read_bytes())
    return f"{value:x}"


def write(name: str, shot: bytes, final_width: int) -> None:
    """Downscale from the 2x capture and save, palettised, the way the tree stores these."""
    out = OUT_DIR / f"{name}.png"
    tmp = out.with_suffix(".raw.png")

    tmp.write_bytes(shot)
    image = Image.open(tmp)
    image = image.resize((final_width, round(image.height * final_width / image.width)),
                         Image.LANCZOS).convert("P", palette=Image.ADAPTIVE, colors=256)
    image.save(out, optimize=True)
    tmp.unlink()
    shown = out.relative_to(REPO) if out.is_relative_to(REPO) else out
    print(f"wrote {shown}: {image.width}x{image.height}, "
          f"{out.stat().st_size // 1024} KB")


def main() -> None:
    page_url = (REPO / "webconfig/config.htm").as_uri()
    with sync_playwright() as p:
        browser = p.chromium.launch()
        # Export hands over a file as well as filling the panel, and a page that cannot
        # download throws rather than posing for the shot.
        context = browser.new_context(viewport={"width": WIDTH, "height": 1200},
                                      device_scale_factor=2, accept_downloads=True)
        page = context.new_page()
        page.clock.set_fixed_time(FIXED_TIME)
        errors: list[str] = []
        page.on("pageerror", lambda e: errors.append(str(e)))
        page.goto(page_url)
        page.wait_for_selector("#panel", timeout=15000)
        undriven = page.evaluate(DRIVE, [{str(k): v for k, v in VALUES.items()},
                                         firmware_version(), firmware_checksum()])
        page.wait_for_timeout(400)

        # The double-tap preview animates on a 7s loop, so an unpinned capture would land
        # on a different frame every run and churn the committed PNG. Hold it at 17%, the
        # pointer's first push against the edge, which is the moment the setting is about.
        page.evaluate("""() => document.getAnimations().forEach(a => {
            if (String(a.animationName || '').startsWith('dh-dtap')) {
              a.pause();
              a.currentTime = 1190;
            }
        })""")

        # The Save bar is position:sticky. A full_page capture paints it at the initial
        # viewport's bottom edge, over the Keep awake section. Growing the viewport to the
        # whole document puts it back where it sits naturally, at the foot of the card.
        page.set_viewport_size({"width": WIDTH,
                                "height": page.evaluate("document.documentElement.scrollHeight")})
        page.wait_for_timeout(200)

        shots = {}
        for name, spec in BOXES.items():
            spec = spec if isinstance(spec, dict) else {"box": spec}
            pad = spec.get("pad", PAD)

            page.evaluate(RESET)

            if "setup" in spec:
                page.evaluate(spec["setup"])
                page.wait_for_timeout(250)

            box = page.evaluate(spec["box"])
            left, top = max(0, box["x"] - pad), max(0, box["y"] - pad)
            shots[name] = page.screenshot(clip={
                "x": left,
                "y": top,
                "width": min(WIDTH - left, box["width"] + 2 * pad),
                "height": box["height"] + box["y"] - top + pad,
            })

        browser.close()

    if errors:
        sys.exit(f"page raised errors, refusing to write a broken screenshot: {errors}")
    if undriven:
        print(f"warning: settings {', '.join(undriven)} are on the page but not in VALUES, so "
              "they render at their default - add them from webconfig/form.py", file=sys.stderr)

    for name, shot in shots.items():
        image = Image.open(io.BytesIO(shot))
        write(name, shot, FINAL_WIDTH if name == "config-page-extended" else image.width // 2)


if __name__ == "__main__":
    main()
