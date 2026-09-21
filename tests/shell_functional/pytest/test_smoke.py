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
	# The response prints twice: one field per line, then the same message
	# compact. Both are asserted, because asserting only the joined string
	# would pass on the compact line's strength alone and so would not
	# notice the block disappearing.
	assert any(ln.strip() == "is_running: false" for ln in out.splitlines()), out
	assert any(ln.strip() == "is_ready: true" for ln in out.splitlines()), out
	assert "is_running: false, is_ready: true" in out, out

	# No RPC accepts Lifecycle.Status as a request, so the compact line
	# carries no command prefix. The echoed command starts with `zlet`
	# too, so this looks for a prefix carrying the message with it.
	assert not any(ln.strip().startswith("zlet ") and "is_running" in ln
		       for ln in out.splitlines()), out

	# Compact emits no trailing newline of its own, so the frontend adds
	# one. Without it the response runs into the next prompt and this line
	# comes back with the echoed command glued to it.
	assert not out.rstrip("\r\n").endswith("$"), out
