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

    return disk.IMAGE_LEN - (disk.DATA + (disk.FIRST_CLUSTER - 2) * disk.CLUSTER)


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

    for item in output_A() + output_B() + output_status() + output_config():
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


def encode_file(payload):
    # Compress using raw DEFLATE
    compressed_data = zlib.compress(payload.encode('utf-8'))[2:-4]

    # Encode to base64
    base64_compressed_data = base64.b64encode(compressed_data).decode('utf-8')

    return base64_compressed_data


if __name__ == "__main__":
    context = dict(
        screen_A=output_A(),
        screen_B=output_B(),
        status=output_status(),
        config=output_config(),
        build=build_version(),
    )

    # Read main template contents
    webpage = render(INPUT_FILENAME, **context)

    # Compress file and encode to base64. The decompressed length goes with it: the packer
    # inflates into a fixed-size Uint8Array and tiny-inflate does not bounds check its
    # destination, so a buffer that no longer fits the page corrupts it in the browser
    # rather than failing here. Sizing it from the page removes the cliff instead of
    # moving it, and stops the trailing slack being decoded into the document.
    encoded_data = {
        'payload': encode_file(webpage),
        'decoded_len': len(webpage.encode('utf-8')),
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