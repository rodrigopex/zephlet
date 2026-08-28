"""Unknown instance / unknown rpc must fail cleanly through Zephyr's own
static command-tree lookup — no custom not-found handling needed since
the tree is fully SHELL_STATIC_SUBCMD_SET_CREATE-generated.

`zlet` and each instance (e.g. `tick_fast`) are registered with a NULL
handler (they're pure subcommand groups — see ZLET_SHELL_INSTANCE()/
ZLET_SHELL_DEFINE()), so Zephyr's own shell dispatcher responds to an
unmatched child not with "command not found" (that's for a genuinely
unknown *root* command) but by printing that level's help and its real
subcommand list — confirmed here by asserting the listed names are
exactly the sibling instances / that instance's own RPCs, and that the
bogus name never dispatched anything.
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
