"""Two error surfaces, kept in one file because they are easy to
confuse.

**Command-tree lookup** — an unknown instance or rpc fails through
Zephyr's own static tree, with no custom not-found handling needed since
the tree is fully SHELL_STATIC_SUBCMD_SET_CREATE-generated. `zlet` and
each instance are registered with a NULL handler (they're pure
subcommand groups), so Zephyr responds to an unmatched child not with
"command not found" — that's for a genuinely unknown *root* command —
but by printing that level's help and its real subcommand list.

**Message parsing** — once an rpc leaf is reached, its raw argument goes
to pb_tf_parse(), whose failures carry a byte offset and, when a field
was in scope, that field's name.
"""

from __future__ import annotations

from twister_harness import DeviceAdapter, Shell


def _out(shell: Shell, cmd: str) -> str:
	return "\n".join(shell.get_filtered_output(shell.exec_command(cmd)))


def test_unknown_instance_rejected(dut: DeviceAdapter, shell: Shell):
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	out = _out(shell, "zlet nope get_status")
	assert "Subcommands:" in out, out
	assert "nope" not in out, out
	for instance in ("tick_fast", "tick_slow", "ui_main"):
		assert instance in out, out


def test_unknown_rpc_rejected(dut: DeviceAdapter, shell: Shell):
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	out = _out(shell, "zlet tick_fast nope")
	assert "Subcommands:" in out, out
	assert "nope" not in out, out
	for rpc in ("start", "stop", "get_status", "config", "get_config", "kick"):
		assert rpc in out, out


def test_unknown_field_reports_no_such_field(dut: DeviceAdapter, shell: Shell):
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	out = _out(shell, "zlet tick_fast config nope: 1")
	assert "no such field" in out, out


def test_malformed_value_reports_offset_and_field(dut: DeviceAdapter, shell: Shell):
	"""pb_tf_strerror() names only the fault, so the handler adds the
	offset and the field — both halves are asserted here."""
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	out = _out(shell, "zlet tick_fast config duration_ms: notanumber")
	assert "at offset" in out, out
	assert "duration_ms" in out, out


def test_out_of_range_value_is_a_range_error(dut: DeviceAdapter, shell: Shell):
	"""duration_ms is uint32; a value past its width is refused rather
	than silently truncated."""
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	out = _out(shell, "zlet tick_fast config duration_ms: 99999999999999")
	assert "out of range" in out, out
	assert "duration_ms" in out, out


def test_negative_value_in_unsigned_field_is_rejected(dut: DeviceAdapter, shell: Shell):
	"""A negative literal in an unsigned field is a range error, never a
	wrap to a huge positive."""
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	out = _out(shell, "zlet tick_fast config duration_ms: -1")
	assert "out of range" in out, out
	assert "duration_ms: 4294967295" not in out, out


def test_unterminated_brace_is_a_syntax_error(dut: DeviceAdapter, shell: Shell):
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	out = _out(shell, "zlet tick_fast config {duration_ms: 1")
	assert "config:" in out, out
	assert "duration_ms: 1" not in out, out


def test_failed_parse_does_not_reach_the_zephlet(dut: DeviceAdapter, shell: Shell):
	"""The parser writes straight into the destination with no scratch
	buffer, so a failed parse can leave partial bytes behind. The
	generated handler dispatches only on PB_TF_OK, so the zephlet's own
	config must be untouched by a rejected call."""
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	_out(shell, "zlet tick_fast config duration_ms: 250, period_ms: 250")
	_out(shell, "zlet tick_fast config duration_ms: 7, period_ms: notanumber")

	out = _out(shell, "zlet tick_fast get_config")
	assert "duration_ms: 250" in out, out
	assert "period_ms: 250" in out, out
	assert "duration_ms: 7" not in out, out
