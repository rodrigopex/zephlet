"""How the shell presents a message: the two response forms, the `zlet_fmt`
command that selects them, and the request template in each RPC's help.

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


# ----- the help template ----------------------------------------------


def test_help_lists_the_request_as_a_template(dut: DeviceAdapter, shell: Shell):
	"""Listing an instance shows each RPC's request spelled as text format
	with `<type>` placeholders, so the line is a template to fill in rather
	than the name of a C struct."""
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	out = _out(shell, "zlet tick_fast")

	assert "duration_ms: <uint32>, period_ms: <uint32>" in out, out
	# The old form named the struct and nothing else.
	assert "<text-format" not in out, out


def test_help_shows_the_response_when_there_is_no_request(dut: DeviceAdapter,
							  shell: Shell):
	"""`get_status` takes nothing, so naming its request was useless. It
	describes its response instead -- and Lifecycle.Status comes from the
	shared zephlet.proto, so this covers codegen resolving a type declared
	outside the per-zephlet file."""
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	out = _out(shell, "zlet tick_fast")

	assert "-> is_running: <bool>, is_ready: <bool>" in out, out
	assert "-> duration_ms: <uint32>, period_ms: <uint32>" in out, out


def test_help_template_round_trips_when_filled_in(dut: DeviceAdapter, shell: Shell):
	"""The point of the placeholders: substitute them and the line is a
	working command. Done here by hand for `config`, which is what a reader
	does with it."""
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	assert "duration_ms: <uint32>, period_ms: <uint32>" in _out(shell, "zlet tick_fast")

	_fmt(shell, "pretty")
	out = _out(shell, "zlet tick_fast config duration_ms: 700, period_ms: 70")
	for bad in ("syntax error", "no such field", "wrong value type"):
		assert bad not in out, out
	assert any(ln.strip() == "duration_ms: 700" for ln in out.splitlines()), out


# ----- the copyable template on failure --------------------------------

_TICK_TMPL = "zlet tick_fast config duration_ms: <uint32>, period_ms: <uint32>"


def test_missing_request_prints_a_copyable_template(dut: DeviceAdapter, shell: Shell):
	"""Asking for nothing is the clearest way of asking what to type, so
	the shape is printed there.

	It carries the `zlet <instance> <rpc>` prefix, so it has the same shape
	as the pasteable line a successful response prints -- placeholders where
	that one has values -- and nothing needs retyping.

	The template must stand alone on its line: with the label on the same
	line, selecting the line would copy the label too, which is the whole
	failure mode this replaced."""
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	out = _out(shell, "zlet tick_fast config")

	assert "expected a text-format message" in out, out
	assert "Tip:" in out, out
	assert any(ln.strip() == _TICK_TMPL for ln in out.splitlines()), out


def test_malformed_request_prints_the_template_too(dut: DeviceAdapter, shell: Shell):
	"""A typo is when the shape is most wanted, so the error carries it."""
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	out = _out(shell, "zlet tick_fast config duration_ms: x")

	assert "syntax error" in out, out
	assert any(ln.strip() == _TICK_TMPL for ln in out.splitlines()), out


def test_the_printed_template_parses_once_filled_in(dut: DeviceAdapter, shell: Shell):
	"""Pins the advice the tip gives: take the printed line, substitute the
	placeholders, and it must run. Built here by doing exactly that to the
	line the tip emits, so the two cannot drift apart -- if the template
	ever stopped being a valid command, the tip would be telling people to
	do something broken."""
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	printed = next(ln.strip() for ln in _out(shell, "zlet tick_fast config").splitlines()
		       if ln.strip() == _TICK_TMPL)
	filled = printed.replace("<uint32>", "800", 1).replace("<uint32>", "80", 1)

	_fmt(shell, "pretty")
	out = _out(shell, filled)

	for bad in ("syntax error", "no such field", "wrong value type"):
		assert bad not in out, out
	assert any(ln.strip() == "duration_ms: 800" for ln in out.splitlines()), out
