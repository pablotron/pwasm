#ifndef CLI_CMDS_H
#define CLI_CMDS_H

typedef struct {
  const char * const name;
  const char * const tip;
  const char * const help;
  int (*func)(const int, const char **);
} cli_cmd_t;

// find matching command
cli_cmd_t cli_find_cmd(const char *);

// get pointer to all commands
const cli_cmd_t *cli_get_cmds(size_t *);

// commands
int cmd_help(const int argc, const char **);
int cmd_test(const int argc, const char **);
int cmd_wat(const int argc, const char **);
int cmd_list_custom(const int argc, const char **);
int cmd_cat_custom(const int argc, const char **);
int cmd_exports(const int argc, const char **);

#endif /* CLI_CMDS_H */
