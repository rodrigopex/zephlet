"""Init ranks and the per-type SYS_INIT priority header of the graph step."""

import importlib.util
import os
import subprocess
import sys
from pathlib import Path

import pytest

_SCRIPT = Path(__file__).parents[2] / "codegen" / "generate_zephlet.py"
_SPEC = importlib.util.spec_from_file_location("generate_zephlet", _SCRIPT)
gen = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(gen)


def test_rank_is_one_above_the_highest_provider():
	graph = {"ctl": ["climate", "fan"], "climate": ["temperature", "fan"],
		 "fan": [], "temperature": []}
	assert gen.init_ranks(graph) == {"fan": 0, "temperature": 0, "climate": 1, "ctl": 2}


def test_diamond_takes_the_longest_path():
	graph = {"top": ["left", "right"], "left": ["base"], "right": ["mid"],
		 "mid": ["base"], "base": []}
	assert gen.init_ranks(graph) == {"base": 0, "left": 1, "mid": 1, "right": 2, "top": 3}


def test_header_holds_base_plus_rank(tmp_path):
	h = tmp_path / "zephlet_init_prio.h"
	gen.write_init_prio_header(str(h), {"climate": ["fan"], "fan": []}, 10)
	text = h.read_text()
	assert "#define _ZLET_INIT_PRIO_climate 11\n" in text
	assert "#define _ZLET_INIT_PRIO_fan 10\n" in text


def test_header_untouched_when_ranks_do_not_change(tmp_path):
	h = tmp_path / "zephlet_init_prio.h"
	gen.write_init_prio_header(str(h), {"climate": ["fan"], "fan": []}, 10)
	os.utime(h, (1, 1))
	gen.write_init_prio_header(str(h), {"climate": ["fan"], "fan": []}, 10)
	assert h.stat().st_mtime == 1, "same ranks: not rewritten"
	gen.write_init_prio_header(str(h), {"climate": [], "fan": []}, 10)
	assert h.stat().st_mtime != 1, "a rank changed: rewritten"
	assert "#define _ZLET_INIT_PRIO_climate 10\n" in h.read_text()


def test_priority_past_three_digits_is_an_error(tmp_path):
	h = tmp_path / "zephlet_init_prio.h"
	res = subprocess.run(
		[sys.executable, "-c",
		 "import importlib.util,sys;"
		 f"s=importlib.util.spec_from_file_location('g',{str(_SCRIPT)!r});"
		 "g=importlib.util.module_from_spec(s);s.loader.exec_module(g);"
		 f"g.write_init_prio_header({str(h)!r}, {{'a': ['b'], 'b': []}}, 999)"],
		capture_output=True, text=True)
	assert res.returncode != 0
	assert "init priorities reach 1000, above 999" in res.stderr
	assert not h.exists()
