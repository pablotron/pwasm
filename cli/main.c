#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cli.h"

int main(int argc, char *argv[]) {
  // get action
  const char * const op = (argc > 1) ? argv[1] : "help";

  // find and run command
  return cli_find_cmd(op).func(argc, (const char **) argv);
}
