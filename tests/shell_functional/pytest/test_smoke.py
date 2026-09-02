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
	# Both assertions hold under either print style: multi-line puts each
	# field on its own line, compact joins them with ", ", and either way
	# these are substrings. This suite runs in both configurations.
	assert "is_running: false" in out, out
	assert "is_ready: true" in out, out

	# The output must be terminated whatever the style. Compact emits no
	# trailing newline of its own, so the frontend adds one; without it the
	# response runs into the next prompt and this line comes back with the
	# echoed command glued to it.
	assert not out.rstrip("\r\n").endswith("$"), out
