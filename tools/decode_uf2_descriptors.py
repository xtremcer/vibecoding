#!/usr/bin/env python3
"""Decode USB device + configuration descriptors from a Pico ELF (.elf) by
parsing the ELF directly: locate desc_device / desc_configuration symbols,
map their virtual address to the file offset via section headers, and read
the exact bytes. These are the exact bytes the device reports to the host."""
import struct
import sys

DESC_TYPES = {
    0x01: "DEVICE", 0x02: "CONFIG", 0x03: "STRING", 0x04: "INTERFACE",
    0x05: "ENDPOINT", 0x0B: "IAD", 0x21: "HID",
    0x24: "CS_INTERFACE", 0x25: "CS_ENDPOINT",
}


def parse_elf(path):
    with open(path, "rb") as f:
        data = f.read()
    assert data[:4] == b"\x7fELF"
    is64 = data[4] == 2
    # ELF32 little-endian assumed (ARM Cortex-M)
    e_shoff = struct.unpack_from("<I", data, 0x20)[0]
    e_shentsize = struct.unpack_from("<H", data, 0x2E)[0]
    e_shnum = struct.unpack_from("<H", data, 0x30)[0]
    e_shstrndx = struct.unpack_from("<H", data, 0x32)[0]
    shdrs = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        (sh_name, sh_type, sh_flags, sh_addr, sh_offset,
         sh_size, sh_link, sh_info, sh_addralign, sh_entsize) = struct.unpack_from("<IIIIIIIIII", data, off)
        shdrs.append(dict(name=sh_name, type=sh_type, addr=sh_addr,
                          offset=sh_offset, size=sh_size, link=sh_link))
    # section name strings
    shstr = shdrs[e_shstrndx]
    def shname(s):
        base = shstr["offset"] + s["name"]
        end = data.index(b"\x00", base)
        return data[base:end].decode()
    for s in shdrs:
        s["sname"] = shname(s)
    # symbol table
    symtab = next((s for s in shdrs if s["sname"] == ".symtab"), None)
    strtab = shdrs[symtab["link"]] if symtab else None
    symbols = {}
    if symtab and strtab:
        entsize = 16
        for i in range(symtab["size"] // entsize):
            base = symtab["offset"] + i * entsize
            (st_name, st_value, st_size, st_info, st_other, st_shndx) = struct.unpack_from("<IIIBBH", data, base)
            nbase = strtab["offset"] + st_name
            nend = data.index(b"\x00", nbase)
            name = data[nbase:nend].decode()
            symbols[name] = dict(value=st_value, size=st_size, shndx=st_shndx)
    return data, shdrs, symbols


def read_sym_bytes(data, shdrs, symbols, name):
    if name not in symbols:
        return None, None
    s = symbols[name]
    sec = shdrs[s["shndx"]]
    file_off = s["value"] - sec["addr"] + sec["offset"]
    return data[file_off:file_off + s["size"]], sec["sname"]


def walk_config(buf):
    i = 0
    lines = []
    while i + 2 <= len(buf):
        bLength = buf[i]
        bType = buf[i + 1]
        if bLength == 0:
            lines.append(f"    [{i:04x}] ZERO-LENGTH @ type {bType:#x} -> STOP")
            break
        name = DESC_TYPES.get(bType, f"TYPE_{bType:#x}")
        if bType == 0x02:
            wTotal = buf[i + 2] | (buf[i + 3] << 8)
            bNumIf = buf[i + 4]
            cfgVal = buf[i + 6]
            strI = buf[i + 7]
            lines.append(f"  CONFIG total={wTotal} #if={bNumIf} cfgVal={cfgVal} strIdx={strI}")
        elif bType == 0x04:
            itf, alt, nEP, cls, sub, proto, iI = buf[i + 2:i + 9]
            lines.append(f"  INTERFACE if={itf} alt={alt} #ep={nEP} class={cls:#x} sub={sub:#x} proto={proto:#x} strIdx={iI}")
        elif bType == 0x05:
            ep, attr = buf[i + 2], buf[i + 3]
            mps = buf[i + 4] | (buf[i + 5] << 8)
            iv = buf[i + 6]
            lines.append(f"  ENDPOINT addr={ep:#x} attr={attr:#x} maxpkt={mps} interval={iv}")
        elif bType == 0x0B:
            first, cnt, fcls, fsub, fproto = buf[i + 2:i + 7]
            lines.append(f"  IAD first={first} count={cnt} funcClass={fcls:#x} funcSub={fsub:#x} funcProto={fproto:#x}")
        elif bType == 0x21:
            bcd = buf[i + 3] | (buf[i + 4] << 8)
            nDesc = buf[i + 6]
            lines.append(f"  HID bcdHID={bcd:#x} #classDesc={nDesc} len={bLength}")
        else:
            lines.append(f"  {name} len={bLength} raw={buf[i:i+bLength].hex()}")
        i += bLength
    return lines


def decode_device(buf):
    bcdUSB = buf[2] | (buf[3] << 8)
    cls, sub, proto = buf[4], buf[5], buf[6]
    vid = buf[8] | (buf[9] << 8)
    pid = buf[10] | (buf[11] << 8)
    bcdDev = buf[12] | (buf[13] << 8)
    return [f"  DEVICE bcdUSB={bcdUSB:#06x} class={cls:#x} sub={sub:#x} proto={proto:#x}",
            f"         VID={vid:#06x} PID={pid:#06x} bcdDevice={bcdDev:#06x}"]


def main():
    for elf in sys.argv[1:]:
        print("=" * 72)
        print(f"ELF: {elf}")
        data, shdrs, symbols = parse_elf(elf)
        for sym in ("desc_device", "desc_configuration", "hid_report_descriptor"):
            raw, sec = read_sym_bytes(data, shdrs, symbols, sym)
            if raw is None:
                print(f"  [!] {sym} not found in symtab")
                continue
            print(f"--- {sym}  (size={len(raw)}, section={sec}) ---")
            if sym == "desc_device":
                for l in decode_device(raw):
                    print(l)
            elif sym == "desc_configuration":
                for l in walk_config(raw):
                    print(l)
            else:
                print(f"  raw={raw.hex()}")


if __name__ == "__main__":
    main()
