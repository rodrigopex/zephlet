"""Full-type-coverage RPC round-trip over the real `zlet` shell, using
the `typelab_bench` instance (`zephlets/typelab/`).

`tests/shell_macros/` already exercises all 18 supported nanopb scalar
types, but against the macro framework directly (PARSE_FIELD/PRINT_FIELD
called in isolation, a throwaway registered shell command for the print
side) — it never goes through a real `SHELL_STATIC_SUBCMD_SET_CREATE`
tree or `shell_execute_cmd()`. `tests/shell_functional/`'s other tests
(test_rpc.py) go through the real shell but only ever touch `tick`'s two
`uint32` config fields. Neither proves every type dispatches correctly
through an actual `zlet typelab_bench set_X <value>` / `get_X` command
typed at a real console — this file does.
"""

from __future__ import annotations

import pytest
from twister_harness import DeviceAdapter, Shell


def _out(shell: Shell, cmd: str) -> str:
	return "\n".join(shell.get_filtered_output(shell.exec_command(cmd)))


# (rpc suffix, `set_<suffix>` argument, the printed value as it appears
# after " = "). One row per nanopb scalar type the shell frontend
# supports — declaration order matches zlet_typelab.proto's Config.
#
# The per-type set_X/get_X wrappers always print "value = <printed>"
# (their message's one field is literally named `value`); the aggregate
# config/get_config prints "f_<suffix> = <printed>" (Config's own field
# names) — both forms are derived from `printed` below rather than
# hand-duplicated, so the two RPC surfaces can't silently drift apart.
TYPE_CASES = [
	("uint32", "4000000000", "4000000000"),
	("uint64", "18000000000000000000", "18000000000000000000"),
	("fixed32", "305419896", "305419896"),
	("fixed64", "1234605616436508552", "1234605616436508552"),
	("uenum", "1", "1"),
	("int32", "-2000000000", "-2000000000"),
	("int64", "-9000000000000000000", "-9000000000000000000"),
	("sint32", "-12345", "-12345"),
	("sint64", "-123456789012345", "-123456789012345"),
	("sfixed32", "-1", "-1"),
	("sfixed64", "-1", "-1"),
	("enum", "-1", "-1"),
	("float", "3.5", "3.5"),
	("double", "-2.25", "-2.25"),
	("bool", "true", "true"),
	("bytes", 'hDEADBEEF', 'h"deadbeef"'),
	("fixed_bytes", 'hCAFEBABE', 'h"cafebabe"'),
	("string", "hello", '"hello"'),
]


@pytest.mark.parametrize("suffix,set_arg,printed", TYPE_CASES, ids=[c[0] for c in TYPE_CASES])
def test_set_get_round_trip(dut: DeviceAdapter, shell: Shell, suffix, set_arg, printed):
	assert shell.wait_for_prompt(), "shell prompt never appeared"
	expect = f"value = {printed}"

	set_out = _out(shell, f'zlet typelab_bench set_{suffix} {set_arg}')
	assert expect in set_out, set_out

	get_out = _out(shell, f"zlet typelab_bench get_{suffix}")
	assert expect in get_out, get_out


def test_config_round_trip_all_18_fields_decimal(dut: DeviceAdapter, shell: Shell):
	"""The aggregate `config`/`get_config` (one call, all 18 fields) is
	independent RPC machinery from the 18 set_X/get_X pairs above but
	reads/writes the *same* underlying `struct typelab_config` — prove
	that here rather than just asserting it in a comment."""
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	args = ("4000000000 18000000000000000000 305419896 1234605616436508552 1 "
		"-2000000000 -9000000000000000000 -12345 -123456789012345 -1 -1 -1 "
		"3.5 -2.25 true hDEADBEEF hCAFEBABE hello")
	out = _out(shell, f"zlet typelab_bench config {args}")
	for suffix, _, printed in TYPE_CASES:
		assert f"f_{suffix} = {printed}" in out, out

	out = _out(shell, "zlet typelab_bench get_config")
	for suffix, _, printed in TYPE_CASES:
		assert f"f_{suffix} = {printed}" in out, out


def test_config_and_set_x_share_state(dut: DeviceAdapter, shell: Shell):
	"""Setting one field via the aggregate `config` call, then reading
	it back via the narrower `get_uint32` — and vice versa — only
	agrees if both RPC surfaces operate on the same `z->config`."""
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	# get_config's other 17 fields are whatever the previous test left
	# them at; only f_uint32 needs to change here, so read-modify-write
	# through get_config isn't needed — set_uint32 alone proves it.
	_out(shell, "zlet typelab_bench set_uint32 777")
	out = _out(shell, "zlet typelab_bench get_config")
	assert "f_uint32 = 777" in out, out

	_out(shell, "zlet typelab_bench config 111 0 0 0 0 0 0 0 0 0 0 0 0 0 false h00 h00000000 x")
	out = _out(shell, "zlet typelab_bench get_uint32")
	assert "value = 111" in out, out


def test_hex_form_via_set_x(dut: DeviceAdapter, shell: Shell):
	"""`h<hex>` is a hex numeral (not a byte-dump) for integer families —
	confirm that still holds through set_X, not just the aggregate
	`config` call or the tests/shell_macros ZTEST."""
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	out = _out(shell, 'zlet typelab_bench set_fixed32 h12345678')
	assert "value = 305419896" in out, out

	out = _out(shell, 'zlet typelab_bench set_sfixed64 hFFFFFFFFFFFFFFFF')
	assert "value = -1" in out, out


def test_malformed_value_rejected_cleanly(dut: DeviceAdapter, shell: Shell):
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	out = _out(shell, "zlet typelab_bench set_uint32 not-a-number")
	assert "set_uint32:" in out, out
	assert "value = not-a-number" not in out, out
