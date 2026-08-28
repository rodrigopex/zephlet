"""RPC round-trip over the `zlet` shell command: decimal and `h"<hex>"`
argument forms, a custom (non-base) rpc, and the field-count-derived
`mandatory` argument gate.
"""

from __future__ import annotations

from twister_harness import DeviceAdapter, Shell


def _out(shell: Shell, cmd: str) -> str:
	return "\n".join(shell.get_filtered_output(shell.exec_command(cmd)))


def test_config_get_config_round_trip_decimal(dut: DeviceAdapter, shell: Shell):
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	out = _out(shell, "zlet tick_fast config 250 250")
	assert "duration_ms = 250" in out, out
	assert "period_ms = 250" in out, out

	out = _out(shell, "zlet tick_fast get_config")
	assert "duration_ms = 250" in out, out
	assert "period_ms = 250" in out, out


def test_config_round_trip_hex_form(dut: DeviceAdapter, shell: Shell):
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	# h"FA" == 250 decimal.
	out = _out(shell, 'zlet tick_fast config h"FA" h"FA"')
	assert "duration_ms = 250" in out, out
	assert "period_ms = 250" in out, out


def test_config_rejects_zero_period(dut: DeviceAdapter, shell: Shell):
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	out = _out(shell, "zlet tick_fast config 100 0")
	# tick_config_impl() returns -EINVAL for a zero period/duration; the
	# generated handler surfaces it via shell_error(sh, "%s: %d", ...).
	assert "config:" in out, out


def test_custom_rpc_kick(dut: DeviceAdapter, shell: Shell):
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	out = _out(shell, "zlet tick_fast kick")
	assert "is_running" in out, out
	assert "is_ready" in out, out


def test_config_missing_mandatory_argument_is_rejected(dut: DeviceAdapter, shell: Shell):
	"""`config` needs 2 args; Zephyr's own arg-count gate (SHELL_CMD_ARG's
	`mandatory`, computed from TICK_CONFIG_FIELDLIST's 2 fields) rejects
	a call with only 1, before our handler ever runs."""
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	out = _out(shell, "zlet tick_fast config 250")
	assert "duration_ms = 250" not in out, out
