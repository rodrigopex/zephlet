"""No cross-talk between shell command handlers: two instances of the
same type (tick_fast/tick_slow) and two different types (tick/ui) each
carry their own generated handler set, closing over their own literal
instance — never a shared dispatcher keyed by argv.
"""

from __future__ import annotations

from twister_harness import DeviceAdapter, Shell


def _out(shell: Shell, cmd: str) -> str:
	return "\n".join(shell.get_filtered_output(shell.exec_command(cmd)))


def test_two_tick_instances_keep_independent_config(dut: DeviceAdapter, shell: Shell):
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	_out(shell, "zlet tick_fast config duration_ms: 111, period_ms: 111")
	_out(shell, "zlet tick_slow config duration_ms: 222, period_ms: 222")

	fast_out = _out(shell, "zlet tick_fast get_config")
	assert "duration_ms: 111" in fast_out, fast_out
	assert "period_ms: 111" in fast_out, fast_out

	slow_out = _out(shell, "zlet tick_slow get_config")
	assert "duration_ms: 222" in slow_out, slow_out
	assert "period_ms: 222" in slow_out, slow_out


def test_tick_and_ui_types_keep_independent_config(dut: DeviceAdapter, shell: Shell):
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	_out(shell, "zlet tick_fast config duration_ms: 333, period_ms: 333")
	_out(shell, "zlet ui_main config blink_period_ms: 44")

	tick_out = _out(shell, "zlet tick_fast get_config")
	assert "duration_ms: 333" in tick_out, tick_out

	ui_out = _out(shell, "zlet ui_main get_config")
	assert "blink_period_ms: 44" in ui_out, ui_out
	# ui's config has one field, not tick's two — confirms ui_main's
	# handler used ui_config_t_tf, not a leaked tick descriptor.
	assert "duration_ms" not in ui_out, ui_out


def test_field_name_from_another_type_is_rejected(dut: DeviceAdapter, shell: Shell):
	"""Each handler parses against its own message descriptor, so a field
	that exists on tick's Config is unknown to ui's."""
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	out = _out(shell, "zlet ui_main config duration_ms: 5")
	assert "no such field" in out, out
