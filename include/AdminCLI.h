#ifndef ADMIN_CLI_H
#define ADMIN_CLI_H

#include <stdbool.h>

bool admin_cli_stdin_is_tty(void);
int admin_cli_run(void);

#endif
