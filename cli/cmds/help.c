#include <stdlib.h> // size_t
#include <stdio.h> // printf()
#include "../cmds.h" // cli_cmd_t, cli_get_cmds()

static const char USAGE[] =
  "Usage: %s <command> [args]\n"
  "\n"
  "Commands:\n";

static int show(const int argc, const char **argv) {
  // get command
  const char * const op = (argc > 2) ? argv[1] : "";
  const cli_cmd_t cmd = cli_find_cmd(op);

  if (cmd.help) {
    // print command help
    printf(cmd.help, argv[0]);
    return 0;
  } else {
    // print error
    fprintf(stderr, "Unknown command: %s", argv[1]);
    return -1;
  }
}

static int list(const int argc, const char **argv) {
  (void) argc;

  // get commands
  size_t num_cmds;
  const cli_cmd_t * const cmds = cli_get_cmds(&num_cmds);

  // print usage header and then summary of each command
  printf(USAGE, argv[0]);
  for (size_t i = 0; i < num_cmds; i++) {
    // print command summary
    printf("%s: %s\n", cmds[i].name, cmds[i].tip);
  }

  return 0;
}

int cmd_help(
  const int argc,
  const char ** argv
) {
  if (argc > 2) {
    // show help for a single command
    return show(argc, argv);
  } else {
    // list commands
    return list(argc, argv);
  }
}
