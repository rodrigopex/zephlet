"""Regression test for generated-header build ordering.

A plain `zephyr_library` that includes a generated zephlet interface
header must be compilable on its own. Asking ninja for that one object
is what makes the check deterministic: nothing else in the graph can
pull the generators in as a side effect, so the object builds only if
`zephyr_zephlet_generate()` genuinely ordered them ahead of every
Zephyr library.

A full build cannot test this -- whether the compile loses the race
depends on core count, ninja -j and the ccache hit rate, which is what
made the original failure show up on some build machines only.

Expected failures without the fix:

  * no ordering at all      -> zlet_tick_interface.h: No such file
  * codegen ordered, not    -> tick/zlet_tick.pb.h: No such file
    nanopb
"""

import os
import shutil
import subprocess
from pathlib import Path

import pytest

HERE = Path(__file__).parent
BOARD = os.environ.get("ZEPHLET_TEST_BOARD", "native_sim")


def _tool(name):
    path = shutil.which(name)
    if path is None:
        pytest.skip(f"{name} is not on PATH")
    return path


@pytest.fixture(scope="module")
def build_dir(tmp_path_factory):
    """Configure the fixture app under ninja, without building it."""
    if not os.environ.get("ZEPHYR_BASE"):
        pytest.skip("ZEPHYR_BASE is not set")

    cmake = _tool("cmake")
    _tool("ninja")
    out = tmp_path_factory.mktemp("codegen_order")

    res = subprocess.run(
        [cmake, "-GNinja", "-S", str(HERE), "-B", str(out),
         f"-DBOARD={BOARD}"],
        capture_output=True,
        text=True,
    )
    assert res.returncode == 0, (
        f"cmake configure failed:\n{res.stdout}\n{res.stderr}"
    )
    return out


def _consumer_object(build_dir):
    """Find the consumer object -- Zephyr mangles library directories."""
    res = subprocess.run(
        ["ninja", "-C", str(build_dir), "-t", "targets", "all"],
        capture_output=True,
        text=True,
    )
    assert res.returncode == 0, res.stderr

    hits = [
        line.split(":", 1)[0]
        for line in res.stdout.splitlines()
        if "consumer.c.obj" in line
    ]
    assert len(hits) == 1, f"expected exactly one consumer object: {hits}"
    return hits[0]


def test_consumer_object_builds_alone(build_dir):
    """Build only the consumer object; both generators must precede it."""
    obj = _consumer_object(build_dir)

    res = subprocess.run(
        ["ninja", "-C", str(build_dir), obj],
        capture_output=True,
        text=True,
    )
    assert res.returncode == 0, (
        "compiling the consumer object on its own failed, so a generated "
        f"header was not ordered before it:\n{res.stdout}\n{res.stderr}"
    )

    # Both generators ran as part of that single-object build.
    interface = build_dir / "modules" / "zlet_tick" / "zlet_tick_interface.h"
    nanopb = build_dir / "tick" / "zlet_tick.pb.h"
    assert interface.is_file(), f"{interface} was not generated"
    assert nanopb.is_file(), f"{nanopb} was not generated"
