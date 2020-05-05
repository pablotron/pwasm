#include <stdlib.h> // size_t
#include <stdio.h> // printf()
#include "../cmds.h" // cli_cmd_t, cli_get_cmds()

static const char USAGE_HEADER[] =
  "Usage:\n"
  "  %s <command> [args]\n";

static const char USAGE_FOOTER[] =
  "\n"
  "Use \"help <command>\" for more details on a specific command.\n";

static int show(const int argc, const char **argv) {
  // get command
  const char * const op = (argc > 2) ? argv[2] : "";
  const cli_cmd_t cmd = cli_find_cmd(op);

  if (cmd.help) {
    // print command help
    fputs(cmd.help, stdout);
    fputc('\n', stdout);
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
  printf(USAGE_HEADER, argv[0]);

  // print command sets
  for (size_t i = 0; i < CLI_CMD_SET_LAST; i++) {
    printf("\n%s Commands:\n", cli_get_cmd_set_name(i));

    // print commands in set
    for (size_t j = 0; j < num_cmds; j++) {
      if (cmds[j].set == i) {
        // print command summary
        printf("  %s: %s\n", cmds[j].name, cmds[j].tip);
      }
    }
  }

  // print usage footer
  fputs(USAGE_FOOTER, stdout);

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
