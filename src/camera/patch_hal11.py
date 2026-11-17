#!/usr/bin/env python3
# Patch YOUR OWN copy of Samsung's camera HAL so it survives in the postmarketOS container.
#   usage: python3 patch_hal11.py camera.s5e9945.so camera.s5e9945.hal11.so
# Input: /vendor/lib64/hw/camera.s5e9945.so from firmware S921BXXU6BYE7 (md5 e6499fe0ac2d190096b0464ca5f3ab68).
# Output md5 should be ec01ddb2b5ceeb4f6c123a901226bca2. Any other input is refused (the offsets are
# exact addresses in that one build). Install the output as /usr/local/share/s24/camera.s5e9945.hal11.so;
# s24-camera-run bind-mounts it over the stock library inside the container only.
# What it changes (all explained in docs/hardware/camera.md):
#   1. LooperWatchdog::notify: no deliberate crash when its timer fires (the container is slow)
#   2. Scenario::waitPreviousAsyncCtlTask: returns at once (keeps its BTI/PAC landing pad)
#   3. a constructor that ignores SIGPIPE (cameraserver's dropped connections killed the provider)
#   4. destroyCamera: its deliberate crash on a half-set-up frame becomes the normal return
#   5. DOF::startInternal: the depth-of-field plugin is always skipped (a null call in live preview)
#
# History notes from the port follow (v5 = 1-3, v6 = +4, v11 = +5).
# HAL patch v6 = v5 (stability + baked SIGPIPE-ignore ctor) PLUS neutralize the destroyCamera
# deliberate-crash (S24, Claude 2026-11-16). destroyCamera (…around 0x2bbd40) validates two
# Image/Frame state fields ([x21+16],[x20+16] must be 2|3); in the container they aren't, so it
# takes the error epilogue, logs, then hits `mov x8,#0; str 0xdeaddead,[x8]` at 0x2bbe84 (a
# Samsung LOG_ALWAYS_FATAL-style self-crash). That kills the provider when s24-cam opens/configures
# (and when cameraserver's close leaves a deferred destroy). Fix: replace the crash store with
# `b 0x2bbe10` — the normal success return (0x2bbe10 = `mov x0,xzr` → frame restore → autiasp → ret).
# So the error path returns 0 cleanly instead of crashing.
import shutil, struct, sys, hashlib

src = sys.argv[1] if len(sys.argv) > 2 else "camera.s5e9945.so"
dst = sys.argv[2] if len(sys.argv) > 2 else (sys.argv[1] if len(sys.argv) > 1 else "camera.s5e9945.hal11.so")
STOCK_MD5 = "e6499fe0ac2d190096b0464ca5f3ab68"
got = hashlib.md5(open(src, "rb").read()).hexdigest()
if got != STOCK_MD5:
    sys.exit("%s has md5 %s, not the S921BXXU6BYE7 library (%s): not patching" % (src, got, STOCK_MD5))
STUB_VA = 0x57ce40; IA0_OFF = 0x5885b8
shutil.copy(src, dst); b = bytearray(open(dst, "rb").read())

# stability
for off, old, new in [
    (0x223c08, 0xb9000109, 0xd503201f),
    (0x3207a4, 0xd10343ff, 0xd50323bf),
    (0x3207a8, 0xa90b7bfd, 0xd65f03c0),
]:
    assert struct.unpack("<I", b[off:off+4])[0] == old, "0x%x" % off
    b[off:off+4] = struct.pack("<I", new)

# SIGPIPE-ignore constructor (identical to v5)
orig_ctor = struct.unpack("<Q", b[IA0_OFF:IA0_OFF+8])[0]; assert orig_ctor == 0x1f37c0
words = [0xD503245F,0xA9BC7BFD,0x910003FD,0xA90107E0,0xF90013E2,0xA902FFFF,0xF9001FFF,0xD2800028,
         0xF90017E8,0xD28001A0,0x9100A3E1,0xD2800002,0xD2800103,0xD28010C8,0xD4000001,0xA94107E0,
         0xF94013E2,0xA8C47BFD]
b_va = STUB_VA + len(words)*4
words.append(0x14000000 | (((orig_ctor - b_va) >> 2) & 0x03FFFFFF))
stub = b"".join(struct.pack("<I", w) for w in words)
assert all(x == 0 for x in b[STUB_VA:STUB_VA+len(stub)])
b[STUB_VA:STUB_VA+len(stub)] = stub
b[IA0_OFF:IA0_OFF+8] = struct.pack("<Q", STUB_VA)

# destroyCamera deliberate-crash -> branch to the normal success return
CRASH_OFF = 0x2bbe84; RET_VA = 0x2bbe10
assert struct.unpack("<I", b[CRASH_OFF:CRASH_OFF+4])[0] == 0xb9000109, "destroyCamera crash moved"
b_insn = 0x14000000 | (((RET_VA - CRASH_OFF) >> 2) & 0x03FFFFFF)
b[CRASH_OFF:CRASH_OFF+4] = struct.pack("<I", b_insn)
print("destroyCamera 0x%x: str 0xdeaddead,[0] -> b 0x%x (%08x)" % (CRASH_OFF, RET_VA, b_insn))

# v11: DOF::startInternal null-vtable blr @0x2a07e0 (bokeh plugin) crashes in continuous streaming.
# Force the existing `cbz x0,0x2a0874` @0x2a07cc unconditional so DOF always skips the crash and
# returns 0 (success). Preview needs no depth-of-field.
assert struct.unpack("<I", b[0x2a07cc:0x2a07d0])[0] == 0xb4000540, "DOF cbz moved"
b[0x2a07cc:0x2a07d0] = struct.pack("<I", 0x14000000 | (((0x2a0874 - 0x2a07cc) >> 2) & 0x03FFFFFF))
print("DOF::startInternal 0x2a07cc: cbz -> b 0x2a0874 (skip bokeh crash)")

open(dst, "wb").write(b)
print("md5", hashlib.md5(b).hexdigest(), "size", len(b))
