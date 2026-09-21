"""The two response forms, and the `zlet_fmt` command that selects them.

A response prints twice by default: an indented block to read, then the
compact form prefixed with the command that writes it back. Both are valid
text format, but only the compact one can be pasted into the shell, which
submits a command on every newline — so neither form alone serves both
reading and editing, which is why the default prints both.

The command prefix is resolved by codegen, matching a response's type
against every RPC's request type. Both outcomes are covered here:
`Tick.Config`, which `config` accepts, and `Lifecycle.Status`, which no RPC
accepts and which therefore prints unprefixed.
"""

from __future__ import annotations

from twister_harness import DeviceAdapter, Shell


def _out(shell: Shell, cmd: str) -> str:
	return "\n".join(shell.get_filtered_output(shell.exec_command(cmd)))


def _fmt(shell: Shell, form: str) -> str:
	return _out(shell, f"zlet_fmt {form}")


def _paste_lines(out: str, instance: str) -> list[str]:
	"""Lines that are a printed command rather than message text.

	The echoed command also starts with `zlet`, so a prefix is only
	recognised when it carries a field of the message with it.
	"""
	return [ln.strip() for ln in out.splitlines()
		if ln.strip().startswith(f"zlet {instance} ") and ": " in ln]


def test_both_forms_printed_by_default(dut: DeviceAdapter, shell: Shell):
	"""Asserting both forms is what pins 'additive': a regression to
	either one alone leaves one of these assertions unmatched, where
	asserting only the compact string would pass on its strength alone."""
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	_fmt(shell, "both")
	_out(shell, "zlet tick_fast config duration_ms: 250, period_ms: 25")
	out = _out(shell, "zlet tick_fast get_config")

	# The block: one field per line, so each stands alone on its line.
	assert any(ln.strip() == "duration_ms: 250" for ln in out.splitlines()), out
	assert any(ln.strip() == "period_ms: 25" for ln in out.splitlines()), out

	# The pasteable line: same message, compact, behind the RPC that
	# writes it. `config` is what codegen resolved for Tick.Config.
	assert "zlet tick_fast config duration_ms: 250, period_ms: 25" in out, out


def test_paste_line_is_accepted_verbatim(dut: DeviceAdapter, shell: Shell):
	"""The whole point of the prefix: what is printed is a command. Take
	it out of the response unedited and run it."""
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	_fmt(shell, "both")
	_out(shell, "zlet tick_fast config duration_ms: 300, period_ms: 30")
	printed = _paste_lines(_out(shell, "zlet tick_fast get_config"), "tick_fast")
	assert printed, "no command line printed under the response"

	again = _out(shell, printed[0])
	for bad in ("syntax error", "no such field", "wrong value type"):
		assert bad not in again, again
	assert "duration_ms: 300" in again, again


def test_response_with_no_writer_prints_unprefixed(dut: DeviceAdapter, shell: Shell):
	"""Nothing accepts Lifecycle.Status as a request, so get_status has no
	command to name. Its compact line must still print: skipping it
	instead of unprefixing it would make the compact-only form emit
	nothing at all for this RPC."""
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	_fmt(shell, "both")
	out = _out(shell, "zlet tick_fast get_status")
	assert "is_ready: true" in out, out
	assert _paste_lines(out, "tick_fast") == [], out

	_fmt(shell, "compact")
	out = _out(shell, "zlet tick_fast get_status")
	assert "is_ready: true" in out, out
	assert _paste_lines(out, "tick_fast") == [], out


def test_fmt_selects_one_form_or_both(dut: DeviceAdapter, shell: Shell):
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	_out(shell, "zlet tick_fast config duration_ms: 400, period_ms: 40")

	_fmt(shell, "pretty")
	out = _out(shell, "zlet tick_fast get_config")
	assert any(ln.strip() == "duration_ms: 400" for ln in out.splitlines()), out
	assert "duration_ms: 400, period_ms: 40" not in out, out

	_fmt(shell, "compact")
	out = _out(shell, "zlet tick_fast get_config")
	assert "zlet tick_fast config duration_ms: 400, period_ms: 40" in out, out
	assert not any(ln.strip() == "duration_ms: 400" for ln in out.splitlines()), out

	_fmt(shell, "both")
	out = _out(shell, "zlet tick_fast get_config")
	assert any(ln.strip() == "duration_ms: 400" for ln in out.splitlines()), out
	assert "zlet tick_fast config duration_ms: 400, period_ms: 40" in out, out


def test_fmt_reports_current_form_and_rejects_unknown(dut: DeviceAdapter, shell: Shell):
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	_fmt(shell, "pretty")
	assert "pretty" in _out(shell, "zlet_fmt"), "bare zlet_fmt must report the form"

	_fmt(shell, "both")
	assert "both" in _out(shell, "zlet_fmt")

	# An unknown form must not silently leave the previous one in place.
	assert "expected one of" in _fmt(shell, "bogus")
	assert "both" in _out(shell, "zlet_fmt")
