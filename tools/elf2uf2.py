#!/usr/bin/env python3
"""Minimal RP2040 ELF -> UF2 converter (no picotool required).

Usage:
    python elf2uf2.py <input.elf> <output.uf2> [objcopy_path]

It runs `arm-none-eabi-objcopy -O binary` to get the raw flash image and wraps
it in the UF2 container with the RP2040 family id (0xE48BFF56), producing a
.uf2 byte-compatible with what `picotool uf2` generates.
"""
import os
import struct
import subprocess
import sys

UF2_MAGIC_START0 = 0x0A324655
UF2_MAGIC_START1 = 0x9E5D5157
UF2_MAGIC_END = 0x0AB16F30
UF2_FLAG_FAMILY_ID_PRESENT = 0x00002000
RP2040_FAMILY_ID = 0xE48BFF56
PAYLOAD = 256
FLASH_BASE = 0x10000000


def bin_to_uf2(bin_path, uf2_path, base_addr=FLASH_BASE):
    data = open(bin_path, "rb").read()
    num_blocks = (len(data) + PAYLOAD - 1) // PAYLOAD
    with open(uf2_path, "wb") as f:
        for i in range(num_blocks):
            chunk = data[i * PAYLOAD:(i + 1) * PAYLOAD]
            chunk = chunk.ljust(PAYLOAD, b"\x00")
            hdr = struct.pack(
                "<IIIIIIII",
                UF2_MAGIC_START0,
                UF2_MAGIC_START1,
                UF2_FLAG_FAMILY_ID_PRESENT,
                base_addr + i * PAYLOAD,
                PAYLOAD,
                i,
                num_blocks,
                RP2040_FAMILY_ID,
            )
            block = hdr + chunk.ljust(476, b"\x00") + struct.pack("<I", UF2_MAGIC_END)
            assert len(block) == 512, len(block)
            f.write(block)
    return num_blocks


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    elf = sys.argv[1]
    uf2 = sys.argv[2]
    objcopy = sys.argv[3] if len(sys.argv) > 3 else "arm-none-eabi-objcopy"
    bin_path = uf2 + ".bin"
    subprocess.check_call([objcopy, "-O", "binary", elf, bin_path])
    n = bin_to_uf2(bin_path, uf2)
    os.remove(bin_path)
    print("wrote %s (%d bytes, %d blocks)" % (uf2, os.path.getsize(uf2), n))
    return 0


if __name__ == "__main__":
    sys.exit(main())
