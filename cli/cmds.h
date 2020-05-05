#ifndef CLI_CMDS_H
#define CLI_CMDS_H

#define CMD_SETS \
  CMD_SET(MOD, "Module") \
  CMD_SET(OTHER, "Other")

typedef enum {
#define CMD_SET(a, b) CLI_CMD_SET_ ## a,
CMD_SETS
#undef CMD_SET
  CLI_CMD_SET_LAST,
} cli_cmd_set_t;

const char *cli_get_cmd_set_name(const cli_cmd_set_t);

typedef struct {
  const cli_cmd_set_t set;
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
int cmd_customs(const int argc, const char **);
int cmd_cat(const int argc, const char **);
int cmd_exports(const int argc, const char **);
int cmd_imports(const int argc, const char **);

#endif /* CLI_CMDS_H */
