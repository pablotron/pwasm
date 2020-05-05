#include <stdlib.h> // size_t
#include <stdio.h> // fprintf()
#include <string.h> // strlen()
#include "cmds.h"

static const char *SET_NAMES[] = {
#define CMD_SET(a, b) b,
CMD_SETS
#undef CMD_SET
  "Unknown Set",
};

const char *cli_get_cmd_set_name(const cli_cmd_set_t set) {
  return SET_NAMES[(set < CLI_CMD_SET_LAST) ? set : CLI_CMD_SET_LAST];
}

// full list of commands
static const cli_cmd_t CMDS[] = {{
  .set  = CLI_CMD_SET_OTHER,
  .name = "help",
  .tip  = "Show help.",
  .help = "Show help.",
  .func = cmd_help,
}, {
  .set  = CLI_CMD_SET_OTHER,
  .name = "test",
  .tip  = "Run tests.",
  .help = "Run tests.",
  .func = cmd_test,
}, {
  .set  = CLI_CMD_SET_MOD,
  .name = "cat",
  .tip  = "Extract data for a custom section from a WASM file.",
  .help = "Extract data for a custom section from a WASM file.",
  .func = cmd_cat,
}, {
  .set  = CLI_CMD_SET_MOD,
  .name = "customs",
  .tip  = "List custom sections in a WASM file.",
  .help = "List custom sections in a WASM file.",
  .func = cmd_customs,
}, {
  .set  = CLI_CMD_SET_MOD,
  .name = "exports",
  .tip  = "List exports in a WASM file.",
  .help = "List exports in a WASM file.",
  .func = cmd_exports,
}, {
  .set  = CLI_CMD_SET_MOD,
  .name = "imports",
  .tip  = "List imports in a WASM file.",
  .help = "List imports in a WASM file.",
  .func = cmd_imports,
}, {
  .set  = CLI_CMD_SET_MOD,
  .name = "wat",
  .tip  = "Convert one or more WASM files to WAT files.",
  .help = "Convert one or more WASM files to WAT files.",
  .func = cmd_wat,
}};

const cli_cmd_t *cli_get_cmds(
  size_t * const ret_len
) {
  if (ret_len) {
    // return number of commands
    *ret_len = sizeof(CMDS) / sizeof(CMDS[0]);
  }

  // commands
  return CMDS;
}

// unknown command format string
static const char UNKNOWN[] = 
  "Unknown command: \"%s\".\n"
  "Use \"%s help\" for usage.\n";

/**
 * Unknown command.
 *
 * Returned by cli_find_cmd() if there is no matching command.
 */
static int cmd_unknown(
  const int argc,
  const char ** const argv
) {
  fprintf(stderr, UNKNOWN, (argc > 1) ? argv[1] : "", argv[0]);
  return -1;
}

/**
 * Find single matching command for given arguments.
 *
 * Returns a stub command with no name, tip, or help if no command
 * matches.
 */
cli_cmd_t cli_find_cmd(const char * const op) {
  // get op length
  const size_t op_len = strlen(op);

  // get commands
  size_t num_cmds;
  const cli_cmd_t * const cmds = cli_get_cmds(&num_cmds);

  for (size_t i = 0; i < num_cmds; i++) {
    // get command
    const cli_cmd_t cmd = cmds[i];

    // check name
    if (op_len && !strncmp(cmd.name, op, op_len + 1)) {
      // name matches, return command
      return cmd;
    }
  }

  // return unknown command
  return (cli_cmd_t) {
    .func = cmd_unknown,
  };
}
