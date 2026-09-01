"""RPC round-trip over the `zlet` shell command: a text-format request,
the decimal and `0x` numeral forms, a custom (non-base) rpc, and the
argument handling that SHELL_OPT_ARG_RAW brings.
"""

from __future__ import annotations

from twister_harness import DeviceAdapter, Shell


def _out(shell: Shell, cmd: str) -> str:
	return "\n".join(shell.get_filtered_output(shell.exec_command(cmd)))


def test_config_get_config_round_trip_decimal(dut: DeviceAdapter, shell: Shell):
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	out = _out(shell, "zlet tick_fast config duration_ms: 250, period_ms: 250")
	assert "duration_ms: 250" in out, out
	assert "period_ms: 250" in out, out

	out = _out(shell, "zlet tick_fast get_config")
	assert "duration_ms: 250" in out, out
	assert "period_ms: 250" in out, out


def test_config_round_trip_hex_numeral(dut: DeviceAdapter, shell: Shell):
	"""Integers accept `0x` hex, the text-format spelling. The old
	frontend's own `h`-prefix form is gone."""
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	out = _out(shell, "zlet tick_fast config duration_ms: 0xFA, period_ms: 0xFA")
	assert "duration_ms: 250" in out, out
	assert "period_ms: 250" in out, out


def test_config_field_order_is_free(dut: DeviceAdapter, shell: Shell):
	"""Fields are named, not positional, so declaration order in the
	proto no longer constrains what the user types."""
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	out = _out(shell, "zlet tick_fast config period_ms: 77, duration_ms: 88")
	assert "duration_ms: 88" in out, out
	assert "period_ms: 77" in out, out


def test_omitted_field_is_zeroed_not_retained(dut: DeviceAdapter, shell: Shell):
	"""pb_tf_parse() zeroes the message before writing, so a field left
	out of the text is zero — not whatever the previous call set.

	tick's own validate_config() rejects a zero period, which makes the
	rejection the evidence: had period_ms been carried over from the call
	before, this would have succeeded. validate_config() runs before the
	`*cfg = *req` copy, so the instance also keeps its previous config."""
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	_out(shell, "zlet tick_fast config duration_ms: 123, period_ms: 456")

	out = _out(shell, "zlet tick_fast config duration_ms: 321")
	assert "config: -22" in out, out

	out = _out(shell, "zlet tick_fast get_config")
	assert "duration_ms: 123" in out, out
	assert "period_ms: 456" in out, out


def test_config_rejects_zero_period(dut: DeviceAdapter, shell: Shell):
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	out = _out(shell, "zlet tick_fast config duration_ms: 100, period_ms: 0")
	# tick_config_impl() returns -EINVAL for a zero period/duration; the
	# generated handler surfaces it via shell_error(sh, "%s: %d", ...).
	assert "config:" in out, out


def test_custom_rpc_kick(dut: DeviceAdapter, shell: Shell):
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	out = _out(shell, "zlet tick_fast kick")
	assert "is_running" in out, out
	assert "is_ready" in out, out


def test_config_with_no_message_is_rejected(dut: DeviceAdapter, shell: Shell):
	"""The raw argument is optional, so `argc` can be 1. The generated
	handler checks that before touching argv[1]."""
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	out = _out(shell, "zlet tick_fast config")
	assert "expected a text-format message" in out, out


def test_config_bare_value_is_a_syntax_error(dut: DeviceAdapter, shell: Shell):
	"""What used to be a valid positional call is now malformed text
	format, and must fail rather than be interpreted."""
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	out = _out(shell, "zlet tick_fast config 250 250")
	assert "config:" in out, out
	assert "duration_ms: 250" not in out, out
