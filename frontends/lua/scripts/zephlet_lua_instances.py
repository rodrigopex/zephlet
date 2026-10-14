#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Collect LuaLS annotations for a build of the zephlet Lua frontend.

Codegen knows each zephlet *type* but never sees the instances: those exist
only as ZEPHLET_NEW(...) calls in application C. The per-instance hook names
its record `__zlet_lua__<type>__<instance>`, so the linked ELF lists exactly
the instances that were built -- Kconfig-disabled ones are left out, as they
were never linked.

Writes into --out:
  zephlets_base.lua       shared classes (copied from --base)
  <prefix>_lua_meta.lua   per-type classes (copied from --meta-dir/*/)
  zephlets_instances.lua  the `zephlets` module: one field per instance

Point LuaLS at --out (`workspace.library` in .luarc.json).
"""

import argparse
import re
import shutil
import sys
from pathlib import Path

from elftools.elf.elffile import ELFFile
from elftools.elf.sections import SymbolTableSection

SYMBOL = re.compile(r"^__zlet_lua__(?P<type>[a-z0-9]+(?:_[a-z0-9]+)*?)__(?P<inst>\w+)$")


def instances(elf_path: Path) -> list[tuple[str, str]]:
    found = set()
    with elf_path.open("rb") as f:
        for section in ELFFile(f).iter_sections():
            if not isinstance(section, SymbolTableSection):
                continue
            for sym in section.iter_symbols():
                m = SYMBOL.match(sym.name)
                if m:
                    found.add((m["inst"], m["type"]))
    return sorted(found)


CLASS = re.compile(r"^---@class zephlets\.(?P<type>\w+)\b", re.MULTILINE)


def meta_type(path: Path) -> str | None:
    """The zephlet type a <prefix>_lua_meta.lua annotates, or None for the stub
    of a type that does not set the `lua` option."""
    m = CLASS.search(path.read_text(encoding="utf-8"))
    return m["type"] if m else None


def render(pairs: list[tuple[str, str]]) -> str:
    lines = [
        "---@meta zephlets",
        "-- GENERATED FILE — DO NOT EDIT. Written after the link from the",
        "-- instance records in the ELF.",
        "",
        "---@class zephlets.Module",
        "---@field FOREVER integer timeout_ms meaning wait forever",
    ]
    for inst, type_name in pairs:
        lines.append(f"---@field {inst} zephlets.{type_name}")
    lines += [
        "local M = {}",
        "",
        "---Per-instance event counters, keyed by instance name.",
        "---@return table<string, zephlets.Stats>",
        "function M.stats() end",
        "",
        "---Wait for the next event of any of insts, oldest first, parking the",
        "---calling zephyr.async task. meta.source says which instance sent it.",
        "---@param insts table[] instances",
        "---@param timeout_ms? integer nil or FOREVER = wait forever, 0 = no wait",
        "---@return table|nil ev",
        "---@return zephlets.EventMeta|\"timeout\" meta",
        "function M.await_any(insts, timeout_ms) end",
        "",
        "return M",
        "",
    ]
    return "\n".join(lines)


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--elf", required=True, type=Path)
    p.add_argument("--meta-dir", required=True, type=Path)
    p.add_argument("--base", required=True, type=Path)
    p.add_argument("--out", required=True, type=Path)
    args = p.parse_args()

    pairs = instances(args.elf)
    types = {type_name for _, type_name in pairs}

    args.out.mkdir(parents=True, exist_ok=True)
    shutil.copy(args.base, args.out / args.base.name)
    # Only the types this build links: a zephlet removed or renamed since an
    # earlier build leaves its old annotations behind, here and in meta-dir.
    kept = set()
    for meta in sorted(args.meta_dir.glob("*/*_lua_meta.lua")):
        if meta_type(meta) in types:
            shutil.copy(meta, args.out / meta.name)
            kept.add(meta.name)
    for stale in args.out.glob("*_lua_meta.lua"):
        if stale.name not in kept:
            stale.unlink()

    (args.out / "zephlets_instances.lua").write_text(render(pairs), encoding="utf-8")
    print(f"zephlets LuaLS: {len(pairs)} instance(s) -> {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
