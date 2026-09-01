"""Full-type-coverage RPC round-trip over the real `zlet` shell, using
the `typelab_bench` instance (`zephlets/typelab/`).

Scalar value parsing itself is nanopb-textformat's own tested surface,
so this file is not trying to re-test the library. What it covers is
that each type reaches the library correctly *through* a real
`SHELL_STATIC_SUBCMD_SET_CREATE` tree and a codegen-emitted
`PB_TF_DEFINE` descriptor: a wrong descriptor row, a mismatched struct
name, or a lost type code would show up here as a value that fails to
round-trip through an actual `zlet typelab_bench set_X ...` / `get_X`
command typed at a console.
"""

from __future__ import annotations

import pytest
from twister_harness import DeviceAdapter, Shell


def _out(shell: Shell, cmd: str) -> str:
	return "\n".join(shell.get_filtered_output(shell.exec_command(cmd)))


# (rpc suffix, the value as typed, the value as printed back).
#
# Typed and printed differ for bytes on purpose: input takes `\xH[H]`
# among other escapes, while the printer always emits three-digit octal,
# because a hex escape runs on whenever the next byte is also a hex digit
# (`\x0` followed by `a` reads back as the single byte 0x0A) and three
# octal digits cannot. Round-trip therefore holds at the value level, not
# at the text level.
#
# The per-type set_X/get_X wrappers print "value: <printed>" (their
# message's one field is literally named `value`); the aggregate
# config/get_config prints "f_<suffix>: <printed>" (Config's own field
# names) — both derived from `printed` below rather than hand-duplicated,
# so the two RPC surfaces cannot silently drift apart.
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
	("bytes", r'"\xDE\xAD\xBE\xEF"', r'"\336\255\276\357"'),
	("fixed_bytes", r'"\xCA\xFE\xBA\xBE"', r'"\312\376\272\276"'),
	("string", '"hello"', '"hello"'),
]


@pytest.mark.parametrize("suffix,typed,printed", TYPE_CASES, ids=[c[0] for c in TYPE_CASES])
def test_set_get_round_trip(dut: DeviceAdapter, shell: Shell, suffix, typed, printed):
	assert shell.wait_for_prompt(), "shell prompt never appeared"
	expect = f"value: {printed}"

	set_out = _out(shell, f"zlet typelab_bench set_{suffix} value: {typed}")
	assert expect in set_out, set_out

	get_out = _out(shell, f"zlet typelab_bench get_{suffix}")
	assert expect in get_out, get_out


def test_config_round_trip_all_18_scalar_fields(dut: DeviceAdapter, shell: Shell):
	"""The aggregate `config`/`get_config` (one call, every field) is
	independent RPC machinery from the 18 set_X/get_X pairs above but
	reads/writes the *same* underlying `struct typelab_config` — prove
	that here rather than just asserting it in a comment."""
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	msg = ", ".join(f"f_{suffix}: {typed}" for suffix, typed, _ in TYPE_CASES)

	out = _out(shell, f"zlet typelab_bench config {msg}")
	for suffix, _, printed in TYPE_CASES:
		assert f"f_{suffix}: {printed}" in out, out

	out = _out(shell, "zlet typelab_bench get_config")
	for suffix, _, printed in TYPE_CASES:
		assert f"f_{suffix}: {printed}" in out, out


def test_config_and_set_x_share_state(dut: DeviceAdapter, shell: Shell):
	"""Setting one field via the aggregate `config` call, then reading it
	back via the narrower `get_uint32` — and vice versa — only agrees if
	both RPC surfaces operate on the same `z->config`."""
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	_out(shell, "zlet typelab_bench set_uint32 value: 777")
	out = _out(shell, "zlet typelab_bench get_config")
	assert "f_uint32: 777" in out, out

	_out(shell, "zlet typelab_bench config f_uint32: 111")
	out = _out(shell, "zlet typelab_bench get_uint32")
	assert "value: 111" in out, out


def test_hex_numeral_via_set_x(dut: DeviceAdapter, shell: Shell):
	"""`0x` hex is a numeral for the integer families, distinct from the
	`\\x` escape that appears inside a bytes literal."""
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	out = _out(shell, "zlet typelab_bench set_fixed32 value: 0x12345678")
	assert "value: 305419896" in out, out


def test_bytes_accepts_mixed_escapes_and_ascii(dut: DeviceAdapter, shell: Shell):
	"""A bytes value is a string literal, so escapes and printable ASCII
	mix freely — a payload the old `h<hex>` form could not express at
	all. Also proves the backslashes survived SHELL_OPT_ARG_RAW, which
	is the only reason they reach the parser intact."""
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	out = _out(shell, r'zlet typelab_bench set_bytes value: "\x0F\x0Ahello\x1E"')
	assert r'value: "\017\012hello\036"' in out, out


def test_string_value_may_contain_spaces(dut: DeviceAdapter, shell: Shell):
	"""Impossible under the old positional grammar: the shell's own
	tokeniser split on whitespace and stripped the quotes before the
	handler ran."""
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	out = _out(shell, 'zlet typelab_bench set_string value: "hi there"')
	assert 'value: "hi there"' in out, out


def test_narrow_enum_out_of_range_is_rejected_not_truncated(dut: DeviceAdapter, shell: Shell):
	"""TypelabUFlag/TypelabSFlag declare two values each, so the compiler
	stores them in a single byte. The old frontend assigned through a
	narrowing C cast to the field's declared type with no range check, so
	`set_enum 1000000` silently became `1000000 & 0xFF == 64`.

	The library dispatches every numeric write on the field's own
	`data_size`, read from the descriptor rather than assumed, so an
	out-of-range value is refused instead. This is the one behaviour
	change here that fixes a bug rather than moving syntax around."""
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	out = _out(shell, "zlet typelab_bench set_enum value: 1000000")
	assert "out of range" in out, out
	assert "value: 64" not in out, out


def test_negative_in_unsigned_field_is_rejected_not_wrapped(dut: DeviceAdapter, shell: Shell):
	"""Same root cause as the enum case: a negative literal in an
	unsigned field is a range error, never a wrap to a huge positive."""
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	out = _out(shell, "zlet typelab_bench set_uint32 value: -1")
	assert "out of range" in out, out
	assert "value: 4294967295" not in out, out


def test_malformed_value_rejected_cleanly(dut: DeviceAdapter, shell: Shell):
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	out = _out(shell, "zlet typelab_bench set_uint32 value: not-a-number")
	assert "set_uint32:" in out, out
	assert "value: not-a-number" not in out, out
