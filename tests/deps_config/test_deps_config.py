"""Build-graph checks of `(zephlet.requires)`.

Each scenario writes a few minimal zephlet modules, with their dependencies
declared in the proto or in the options file, and configures the fixture
app over them. The dependency graph check is a build step ordered ahead of
every compile: building any one object must fail when a required type is
not in the build, when the types form a cycle of any length, or when a
declaration is invalid -- and must succeed otherwise, with a consumer's
object compiling on its own against its provider's generated header.
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
	# The graph check ran as part of that one-object build.
	assert (build / "zephlet_dependency_graph.txt").read_text() == \
		"alpha=beta,gamma\nbeta=gamma\ngamma=\n"
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
def test_build_rejects(tmp_path, zephlets, needle):
	"""Configuration succeeds; the first object built stops on the check."""
	build, res = _configure(tmp_path, zephlets)
	assert res.returncode == 0, f"{res.stdout}\n{res.stderr}"
	res = _build_object(build, "zlet_alpha.c")
	assert res.returncode != 0, res.stdout
	assert needle in res.stdout + res.stderr, res.stdout
	assert not (build / "zephlet_dependency_graph.txt").exists()


def test_proto_edit_reruns_check_not_configure(tmp_path):
	"""Adding a requires to a proto re-runs the graph check at build time,
	without reconfiguring."""
	build, res = _configure(tmp_path, {"alpha": ((), False), "beta": ((), False)})
	assert res.returncode == 0, f"{res.stdout}\n{res.stderr}"
	assert _build_object(build, "zlet_alpha.c").returncode == 0

	opts = tmp_path / "zephlets" / "alpha" / "zlet_alpha.zephlet.options"
	opts.write_text("AlphaApi requires:gamma\n")
	# A new options file is picked up by the CONFIGURE_DEPENDS glob, which
	# reconfigures; editing one that exists must not.
	res = _build_object(build, "zlet_alpha.c")
	assert res.returncode != 0
	assert "requires 'gamma', but no zephlet of type 'gamma'" in res.stdout

	proto = tmp_path / "zephlets" / "beta" / "zlet_beta.proto"
	proto.write_text(proto.read_text().replace(
		"service BetaApi {", 'service BetaApi {\n  option (zephlet.requires) = { type: "alpha" };')
		.replace('import "zephlet.proto";', 'import "zephlet.proto";\nimport "zephlet_options.proto";'))
	opts.write_text("AlphaApi requires:beta\n")
	res = _build_object(build, "zlet_alpha.c")
	assert res.returncode != 0
	assert "Re-running CMake" not in res.stdout, "a proto edit reconfigured"
	assert "dependency cycle between zephlet types: alpha -> beta -> alpha" in res.stdout


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
	# Init sub-priorities that are not a bare decimal literal 0-9.
	("ZEPHLET_NEW_PRIO(gamma, g_inst, NULL, NULL, NULL, 10);\n",
	 "zephlet 'g_inst': init sub-priority must be a decimal literal 0-9"),
	("enum { x = 3 };\nZEPHLET_NEW_PRIO(gamma, g_inst, NULL, NULL, NULL, x);\n", "Z_UTIL_INC_x"),
	("ZEPHLET_NEW_PRIO(gamma, g_inst, NULL, NULL, NULL, 1+1);\n",
	 "zephlet 'g_inst': init sub-priority must be a decimal literal 0-9"),
	("ZEPHLET_NEW_PRIO(gamma, g_inst, NULL, NULL, NULL, (2));\n", 'pasting "Z_UTIL_INC_"'),
], ids=["new-on-deps-type", "with-deps-on-plain-type", "mistyped-record", "sub-priority-10",
	"sub-priority-identifier", "sub-priority-expression", "sub-priority-parenthesized"])
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


def test_init_priorities_follow_the_graph_without_reconfiguring(tmp_path):
	"""zephlet_init_prio.h holds base + rank per type. A proto edit that keeps
	every rank leaves it untouched; one that changes a rank rewrites it. Neither
	re-runs CMake."""
	build, res = _configure(tmp_path, {
		"alpha": (("beta",), False), "beta": ((), False), "gamma": ((), False),
	})
	assert res.returncode == 0, f"{res.stdout}\n{res.stderr}"
	assert _build_object(build, "zlet_alpha.c").returncode == 0
	header = build / "zephlet_init_prio.h"
	text = header.read_text()
	assert "#define _ZLET_INIT_PRIO_alpha 11\n" in text
	assert "#define _ZLET_INIT_PRIO_beta 10\n" in text
	os.utime(header, (1, 1))

	opts = tmp_path / "zephlets" / "alpha" / "zlet_alpha.zephlet.options"
	opts.write_text("AlphaApi requires:gamma\n")
	res = _build_object(build, "zlet_alpha.c")
	assert res.returncode == 0, res.stdout
	assert header.stat().st_mtime == 1, "same ranks: header not rewritten"

	proto = tmp_path / "zephlets" / "gamma" / "zlet_gamma.proto"
	proto.write_text(proto.read_text().replace(
		"service GammaApi {", 'service GammaApi {\n  option (zephlet.requires) = { type: "beta" };')
		.replace('import "zephlet.proto";', 'import "zephlet.proto";\nimport "zephlet_options.proto";'))
	res = _build_object(build, "zlet_alpha.c")
	assert res.returncode == 0, res.stdout
	assert "Re-running CMake" not in res.stdout, "a proto edit reconfigured"
	text = header.read_text()
	assert "#define _ZLET_INIT_PRIO_gamma 11\n" in text
	assert "#define _ZLET_INIT_PRIO_alpha 12\n" in text


def _build_all(build):
	"""Build and link the whole fixture; return the ninja result."""
	env = dict(os.environ)
	env["PATH"] = os.path.dirname(sys.executable) + os.pathsep + env.get("PATH", "")
	return subprocess.run(["ninja", "-C", str(build)], capture_output=True, text=True, env=env)


# Instances for the link check: alpha requires gamma; beta is another type.
_INSTANCES = (
	'#include "zlet_beta_interface.h"\n'
	"ZEPHLET_NEW(gamma, g_inst, NULL, NULL, NULL);\n"
	"ZEPHLET_NEW(beta, b_inst, NULL, NULL, NULL);\n")

# Hand-written two-slot records: a.next -> b.next -> a, and a.extra unbound.
# Each defines its runtime record and init entry (ZEPHLET_INIT_DEFINE), as a
# hand-written record must: the entry is what references the zephlet
# sections, and a native target's final link drops sections nothing
# references, so the link check would see no instances.
_HAND_BAD_SLOT_ON_CYCLE = (
	"struct duo_dependencies { const struct zephlet *next; const struct zephlet *extra; };\n"
	"extern const struct zephlet_api duo_api;\n"
	"static const struct zephlet_dep_slot duo_slots[] = {\n"
	'\t{.name = "next", .type = "duo", .api = &duo_api,\n'
	"\t .offset = offsetof(struct duo_dependencies, next)},\n"
	'\t{.name = "extra", .type = "duo", .api = &duo_api,\n'
	"\t .offset = offsetof(struct duo_dependencies, extra)},\n"
	"};\n"
	"const struct zephlet_api duo_api = {.deps = duo_slots, .num_deps = 2};\n"
	"#define DUO(_n, _next, _extra) \\\n"
	"\tstatic const struct duo_dependencies _n##_deps = {.next = (_next), .extra = (_extra)}; \\\n"
	"\tZEPHLET_INIT_DEFINE(_n, 20, 0); \\\n"
	"\tconst STRUCT_SECTION_ITERABLE(zephlet, _n) = { \\\n"
	"\t\t.name = #_n, .api = &duo_api, .dependencies = &_n##_deps, \\\n"
	"\t\t.runtime = ZEPHLET_RUNTIME(_n)}\n"
	"extern const struct zephlet cyc_a, cyc_b;\n"
	"DUO(cyc_a, &cyc_b, NULL);\nDUO(cyc_b, &cyc_a, &cyc_a);\n")


def _alpha_bound_to(binding):
	return (_INSTANCES
		+ f"static const struct alpha_dependencies a_deps = {{{binding}}};\n"
		+ "ZEPHLET_NEW_WITH_DEPS(alpha, a_inst, NULL, NULL, &a_deps, NULL);\n")


def _configure_link_fixture(tmp_path, extra_c):
	build, res = _configure(tmp_path, {
		"alpha": (("gamma",), False, extra_c),
		"gamma": ((), False),
		"beta": ((), False),
	})
	assert res.returncode == 0, f"{res.stdout}\n{res.stderr}"
	return build


def test_link_check_accepts_valid_bindings(tmp_path):
	"""A ZEPHLET_DEP binding links, and the check ran on the ELF."""
	build = _configure_link_fixture(tmp_path, _alpha_bound_to(".gamma = ZEPHLET_DEP(gamma, g_inst)"))
	res = _build_all(build)
	assert res.returncode == 0, f"{res.stdout}\n{res.stderr}"
	assert "check_deps_elf.py" in (build / "build.ninja").read_text()


@pytest.mark.parametrize("extra_c, needle", [
	(_alpha_bound_to("0"),
	 "error: zephlet: 'a_inst': dependency 'gamma' (a gamma) is not bound"),
	(_alpha_bound_to(".gamma = &b_inst"),
	 "error: zephlet: 'a_inst': dependency 'gamma' (a gamma) is bound to 'b_inst', "
	 "of another type"),
	# A zeroed, non-const record lands in .bss: read as all NULL.
	(_INSTANCES
	 + "static struct alpha_dependencies a_deps;\n"
	 + "ZEPHLET_NEW_WITH_DEPS(alpha, a_inst, NULL, NULL, &a_deps, NULL);\n",
	 "error: zephlet: 'a_inst': dependency 'gamma' (a gamma) is not bound"),
], ids=["unbound", "other-type", "bss-record"])
def test_link_check_rejects(tmp_path, extra_c, needle):
	"""Every object compiles; the post-link check fails the build."""
	build = _configure_link_fixture(tmp_path, extra_c)
	res = _build_all(build)
	assert res.returncode != 0, res.stdout
	assert needle in res.stdout + res.stderr, res.stdout


def test_link_check_reports_a_bad_slot_of_a_hand_written_record(tmp_path):
	"""A hand-written record's unbound slot fails the build. The cycle beside
	it is not reported: it needs a cycle of types, which only hand-written
	apis can form, and those instances are skipped at boot."""
	build = _configure_link_fixture(tmp_path, _HAND_BAD_SLOT_ON_CYCLE)
	res = _build_all(build)
	out = res.stdout + res.stderr
	assert res.returncode != 0, res.stdout
	assert "error: zephlet: 'cyc_a': dependency 'extra' (a duo) is not bound" in out, out
	assert "cycle" not in out, out


def test_dep_of_another_type_fails_to_compile(tmp_path):
	"""ZEPHLET_DEP() refuses an instance of another type at compile time."""
	build = _configure_link_fixture(tmp_path, _alpha_bound_to(".gamma = ZEPHLET_DEP(gamma, b_inst)"))
	res = _build_object(build, "zlet_alpha.c")
	assert res.returncode != 0, res.stdout
	out = res.stdout + res.stderr
	assert "_Generic" in out and "beta_zlet_tag" in out, out
