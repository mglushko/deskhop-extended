# Re-cutting the FAT geometry of the config-page image

Findings from investigating how much of the 64 kB disk image is spent on filesystem
overhead, and what it would take to get it back. **Nothing here is implemented.** This is a
parked design with a complete recipe, written so the next person does not have to
rediscover any of it.

Numbers are as of the commit this branch sits on, where `webconfig/config.htm` is 31,643
bytes.

## The problem

`config.htm` ships inside a 64 kB FAT12 image baked into the firmware. That image is the
only route into the device's settings and the only way to undo a keyboard shortcut. The
filesystem inside the 64 kB window spends 22 kB of it on overhead for a volume that holds
one file:

```
boot sector      512
FAT1            1536
FAT2            1536
root directory 16384   <- 512 entries, 3 of them used
wasted cluster 2 2048
---------------------
overhead       22016   of 65536
```

`create.sh` has been untouched since upstream v0.61 and lets `mkdosfs` pick defaults. 512
root entries is the default for a hard disk. Nothing about this volume is a hard disk.

Cutting the root directory to 64 entries reclaims 14,336 bytes, taking usable capacity from
43,008 to 57,344, which at the current page size is 55% full instead of 74%.

## Two bugs found on the way, both independent of any geometry change

**`webconfig/render.py`'s capacity formula overstates by 512 bytes.** It computes
`IMAGE_LEN - (DATA + (FIRST_CLUSTER - 2) * CLUSTER)` and gets 43,520, but a file occupies
whole clusters, and the last one has to fit inside the image. `rebuild()` in
`misc/rebuild-disk-image.py` therefore accepts at most 43,008. A page between 43,009 and
43,520 bytes passes `make` and is then rejected by the no-sudo path. The correct expression
is `((IMAGE_LEN - DATA) // CLUSTER - (FIRST_CLUSTER - 2)) * CLUSTER`. The overstatement is
exactly 512 bytes in every geometry considered here.

**The committed `disk.img` carries 9,660 stale bytes of an older page.** `rebuild()` zeroes
only `buf[start:end]`, the extent of the new payload, so clusters the previous, longer page
occupied keep their contents. Zeroing `buf[start:IMAGE_LEN]` instead makes the image a pure
function of `config.htm` rather than of its own history.

## The proposed geometry

`-r 64`, keeping two FATs, keeping the 2 MB volume declaration and the 128-sector
truncation.

| | now | after |
| --- | --- | --- |
| `BPB_RootEntCnt` | 512 | 64 |
| root directory | 16,384 B (32 sectors) | 2,048 B (4 sectors) |
| data area starts at | 19,968 (sector 39) | 5,632 (sector 11) |
| `BPB_FATSz16` | 3 | 3 (1,535 of 1,536 bytes used) |
| clusters | 1,014 | 1,021 |
| usable capacity | 43,008 | 57,344 |
| `config.htm` at 31,643 | 74% full | 55% full |

### Why 64 and not 16

16 entries would give another 2,048 bytes, and the pre-data sector count would even land
cluster-aligned. It is still the wrong choice. It leaves 13 free root slots, and both
Windows and macOS write housekeeping entries to removable volumes (`System Volume
Information`, `.fseventsd`, `.Spotlight-V100`, `.Trashes`), while a dragged
`deskhop-ex-vX.YZ-beta.uf2` needs three slots for its long-name plus 8.3 entries, and macOS
may add an AppleDouble `._` sidecar on top of that. 64 leaves 61 free and costs 2,048 bytes.

64 is also the smallest historically standard FAT12 root count. It is what Linux's
`floppy_defaults[]` carries for 160K and 180K media, and it is what Adafruit's `tinyuf2`
bootloader ships (`#define BPB_ROOT_DIR_ENTRIES (64)`), which is the closest production
analogue to this use case: a tiny synthetic FAT volume whose job is to accept a dragged
`.uf2`.

The spec constraint is only that `RootEntCnt * 32` be a whole number of sectors. There is no
minimum. The "use 512 for maximum compatibility" line in the Microsoft spec is scoped to
FAT16 and does not apply. Keeping the count a multiple of 16 matters for a subtler reason
than pedantry: Windows FASTFAT computes the data-area offset with truncating byte
arithmetic (`RootEntries * sizeof(DIRENT)`) while Linux and macOS round up to whole sectors,
so a non-multiple value would place cluster 2 at different offsets on different hosts. 16,
32 and 64 are all exact.

### Why two FATs, and why not chase the last 2 kB

Dropping to one FAT saves 1,536 bytes. The FAT spec warns that "some FAT file system drivers
might not recognize such a volume properly", `tinyuf2` deliberately asserts
`BPB_NUMBER_OF_FATS == 2` for compatibility, and `set_fat_entry()` in
`misc/rebuild-disk-image.py` hardcodes a two-FAT tuple whose second write address *is* the
root directory when `NumFATs == 1`. Not worth it.

Moving the file from cluster 3 to cluster 2 would save another 2,048. Cluster 3 is where the
Linux vfat allocator puts the first file: `fat_alloc_clusters` starts searching at
`prev_free + 1` and `prev_free` initialises to 2. Forcing cluster 2 means abandoning the
`sudo mount` and having Python synthesise the directory entry, which deletes the two
independent build paths that `--selftest` cross-validates. Also not worth it.

### The alignment question, settled

`mkfs.fat`'s man page warns that `-r` is a minimum that "may be increased by mkfs.fat due to
alignment of structures". It will not be, here. `setup_tables()` disables alignment entirely
for volumes of 8,192 sectors or fewer, and this volume declares 4,096. Verified in the
installed binary rather than from the documentation: at `0x6332` in `mkfs.fat` 4.2 there is
a `cmpl $0x2000,<num_sectors>` followed by `ja`, and the fall-through path zeroes the
`align_structures` global. This is also why the current image sits unaligned at sector 39.

## Recipe

### Step 1: the gate, worth landing on its own

CI installs `dosfstools` unpinned, runs `create.sh` on the runner, and ships *that* image
rather than the committed one, without ever checking it. A read-back verifier closes that
hole today, whether or not the geometry ever changes.

New `misc/verify-disk-image.py`, reading `config.htm` back out of `disk.img` with a parser
that shares no constants with the builder, walking the chain from the directory entry using
only what the boot sector says. It asserts:

- the BPB tuple `(BytsPerSec, SecPerClus, RsvdSecCnt, NumFATs, RootEntCnt, TotSec16, FATSz16)`
  matches the pinned expected value
- `len(image) == 65536`
- no cluster in the chain runs past 65,536
- the extracted bytes equal `webconfig/config.htm` exactly

Wire it into `.github/workflows/build.yml` twice: once on the committed image, once on the
image `create.sh` just built on the runner.

While in `misc/rebuild-disk-image.py`, note that `extract()` claims to be independent of the
writer and is not. It shares `DATA`, `CLUSTER`, `FAT0`, `ROOT` and `NAME` with `rebuild()`,
so a wrong geometry writes and reads back the same wrong offset and passes green. Its
annotation also says `-> bytes` while it returns a tuple.

### Step 2: the geometry, one commit, all files together

`disk/create.sh`, add `-r 64`:

```bash
mkdosfs -F12 -n DESKHOP -i 0 -r 64 fat.img
```

Everything else stays: the 2 MB volume, the mount and copy, the trailing `dd ... count=128`.

`misc/rebuild-disk-image.py`:

- `BPS, SPC, RESERVED, FATS, SPF, ROOT_ENTRIES = 512, 4, 1, 2, 3, 64`, so `ROOT` stays 3,584
  and `DATA` becomes 5,632. Update the `# 19968` comment.
- Keep `FIRST_CLUSTER = 3` and `IMAGE_LEN = 65536`. `IMAGE_LEN` must never be derived from
  `TotSec16`, which lies about being 2 MB.
- Add a `check_geometry(image)` that parses the BPB and raises `SystemExit` on any mismatch
  with the constants or on `len(image) != IMAGE_LEN`, called first thing in `rebuild()`.
  This is what makes constants-versus-image drift loud instead of silently corrupting.
- Add `capacity()` returning `((IMAGE_LEN - DATA) // CLUSTER - (FIRST_CLUSTER - 2)) * CLUSTER`.
- Make `set_fat_entry()` iterate `FATS` rather than hardcoding two FATs.
- Zero `buf[start:IMAGE_LEN]` rather than `buf[start:end]`.
- Do not extend the free-cluster loop to cover all declared clusters. Leaving the high
  clusters at `0x000` is exactly what makes the host believe there is ~2 MB free for a
  dropped `.uf2`.

`webconfig/render.py`: `disk_capacity()` becomes `return disk.capacity()`. Recast the
`minify()` docstring's "the 43 kB the disk image has room for" and the level-9 comment's "a
budget the page has already nearly used up", both of which stop being true at 57,344.

`README.md`: edit only the fork's own "Building the config page" paragraph. Do not touch the
`./create.sh` mention further down, which sits below the `<!-- upstream-readme-below` marker
and would be reverted by `misc/sync-upstream-readme.sh` on the next sync, breaking the
byte-identity that lets upstream README changes merge cleanly. The "same 64 kB flash budget"
line near the top stays true and stays as it is; the partition is unchanged, only the
filesystem overhead inside it shrinks.

Commit ordering matters. Everything goes in one commit. Constants-first silently corrupts
the image on the next rebuilder run; image-first writes a spurious second copy of the
payload. `--selftest` compares against `git show HEAD:`, so it is red until the commit lands
and green after.

### What must not change

`src/ramdisk.c` `NUMBER_OF_BLOCKS 4096` must keep matching `TotSec16`. Dropping it makes the
host see a nearly-full 64 kB volume and refuse the ~512 kB `.uf2`. `ACTUAL_NUMBER_OF_BLOCKS
128` must keep matching `IMAGE_LEN`, `create.sh`'s `count=128` and `misc/memory_map.ld`'s
`__DISK_IMAGE_LEN = 64k`. `CMakeLists.txt`'s `OBJECT_DEPENDS` on `disk.img` is the only
thing making an incremental build notice a regenerated image.

## Downsides

Serious:

- **A mistake removes its own recovery path.** The volume is the only settings UI, the only
  way to undo a shortcut, and the primary firmware upgrade path. A bad BPB shows as "You
  need to format the disk" and takes out the tool you would fix it with. What is left is
  BOOTSEL: unplug both boards, hold the physical button on each, flash individually, and
  settings do not survive that trip. `dist/` holds prior release `.uf2` files for rollback.
- **Zero existing test coverage.** `misc/test-config-page.py`, `misc/test-config-backup.py`
  and `misc/shoot-config-page.py` all load `webconfig/config.htm` over `file://` from the
  working tree. None opens `disk.img`. `misc/hosttest` never compiles `ramdisk.c`. The full
  suite passes green on a firmware whose config volume no host can mount.
- **The macOS root-directory analysis is desk analysis, never measured.** 61 free slots
  against a modelled worst case of about 25 is comfortable, but nobody has dragged a real
  `.uf2` onto a real 64-entry volume on a real Mac.
- **The toolchain is not pinned.** The geometry rests on the `num_sectors <= 8192` alignment
  exemption. If a future dosfstools drops that heuristic, the same command yields a
  different layout and a real capacity below what the guard reports, and `dd count=128`
  truncates the page with a green build. Step 1's BPB assertion is the mitigation. Adding
  `-a` pins the behaviour independently of the size heuristic.

Minor:

- The `disk.img` diff is unreviewable by eye, because regenerating rewrites the 9,660 stale
  bytes on top of every moved structure. The verifier is the review, not the diff.
- FAT slack drops to one spare byte: 1,021 clusters need 1,535 of 1,536. Harmless as
  written, but `set_fat_entry()` and `fat_entry()` have no bounds check, so any future nudge
  that adds a cluster writes into FAT2.
- The backed-but-free region grows from 12,288 to 26,624 bytes, so a host that re-reads a
  just-written `.uf2` (write-verify, an on-write virus scan, an evicted cache page) gets
  zeros across a window twice as wide. Nothing routinely re-reads, and the write path is
  unaffected.
- `create.sh` keeps needing sudo, and the image stays non-reproducible across machines
  because the directory entry's timestamp comes from the kernel clock at `cp` time.
  `--invariant` does not fix that; only abandoning the mount would.
- This diverges `create.sh` from upstream for the first time since v0.61.

## Verification

Automated:

```bash
misc/verify-disk-image.py disk/disk.img webconfig/config.htm
misc/rebuild-disk-image.py --selftest
cd webconfig && make          # must now report 57344, not 43520
fsck.fat -n -v disk/disk.img  # 1021 clusters, 3 sectors per FAT, 64 root entries
```

Sanity-check the guard by padding `config.htm` past 57,344, confirming `SystemExit`, and
reverting. Confirm no stale bytes remain past `5632 + 2048 + len(config.htm)`.

Manual, and unavoidable, because nothing above touches a real host FAT driver:

1. **Linux.** Enter config mode, confirm the volume automounts as DESKHOP with no format
   prompt, open the page, save a setting, then copy a release `.uf2` onto it and confirm the
   board reboots into it. Use a root USB port, not a hub: config-mode re-enumeration breaks
   behind hubs and would look exactly like a geometry failure.
2. **Windows 11.** Confirm Explorer mounts it and does not offer to format. Open the page in
   Edge and Chrome. Confirm roughly 2 MB free. Drag a release `.uf2` on and confirm the flash
   completes. Windows will also claim `System Volume Information`; confirm rather than assume.
3. **macOS**, the check the arithmetic cannot settle. Mount, confirm Finder shows
   `config.htm`, then `ls -la /Volumes/DESKHOP` and count what housekeeping it created. Drag
   a `.uf2` downloaded through a browser, so it carries `com.apple.quarantine` and provokes
   the `._` sidecar. A "directory full" or "disk full" error on a volume reporting 2 MB free
   is the failure this check exists to catch.

Do not cut a release from the geometry commit until all three have passed.

## Open questions, and what settles each

- Does `mkfs.fat` here actually emit `rootent=64` with the data area at 5,632? Run the
  amended `create.sh` and dump the BPB, asserting `(512, 4, 1, 2, 64, 4096, 3)`. Everything
  downstream is wrong if it differs.
- Does the vfat allocator still place the file at cluster 3 under the new geometry, or at 2?
  Worth 2,048 bytes. Read `DIR_FstClusLO` at `ROOT + 2*32 + 26` in the same dump. If it is
  2, set `FIRST_CLUSTER = 2` and capacity becomes 59,392.
- What geometry does the CI runner's unpinned dosfstools produce? Nobody has looked. Land
  step 1 and read the log. Worth knowing even if the geometry never changes.
- Does macOS accept the `.uf2` drag with 61 free slots? Only a Mac settles it.
- Do `-a` and `--invariant` change the image at all? Believed to be no-ops. Build with and
  without, and `cmp`.
