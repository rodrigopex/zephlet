"""Smoke test: the `zlet` shell root command boots and dispatches a
base-lifecycle RPC end to end through a real ZLET_SHELL_INSTANCE()/
ZLET_SHELL_DEFINE()-generated command tree.
"""

from __future__ import annotations

from twister_harness import DeviceAdapter, Shell


def test_get_status_round_trip(dut: DeviceAdapter, shell: Shell):
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	lines = shell.exec_command("zlet tick_fast get_status")
	out = "\n".join(shell.get_filtered_output(lines))

	assert "is_running = false" in out, out
	assert "is_ready = true" in out, out
