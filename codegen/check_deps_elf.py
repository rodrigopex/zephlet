#!/usr/bin/env python3
"""Post-link check of zephlet dependency bindings.

Reads every instance of the `zephlet` iterable section from the linked ELF,
with its type's dependency slots and its dependency record, and fails on

  - an instance of a type with dependencies but no record,
  - an unbound slot, or one bound to something that is not an instance,
  - a slot bound to an instance of another type.

At boot an instance with such a binding is skipped with -ENODEV; this moves
the failure to the build. A record in `.bss` (a zeroed, non-const one) reads
as all NULL: every slot unbound.

Instance cycles are not checked here: a slot only accepts an instance of
its own type, so a cycle of instances needs a cycle of types, which the
dependency-graph build step rejects. Only hand-written apis could form one,
and those instances are skipped at boot.

It checks the instances the image holds. On a native target the final link
(zephyr.exe) drops sections nothing references, so a hand-written record
without an init entry (ZEPHLET_INIT_ENTRY) is not there to check; every
ZEPHLET_NEW instance has one.

Every field read is pointer-sized; zephlet.c BUILD_ASSERTs the layout
below, so a change to the structs fails the build there first.
"""

import argparse
import sys

from elftools.elf.elffile import ELFFile

# Field indexes, in pointers (zephlet.c BUILD_ASSERTs them).
ZEPHLET_NAME, ZEPHLET_API, ZEPHLET_DEPS, ZEPHLET_SIZE = 0, 1, 6, 10
API_DEPS, API_NUM_DEPS = 2, 3
SLOT_NAME, SLOT_TYPE, SLOT_API, SLOT_OFFSET, SLOT_SIZE = 0, 1, 2, 3, 4


class Image:
    """Reads pointers and strings out of the ELF's loaded sections; a
    NOBITS section (`.bss`) reads as zeros."""

    def __init__(self, elf):
        self.ptr = elf.elfclass // 8
        self.order = "little" if elf.little_endian else "big"
        self.sections = []
        for s in elf.iter_sections():
            if not s["sh_flags"] & 0x2:
                continue
            if s["sh_type"] == "SHT_NOBITS":
                data = bytes(s["sh_size"])
            else:
                data = s.data()
            self.sections.append((s["sh_addr"], data))
        self.symbols = {}
        for sec in elf.iter_sections():
            if sec["sh_type"] == "SHT_SYMTAB":
                for sym in sec.iter_symbols():
                    self.symbols[sym.name] = sym["st_value"]

    def read(self, addr, size):
        for base, data in self.sections:
            if base <= addr and addr + size <= base + len(data):
                return data[addr - base : addr - base + size]
        raise LookupError(f"address {addr:#x} is not in a loaded section")

    def word(self, addr, index=0):
        at = addr + index * self.ptr
        return int.from_bytes(self.read(at, self.ptr), self.order)

    def string(self, addr):
        for base, data in self.sections:
            if base <= addr < base + len(data):
                end = data.index(b"\0", addr - base)
                return data[addr - base : end].decode()
        raise LookupError(f"string at {addr:#x} is not in a loaded section")

    def span(self, name):
        start = self.symbols.get(f"_{name}_list_start")
        end = self.symbols.get(f"_{name}_list_end")
        if start is None or end is None:
            return None
        return start, end


def check(path):
    with open(path, "rb") as f:
        elf = ELFFile(f)
        if elf["e_type"] == "ET_REL":
            return [f"{path} is relocatable; check the final, linked image"]
        img = Image(elf)

        span = img.span("zephlet")
        if span is None:
            return []
        start, end = span
        size = ZEPHLET_SIZE * img.ptr
        addrs = [start + k * size for k in range((end - start) // size)]
        names = {a: img.string(img.word(a, ZEPHLET_NAME)) for a in addrs}
        errors = []

        for a in addrs:
            name, api = names[a], img.word(a, ZEPHLET_API)
            if api == 0:
                continue
            num, slots = img.word(api, API_NUM_DEPS), img.word(api, API_DEPS)
            if num == 0:
                continue
            record = img.word(a, ZEPHLET_DEPS)
            if record == 0:
                errors.append(f"'{name}' has no dependency record")
                continue
            for d in range(num):
                slot = slots + d * SLOT_SIZE * img.ptr
                sname = img.string(img.word(slot, SLOT_NAME))
                stype = img.string(img.word(slot, SLOT_TYPE))
                p = img.word(record + img.word(slot, SLOT_OFFSET))
                where = f"'{name}': dependency '{sname}' (a {stype})"
                if p == 0:
                    errors.append(f"{where} is not bound")
                elif p not in names:
                    errors.append(f"{where} is not bound to a zephlet instance")
                elif img.word(p, ZEPHLET_API) != img.word(slot, SLOT_API):
                    errors.append(f"{where} is bound to '{names[p]}', of another type")
        return errors


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("elf", help="the linked zephyr ELF")
    args = parser.parse_args()

    try:
        errors = check(args.elf)
    except LookupError as err:
        errors = [f"cannot read the bindings from {args.elf}: {err}"]
    for err in errors:
        print(f"error: zephlet: {err}", file=sys.stderr)
    if errors:
        print(
            "error: zephlet: dependency bindings rejected "
            "(CONFIG_ZEPHLET_DEPS_LINK_CHECK)",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
