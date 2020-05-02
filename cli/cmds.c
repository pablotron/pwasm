#include <stdlib.h> // size_t
#include <stdio.h> // fprintf()
#include <string.h> // strlen()
#include "cmds.h"

// full list of commands
static const cli_cmd_t CMDS[] = {{
  .name = "help",
  .tip  = "Show help.",
  .help = "Show help.",
  .func = cmd_help,
}, {
  .name = "test",
  .tip  = "Run test.",
  .help = "Show help.",
  .func = cmd_test,
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
    if (!strncmp(cmd.name, op, op_len)) {
      // name matches, return command
      return cmd;
    }
  }

  // return unknown command
  return (cli_cmd_t) {
    .func = cmd_unknown,
  };
}
