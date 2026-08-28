#include <zephyr/shell/shell.h>

/**
 * @file
 * @brief The `zlet` shell root command.
 *
 * Defined exactly once here, unconditionally (compiled whenever
 * CONFIG_ZEPHLETS_SHELL=y). Each instance adds itself as a child under
 * this root via ZLET_SHELL_INSTANCE()'s `SHELL_SUBCMD_ADD((zlet), ...)`
 * call — invoked automatically by ZEPHLET_NEW(...) through the
 * `_ZLET_SHELL_HOOK_<type>` hook, from however many translation units
 * declare zephlet instances. No manifest of instance names is needed;
 * this mirrors the in-tree `kernel`/`thread` shell commands
 * (zephyr/subsys/shell/modules/kernel_service/).
 */
SHELL_SUBCMD_SET_CREATE(zlet_shell_root_cmds, (zlet));
SHELL_CMD_REGISTER(zlet, &zlet_shell_root_cmds, "Invoke a zephlet instance's RPC.", NULL);
