"""Configuration-time checks of `(zephlet.requires)`.

Each scenario writes a few minimal zephlet modules, with their dependencies
declared in the proto or in the options file, and configures the fixture
app over them. Configuration must fail when a required type is not in the
build, when the types form a cycle of any length, or when a declaration is
invalid -- and must succeed otherwise, with a consumer's object compiling
on its own against its provider's generated header.
"""

import os
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

HERE = Path(__file__).parent
BOARD = os.environ.get("ZEPHLET_TEST_BOARD", "native_sim")


def _tool(name):
	path = shutil.which(name)
	if path is None:
		pytest.skip(f"{name} is not on PATH")
	return path


def _write_zephlet(root, type_name, requires=(), in_proto=False, extra_c=""):
	"""A minimal zephlet module `<root>/<type_name>` requiring @p requires,
	each `<type>` or `<name>=<type>`, in its options file or its proto, with
	@p extra_c appended to its source."""
	camel = type_name.capitalize()
	mod = root / type_name
	(mod / "zephyr").mkdir(parents=True)
	(mod / "zephyr" / "module.yml").write_text(
		f"name: zlet_{type_name}\nbuild:\n  cmake: .\n  kconfig: Kconfig\n")
	upper = type_name.upper()
	(mod / "Kconfig").write_text(
		f"config ZEPHLET_{upper}\n"
		f'    bool "{type_name} (dependency configuration fixture)"\n'
		"    default y\n"
		"    select NANOPB\n"
		"    help\n"
		"      Fixture zephlet of the dependency configuration test.\n\n"
		f"if ZEPHLET_{upper}\n\nmodule = ZEPHLET_{upper}\n"
		f"module-str = zlet_{type_name}\n"
		'source "subsys/logging/Kconfig.template.log_config"\n\nendif\n')
	(mod / "CMakeLists.txt").write_text(
		f"if(CONFIG_ZEPHLET_{upper})\n"
		f"    zephyr_zephlet_generate(TYPE {type_name} PREFIX zlet_{type_name}\n"
		f"                            SOURCES zlet_{type_name}.c)\n"
		"endif()\n")

	options = ""
	includes = ""
	for req in requires:
		name, _, dep = req.rpartition("=")
		if in_proto:
			named = f' name: "{name}"' if name else ""
			options += f'\n  option (zephlet.requires) = {{ type: "{dep}"{named} }};'
		includes += f'#include "zlet_{dep}_interface.h"\n'
	if requires and not in_proto:
		(mod / f"zlet_{type_name}.zephlet.options").write_text(
			f"{camel}Api " + " ".join(f"requires:{r}" for r in requires) + "\n")
	(mod / f"zlet_{type_name}.proto").write_text(
		'syntax = "proto3";\n\nimport "nanopb.proto";\nimport "zephlet.proto";\n'
		+ ('import "zephlet_options.proto";\n' if in_proto else "")
		+ "\noption (nanopb_fileopt).long_names = false;\n\n"
		f"message {camel} {{\n  message Config {{\n    uint32 x = 1;\n  }}\n\n"
		"  message Events {\n    uint32 y = 1;\n  }\n}\n\n"
		f"service {camel}Api {{{options}\n"
		"  rpc get_status(Empty) returns (Lifecycle.Status);\n}\n")
	(mod / f"zlet_{type_name}.c").write_text(
		f'#include "zlet_{type_name}_interface.h"\n' + includes + extra_c)
	return mod


def _build_object(build, source_name):
	"""Build only the object of @p source_name; return the ninja result."""
	targets = subprocess.run(["ninja", "-C", str(build), "-t", "targets", "all"],
				 capture_output=True, text=True)
	objs = [line.split(":", 1)[0] for line in targets.stdout.splitlines()
		if line.split(":", 1)[0].endswith(f"{source_name}.obj")]
	assert len(objs) == 1, objs
	# nanopb's protoc plugin starts through `env python3`; make that the
	# interpreter running this test, which has protobuf.
	env = dict(os.environ)
	env["PATH"] = os.path.dirname(sys.executable) + os.pathsep + env.get("PATH", "")
	return subprocess.run(["ninja", "-C", str(build), objs[0]],
			      capture_output=True, text=True, env=env)


def _configure(tmp_path, zephlets):
	"""Configure the fixture over @p zephlets, `{type: (requires, in_proto)}`."""
	if not os.environ.get("ZEPHYR_BASE"):
		pytest.skip("ZEPHYR_BASE is not set")
	cmake = _tool("cmake")
	_tool("ninja")
	root = tmp_path / "zephlets"
	mods = [str(_write_zephlet(root, t, *spec)) for t, spec in zephlets.items()]
	build = tmp_path / "build"
	return build, subprocess.run(
		# The interpreter running this test has Zephyr's Python packages; a
		# build dir outside the workspace may resolve another one.
		[cmake, "-GNinja", "-S", str(HERE), "-B", str(build), f"-DBOARD={BOARD}",
		 f"-DPython3_EXECUTABLE={sys.executable}",
		 f"-DDEPS_CONFIG_ROOT={root}", "-DDEPS_CONFIG_MODULES=" + ";".join(mods)],
		capture_output=True, text=True)


def test_acyclic_graph_configures_and_consumer_builds_alone(tmp_path):
	"""alpha requires beta twice (proto) and gamma (options file); the
	consumer's object, which includes both providers' generated headers,
	compiles on its own: generation is ordered ahead of it."""
	build, res = _configure(tmp_path, {
		"alpha": (("intake=beta", "exhaust=beta", "gamma"), True),
		"beta": (("gamma",), False),
		"gamma": ((), False),
	})
	assert res.returncode == 0, f"{res.stdout}\n{res.stderr}"

	res = _build_object(build, "zlet_alpha.c")
	assert res.returncode == 0, f"{res.stdout}\n{res.stderr}"
	header = (build / "modules" / "zlet_alpha" / "zlet_alpha_interface.h").read_text()
	assert "const struct zephlet *intake;" in header
	assert "const struct zephlet *gamma;" in header


@pytest.mark.parametrize("zephlets, needle", [
	({"alpha": (("beta",), False)},
	 "zephlet type 'alpha' requires 'beta', but no zephlet of type 'beta' is in "
	 "this build"),
	({"alpha": (("beta",), False), "beta": (("alpha",), True)},
	 "dependency cycle between zephlet types: alpha -> beta -> alpha"),
	({"alpha": (("beta",), False), "beta": (("gamma",), False),
	  "gamma": (("alpha",), True)},
	 "dependency cycle between zephlet types: alpha -> beta -> gamma -> alpha"),
	({"alpha": (("beta", "beta"), False), "beta": ((), False)},
	 "type 'beta' is required twice"),
	({"alpha": (("alpha",), True)}, "cannot require its own type"),
], ids=["missing", "cycle2", "cycle3", "duplicate", "self"])
def test_configuration_rejects(tmp_path, zephlets, needle):
	_, res = _configure(tmp_path, zephlets)
	assert res.returncode != 0, res.stdout
	# CMake wraps long messages; compare with whitespace collapsed.
	assert " ".join(needle.split()) in " ".join(res.stderr.split()), res.stderr


@pytest.mark.parametrize("extra_c, needle", [
	# A type with requires defined with the plain macro.
	("static struct alpha_config a_cfg;\n"
	 "ZEPHLET_NEW(alpha, a_inst, &a_cfg, NULL, NULL);\n",
	 "zephlet type 'alpha' declares (zephlet.requires): define 'a_inst' with "
	 "ZEPHLET_NEW_WITH_DEPS"),
	# A type without requires defined with the dependency macro.
	("static const struct alpha_dependencies d = {0};\n"
	 "ZEPHLET_NEW_WITH_DEPS(gamma, g_inst, NULL, NULL, &d, NULL);\n",
	 "zephlet type 'gamma' declares no (zephlet.requires): define 'g_inst' with "
	 "ZEPHLET_NEW"),
	# Another type's dependency record.
	("struct other { const struct zephlet *gamma; };\n"
	 "static const struct other d = {0};\n"
	 "ZEPHLET_NEW_WITH_DEPS(alpha, a_inst, NULL, NULL, &d, NULL);\n",
	 "_Generic"),
], ids=["new-on-deps-type", "with-deps-on-plain-type", "mistyped-record"])
def test_instance_macro_misuse_fails_to_build(tmp_path, extra_c, needle):
	build, res = _configure(tmp_path, {
		"alpha": (("gamma",), False, extra_c),
		"gamma": ((), False),
	})
	assert res.returncode == 0, f"{res.stdout}\n{res.stderr}"
	res = _build_object(build, "zlet_alpha.c")
	assert res.returncode != 0, res.stdout
	# GCC escapes the quotes of a static assertion's message.
	assert needle in (res.stdout + res.stderr).replace("\\'", "'"), res.stdout
