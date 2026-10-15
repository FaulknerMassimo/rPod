"""IPSW -> Firmware-4.x image -> osos (retail OS) image.

The Apple firmware file starts with an ASCII "STOP" banner, a "]ih[" header at
0x100 whose u32 at 0x104 is the directory offset (0x4000), and the directory
itself sits 0x200 bytes further in the file (0x4200). Directory entries are 40
bytes: type[4] id[4] pad devOffset len addr entryOffset checksum vers loadAddr.
Image bytes live at devOffset + 0x200. The checksum is the byte sum.

osos is linked at virtual address 0: the reset code programs the PP5020
memory mapper (0xF000F000) to alias SDRAM 0x10000000 at 0 and jumps to 0x228.
"""
import struct
import zipfile

DIR_FILE_OFFSET = 0x4200
IMAGE_BIAS = 0x200


def read_ipsw(path):
    with zipfile.ZipFile(path) as z:
        names = z.namelist()
        fw_name = next(n for n in names if n.startswith('Firmware-'))
        manifest = z.read('manifest.plist') if 'manifest.plist' in names else None
        return fw_name, z.read(fw_name), manifest


def directory(fw):
    entries = []
    for i in range(16):
        e = fw[DIR_FILE_OFFSET + i * 40: DIR_FILE_OFFSET + i * 40 + 40]
        if len(e) < 40 or e[:4] == b'\0\0\0\0':
            break
        typ, id_, _pad, devoff, ln, addr, entry, ck, vers, load = struct.unpack('<4s4sIIIIIIII', e)
        blob = fw[devoff + IMAGE_BIAS: devoff + IMAGE_BIAS + ln]
        entries.append(dict(type=typ.decode('latin1'), id=id_[::-1].decode('latin1'), dev_offset=devoff,
                            length=ln, addr=addr, entry=entry, checksum=ck, vers=vers, load_addr=load,
                            checksum_ok=(sum(blob) & 0xffffffff) == ck, data=blob))
    return entries


def wrap_elf(raw, base, bss=0x200000):
    """Minimal ELF32 ARM executable (one RWX PT_LOAD) so disassemblers pick arch + base."""
    ehsz, phsz, shsz = 52, 32, 40
    data_off = 0x1000
    shstr = b'\0.text\0.shstrtab\0'
    shstr_off = data_off + len(raw)
    sh_off = (shstr_off + len(shstr) + 3) & ~3
    eh = struct.pack('<16sHHIIIIIHHHHHH', b'\x7fELF\x01\x01\x01' + b'\0' * 9, 2, 40, 1, base, ehsz, sh_off,
                     0x05000200, ehsz, phsz, 1, shsz, 3, 2)
    ph = struct.pack('<IIIIIIII', 1, data_off, base, base, len(raw), len(raw) + bss, 7, 0x1000)
    sh = b'\0' * 40
    sh += struct.pack('<IIIIIIIIII', 1, 1, 7, base, data_off, len(raw), 0, 0, 4, 0)
    sh += struct.pack('<IIIIIIIIII', 7, 3, 0, 0, shstr_off, len(shstr), 0, 0, 1, 0)
    buf = bytearray(eh + ph)
    buf += b'\0' * (data_off - len(buf))
    buf += raw + shstr
    buf += b'\0' * (sh_off - len(buf))
    buf += sh
    return bytes(buf)
