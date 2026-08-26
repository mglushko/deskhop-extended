#!/usr/bin/python3

# Takes a HTML file, outputs a minified and compressed version that self-decompresses when loaded.
# This way, the device config page can be fitted in a small 64 kB "flash" partition and distributed
# with the main binary.

from jinja2 import Environment, FileSystemLoader
from form import *
import base64
import importlib.util
import os
import re
import zlib

# Input and output
TEMPLATE_PATH = "templates/"
INPUT_FILENAME = "main.html"
PACKER_FILENAME = "packer.j2"
OUTPUT_FILENAME = "config.htm"
OUTPUT_UNPACKED = "config-unpacked.htm"
OUTPUT_TEST = "config-test.htm"
HERE = os.path.dirname(os.path.abspath(__file__))
CMAKELISTS = os.path.join(HERE, "..", "CMakeLists.txt")
DISK_REBUILDER = os.path.join(HERE, "..", "misc", "rebuild-disk-image.py")


def disk_capacity():
    """Bytes available to config.htm inside the 64 kB FAT image.

    Taken from misc/rebuild-disk-image.py rather than restated here, so the geometry lives
    in one place. That script raises when the page does not fit, but disk/create.sh - what
    CI actually runs - does not: mkdosfs writes the file across as many clusters as it
    needs and the trailing `dd` then cuts the image back to 128 sectors, silently
    truncating the page. Checking at render time is what turns that into a build failure.
    """
    spec = importlib.util.spec_from_file_location("rebuild_disk_image", DISK_REBUILDER)
    disk = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(disk)

    tail = disk.IMAGE_LEN - (disk.DATA + (disk.FIRST_CLUSTER - 2) * disk.CLUSTER)

    # Whole clusters only: FAT cannot store the remainder past the last one. A page that
    # lands in it passes an unrounded check and is then truncated anyway, which is the
    # very thing this is here to catch. 43008 rather than 43520 at a 2048-byte cluster.
    return (tail // disk.CLUSTER) * disk.CLUSTER


def build_version():
    """Read the version out of CMakeLists.txt, so the label on the page and the number
    the firmware reports can never drift apart. The suffix is a label only - it is not
    part of the uint16 the boards exchange."""
    cmake = open(CMAKELISTS, encoding="utf-8").read()

    def get(name, default=""):
        found = re.search(r'set\(%s "?([^")]*)"?\)' % name, cmake)
        return found.group(1).strip() if found else default

    major, minor = int(get("VERSION_MAJOR", "0")), int(get("VERSION_MINOR", "0"))

    return {
        "version": "v%d.%s" % (major, get("VERSION_MINOR", "0")),
        "suffix": get("VERSION_SUFFIX"),
        # The uint16 the boards exchange, the form formatFwVersion() in script.js undoes.
        "raw": major * 1000 + minor + 100,
    }


def api_fields():
    """key -> {name, default} for every field form.py exposes, for the test page's
    emulated device. It seeds itself and labels packets from this, so config-test.htm
    never carries a second copy of the field map."""
    fields = {}

    for item in output_A() + output_B() + output_status() + output_config() + output_hotkeys():
        if item["elem"] == "label":
            continue
        fields[item["key"]] = {"name": item["name"], "default": item["default"] or 0}

    return fields

def render(filename, *args, **kwargs):
    env = Environment(loader=FileSystemLoader(TEMPLATE_PATH))
    template = env.get_template(filename)
    return template.render(*args, **kwargs)


def write_file(payload, filename=OUTPUT_FILENAME):
    with open(filename, 'w', encoding='utf-8') as file:
        file.write(payload)


def minify(page):
    """Strip from the rendered page what only a reader of it needs.

    Worth about 10 kB packed, which is most of what stands between config.htm and the
    43 kB the disk image has room for. The templates keep every comment and every
    indent: this runs on the rendered output, and only on the copy that gets packed.
    config-unpacked.htm and config-test.htm are still written from the page as rendered,
    so there is still something readable to open when the page misbehaves.

    Deliberately conservative, because a minifier that is wrong here breaks the only
    route into the device's settings. Block comments go, having checked that no /* or
    */ occurs inside a string or template literal anywhere in the templates. They are
    replaced by a space rather than nothing, so a comment sitting between two tokens
    cannot weld them together. Line comments stay: // also opens the authority in a URL,
    and telling those apart needs a real tokeniser to buy about 200 bytes.

    Only whitespace at the ends of lines goes, never the newlines themselves, so
    automatic semicolon insertion sees exactly what it saw before. That is safe because
    every template literal in script.js sits on one line and the one textarea in
    main.html is empty, leaving no run of spaces in the output that anything renders.
    """
    page = re.sub(r'/\*.*?\*/', ' ', page, flags=re.S)
    page = re.sub(r'[ \t]+\n', '\n', page)
    page = re.sub(r'\n[ \t]+', '\n', page)
    page = re.sub(r'\n{2,}', '\n', page)

    return page


def encode_file(payload):
    # Raw DEFLATE, at the level that squeezes hardest. This runs once at build time, so
    # the slower search costs nothing anyone waits on, and every byte it saves is a byte
    # of headroom in a budget the page has already nearly used up.
    compressed_data = zlib.compress(payload, 9)[2:-4]

    # Encode to base64
    base64_compressed_data = base64.b64encode(compressed_data).decode('utf-8')

    return base64_compressed_data


if __name__ == "__main__":
    context = dict(
        screen_A=output_A(),
        screen_B=output_B(),
        status=output_status(),
        config=output_config(),
        hotkeys=output_hotkeys(),
        build=build_version(),
    )

    # Read main template contents
    webpage = render(INPUT_FILENAME, **context)

    # Compress file and encode to base64. What gets packed is the minified page, so the
    # payload and the length the packer inflates into must both be taken from it and not
    # from the page as rendered. The packer inflates into a fixed-size Uint8Array and
    # tiny-inflate does not bounds check its destination, so a length that no longer
    # matches corrupts the page in the browser rather than failing here. Sizing it from
    # the same bytes removes the cliff instead of moving it, and stops the trailing slack
    # being decoded into the document.
    packed_page = minify(webpage).encode('utf-8')

    encoded_data = {
        'payload': encode_file(packed_page),
        'decoded_len': len(packed_page),
    }

    # Tiny Inflate JS decoder (https://github.com/foliojs/tiny-inflate)
    # Decompress the data and replace existing HTML with the decoded version
    self_extracting_webpage = render(PACKER_FILENAME, encoded_data)

    capacity = disk_capacity()
    packed = len(self_extracting_webpage.encode('utf-8'))

    if packed > capacity:
        raise SystemExit(f"config.htm is {packed} bytes, {packed - capacity} over the "
                         f"{capacity} available in the disk image - the page has outgrown it")

    # Write data to output filename
    write_file(self_extracting_webpage)

    print(f"{OUTPUT_FILENAME}: {packed} bytes of {capacity} available "
          f"({encoded_data['decoded_len']} unpacked)")

    # Write unpacked webpage
    write_file(webpage, OUTPUT_UNPACKED)

    # The same page with an emulated device appended, so it can be opened and driven in
    # any browser with no hardware. Never packed and never in the disk image - it is a
    # test artifact, not something the device serves.
    write_file(render(INPUT_FILENAME, mock=True, api_fields=api_fields(), **context),
               OUTPUT_TEST)