"""Smoke test: the `zlet` shell root command boots and dispatches a
base-lifecycle RPC end to end through a real ZLET_SHELL_INSTANCE()-
generated command tree, printing its response as protobuf text format.
"""

from __future__ import annotations

from twister_harness import DeviceAdapter, Shell


def test_get_status_round_trip(dut: DeviceAdapter, shell: Shell):
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	lines = shell.exec_command("zlet tick_fast get_status")
	out = "\n".join(shell.get_filtered_output(lines))

	# Lifecycle.Status lives in the shared zephlet.proto, whose descriptor
	# the infra defines by hand in zephlet_textformat.c rather than through
	# codegen — so this also covers that one descriptor.
	#
	# The frontend prints compact, so both fields land on one line joined
	# by ", ". Asserted as one string rather than two substrings, so a
	# regression to multi-line would fail here rather than pass silently.
	assert "is_running: false, is_ready: true" in out, out

	# Compact emits no trailing newline of its own, so the frontend adds
	# one. Without it the response runs into the next prompt and this line
	# comes back with the echoed command glued to it.
	assert not out.rstrip("\r\n").endswith("$"), out
