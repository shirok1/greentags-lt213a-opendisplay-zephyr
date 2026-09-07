"""Validate all loadable ELF regions, including RAM's Flash initialization image."""
from pathlib import Path
import argparse
import struct
from elftools.elf.elffile import ELFFile


def check(path):
    with Path(path).open("rb") as stream:
        elf = ELFFile(stream)
        flash_end = ram_end = 0
        for segment in elf.iter_segments():
            if segment["p_type"] != "PT_LOAD":
                continue
            physical, virtual = segment["p_paddr"], segment["p_vaddr"]
            if segment["p_filesz"]:
                end = physical + segment["p_filesz"]
                if not 0 <= physical < end <= 126 * 1024:
                    raise ValueError(f"Application overlaps reserved configuration Flash: {physical:#x}..{end:#x}")
                flash_end = max(flash_end, end)
            if segment["p_memsz"] and virtual >= 0x20000000:
                end = virtual + segment["p_memsz"]
                if not 0x20000000 <= virtual < end <= 0x20004000:
                    raise ValueError(f"RAM outside 16 KiB: {virtual:#x}..{end:#x}")
                ram_end = max(ram_end, end - 0x20000000)
        vectors = elf.get_section_by_name("rom_start")
        if vectors is None or vectors["sh_addr"] != 0:
            raise ValueError("Vector table must start at Flash address 0")
        sp, reset = struct.unpack_from("<II", vectors.data())
        if not 0x20000000 < sp <= 0x20004000 or not reset & 1 or reset >= flash_end:
            raise ValueError("Invalid initial SP/reset vector")
    print(f"Application Flash: {flash_end:,} / 129,024 bytes (2,048 bytes reserved for configuration); RAM: {ram_end:,} / 16,384 bytes")
    print(f"Remaining RAM outside statically reserved stacks/buffers: {16384 - ram_end:,} bytes")
    return flash_end, ram_end


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf")
    check(parser.parse_args().elf)
