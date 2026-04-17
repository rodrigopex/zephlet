"""West extension commands for zephlet development tooling."""

from west.commands import WestCommand
from west import log
import subprocess
import sys
from pathlib import Path


class Zephlet(WestCommand):
    """Command class for zephlet development tools."""

    def __init__(self):
        super().__init__(
            'zephlet',
            'zephlet development tools',
            'Tooling for creating and managing zephlets and adapters')

    def do_add_parser(self, parser_adder):
        """Add argument parser for zephlet command and subcommands."""
        parser = parser_adder.add_parser(
            self.name,
            help=self.help,
            description=self.description)

        subparsers = parser.add_subparsers(
            dest='command',
            help='zephlet subcommands')

        # new subcommand
        new_parser = subparsers.add_parser(
            'new',
            help='create a new zephlet',
            description='Create a new zephlet using the copier template')
        new_parser.add_argument(
            '-n', '--name',
            help='zephlet name (non-interactive mode)')
        new_parser.add_argument(
            '-d', '--description',
            help='zephlet description (non-interactive mode)')
        new_parser.add_argument(
            '-a', '--author',
            help='author name (non-interactive mode)')

        # new-adapter subcommand
        adapter_parser = subparsers.add_parser(
            'new-adapter',
            help='create a new adapter',
            description='Create a new adapter between two zephlets (interactive only)')

        # gen subcommand
        gen_parser = subparsers.add_parser(
            'gen',
            help='regenerate zephlet interface files',
            description='Regenerate interface files for an existing zephlet')
        gen_parser.add_argument(
            'zephlet_name',
            help='name of the zephlet to regenerate')

        return parser

    def do_run(self, args, unknown):
        """Execute the requested subcommand."""
        if not args.command:
            log.die('no subcommand specified; try "west zephlet -h"')

        if args.command == 'new':
            self._new_zephlet(args)
        elif args.command == 'new-adapter':
            self._new_adapter(args)
        elif args.command == 'gen':
            self._gen_files(args)

    def _get_workspace_paths(self):
        """Determine workspace paths based on workspace and module locations."""
        # Module directory: this file is in <module>/west/
        shared_dir = Path(__file__).parent.parent

        # Manifest repo root (where west.yml lives)
        workspace_root = Path(self.manifest.repo_abspath)

        def _resolve(p):
            """Resolve a path against workspace_root if relative."""
            p = Path(p)
            return p if p.is_absolute() else workspace_root / p

        # Check west config for custom zephlets path
        try:
            from west.configuration import config
            zephlets_dir = config.get('zephlet', 'zephlets-dir', fallback=None)
            if zephlets_dir:
                zephlets_dir = _resolve(zephlets_dir)
            else:
                zephlets_dir = workspace_root / 'src' / 'zephlets'
                if not zephlets_dir.exists():
                    zephlets_dir = workspace_root / 'zephlets'
        except:
            zephlets_dir = workspace_root / 'src' / 'zephlets'
            if not zephlets_dir.exists():
                zephlets_dir = workspace_root / 'zephlets'

        # Check west config for custom adapters path
        try:
            from west.configuration import config
            adapters_dir = config.get('zephlet', 'adapters-dir', fallback=None)
            if adapters_dir:
                adapters_dir = _resolve(adapters_dir)
            else:
                adapters_dir = workspace_root / 'src' / 'adapters'
                if not adapters_dir.exists():
                    adapters_dir = workspace_root / 'adapters'
        except:
            adapters_dir = workspace_root / 'src' / 'adapters'
            if not adapters_dir.exists():
                adapters_dir = workspace_root / 'adapters'

        return {
            'workspace_root': workspace_root,
            'zephlets_dir': zephlets_dir,
            'shared_dir': shared_dir,
            'codegen_dir': shared_dir / 'codegen',
            'template_dir': shared_dir / 'codegen' / 'zephyr_zephlet_template',
            'adapters_dir': adapters_dir,
            'build_dir': workspace_root / 'build'
        }

    def _ensure_adapters_dir(self, adapters_dir):
        """Create adapters directory structure with base files if missing."""
        if adapters_dir.exists():
            return False

        log.inf(f'Creating adapters directory: {adapters_dir}')

        # Create directory structure
        src_dir = adapters_dir / 'src'
        zephyr_dir = adapters_dir / 'zephyr'
        src_dir.mkdir(parents=True, exist_ok=True)
        zephyr_dir.mkdir(parents=True, exist_ok=True)

        # Create base_adapter.c
        base_adapter_content = """#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(adapter, CONFIG_ADAPTERS_LOG_LEVEL);
"""
        (src_dir / 'base_adapter.c').write_text(base_adapter_content)

        # Create CMakeLists.txt
        cmake_content = """if(CONFIG_ADAPTERS)
    zephyr_library()
    zephyr_library_sources("src/base_adapter.c")

endif()
"""
        (adapters_dir / 'CMakeLists.txt').write_text(cmake_content)

        # Create Kconfig
        kconfig_content = """config ADAPTERS
    bool "Zephlet adapters"
    default y
    select ZBUS
    select ZBUS_ASYNC_LISTENER
    help
      Zephlet adapter infrastructure

if ADAPTERS

module = ADAPTERS
module-str = adapter
source "subsys/logging/Kconfig.template.log_config"

endif
"""
        (adapters_dir / 'Kconfig').write_text(kconfig_content)

        # Create zephyr/module.yml
        module_yml_content = """name: adapters
build:
  cmake: .
  kconfig: Kconfig
"""
        (zephyr_dir / 'module.yml').write_text(module_yml_content)

        log.inf(f'Adapters directory created with base files')
        log.inf(f'NOTE: Add "{adapters_dir.relative_to(adapters_dir.parent.parent)}" to root CMakeLists.txt EXTRA_ZEPHYR_MODULES')

        return True

    def _check_dependencies(self, required_packages):
        """Check if required Python packages are installed."""
        missing = []
        for package in required_packages:
            try:
                __import__(package.replace('-', '_'))
            except ImportError:
                missing.append(package)

        if missing:
            log.die(f'missing required Python packages: {", ".join(missing)}\n'
                   f'Install with: pip install {" ".join(missing)}')

    def _new_zephlet(self, args):
        """Create a new zephlet using copier template."""
        self._check_dependencies(['copier'])

        paths = self._get_workspace_paths()

        if not paths['zephlets_dir'].exists():
            log.die(f'zephlets directory not found: {paths["zephlets_dir"]}\n'
                    f'Create it with: mkdir -p {paths["zephlets_dir"]}\n'
                    f'Or configure custom path: west config zephlet.zephlets-dir <path>')

        if not paths['template_dir'].exists():
            log.die(f'template directory not found: {paths["template_dir"]}')

        cmd = ['copier', 'copy', str(paths['template_dir']), str(paths['zephlets_dir'])]

        # Non-interactive mode if name is provided
        if args.name:
            cmd.extend(['--data', f'zephlet_name={args.name}'])
            if args.description:
                cmd.extend(['--data', f'description={args.description}'])
            if args.author:
                cmd.extend(['--data', f'author_name={args.author}'])
            cmd.append('--defaults')

        log.inf(f'Creating new zephlet...')
        result = subprocess.run(cmd, cwd=paths['workspace_root'])

        if result.returncode != 0:
            log.die('failed to create zephlet')

        if args.name:
            zephlet_dir = paths['zephlets_dir'] / args.name
            rel = zephlet_dir.relative_to(paths['workspace_root'])
            log.inf(f'Zephlet "{args.name}" created successfully')
            log.inf(f'Next steps:')
            log.inf(f'  1. Edit {rel}/zlet_{args.name}.proto')
            log.inf(f'  2. Add to root CMakeLists.txt EXTRA_ZEPHYR_MODULES')
            log.inf(f'  3. Enable CONFIG_ZEPHLET_{args.name.upper()}=y in prj.conf')
            log.inf(f'  4. Compile the project (west build -b <board>)')

    def _new_adapter(self, args):
        """Create a new adapter between two zephlets."""
        self._check_dependencies(['proto-schema-parser', 'jinja2'])

        paths = self._get_workspace_paths()

        # Check zephlets directory exists
        if not paths['zephlets_dir'].exists():
            log.die(f'zephlets directory not found: {paths["zephlets_dir"]}\n'
                    f'Create zephlets first using: west zephlet new\n'
                    f'Or configure custom path: west config zephlet.zephlets-dir <path>')

        # Ensure adapters directory exists (auto-create if needed)
        if not paths['adapters_dir'].exists():
            self._ensure_adapters_dir(paths['adapters_dir'])
            log.inf('')  # Blank line for readability

        script_path = paths['codegen_dir'] / 'generate_adapter.py'

        if not script_path.exists():
            log.die(f'generate_adapter.py not found: {script_path}')

        cmd = [sys.executable, str(script_path)]
        cmd.extend(['--zephlets-path', str(paths['zephlets_dir'])])
        cmd.extend(['--output-dir', str(paths['adapters_dir'])])

        # Pass generated protos path so adapter generator can find Invoke/Report
        # Try multiple build dir locations
        for build_candidate in [
            paths['build_dir'] / 'modules',
            Path(paths['zephlets_dir']).parent.parent / 'build' / 'modules',
        ]:
            if build_candidate.exists():
                cmd.extend(['--generated-protos-path', str(build_candidate)])
                break

        log.inf('Creating new adapter...')
        result = subprocess.run(cmd, cwd=paths['workspace_root'])

        if result.returncode != 0:
            log.die('failed to create adapter')

    def _gen_files(self, args):
        """Regenerate interface files for an existing zephlet."""
        self._check_dependencies(['proto-schema-parser', 'jinja2'])

        paths = self._get_workspace_paths()
        script_path = paths['codegen_dir'] / 'generate_zephlet.py'

        if not script_path.exists():
            log.die(f'generate_zephlet.py not found: {script_path}')

        # Validate build directory exists
        zephlet_build_dir = paths['build_dir'] / 'modules' / f'{args.zephlet_name}_zephlet'
        if not zephlet_build_dir.exists():
            log.die(f'build directory not found: {zephlet_build_dir}\n'
                   f'Run "west build" first to create the build directory')

        # Locate proto file
        zephlet_source_dir = paths['zephlets_dir'] / args.zephlet_name
        proto_file = zephlet_source_dir / f'zlet_{args.zephlet_name}.proto'

        if not proto_file.exists():
            log.die(f'proto file not found: {proto_file}')

        cmd = [sys.executable, str(script_path)]
        cmd.extend(['--proto', str(proto_file)])
        cmd.extend(['--output-dir', str(zephlet_build_dir)])
        cmd.extend(['--zephlet-name', args.zephlet_name])
        cmd.extend(['--module-dir', str(zephlet_source_dir)])
        cmd.append('--no-generate-impl')

        log.inf(f'Regenerating interface files for "{args.zephlet_name}"...')
        result = subprocess.run(cmd, cwd=paths['workspace_root'])

        if result.returncode != 0:
            log.die('failed to regenerate files')

        log.inf(f'Interface files regenerated successfully')
        log.inf(f'Files updated in: {zephlet_build_dir}')
