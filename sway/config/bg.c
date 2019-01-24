#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include "sway/config.h"
#include "log.h"
#include "list.h"

static pid_t swaybg_pid = -1;

void load_swaybg(void) {
	pid_t new_swaybg_pid = fork();

	if (new_swaybg_pid < 0) {
		sway_log(SWAY_ERROR, "Failed to fork swaybg");
	} else if (new_swaybg_pid == 0) {
		setsid();

		char *cmd = config->swaybg_command ? config->swaybg_command : "swaybg";

		size_t argv_cap = 2 + 8 * config->output_configs->length;
		size_t argv_len = 0;
		char **argv = calloc(argv_cap, sizeof(char*));
		argv[argv_len++] = cmd;

		for (int i = 0; i < config->output_configs->length; ++i) {
			struct output_config *oc = config->output_configs->items[i];

			argv[argv_len++] = "--output";
			argv[argv_len++] = oc->name;

			if (oc->background_option) {
				if (strcmp(oc->background_option, "solid_color") == 0) {
					argv[argv_len++] = "--color";
					argv[argv_len++] = oc->background;
					continue;
				}

				argv[argv_len++] = "--mode";
				argv[argv_len++] = oc->background_option;
			}

			if (oc->background) {
				argv[argv_len++] = "--image";
				argv[argv_len++] = oc->background;
			}

			if (oc->background_fallback) {
					argv[argv_len++] = "--color";
					argv[argv_len++] = oc->background_fallback;
			}
		}

		execvp(cmd, argv);

		sway_log(SWAY_ERROR, "Failed to exec swaybg");
		exit(EXIT_FAILURE);
	}

	if (swaybg_pid >= 0) {
		sway_log(SWAY_DEBUG, "Terminating swaybg %d", swaybg_pid);
		int ret = kill(-swaybg_pid, SIGTERM);
		if (ret != 0) {
			sway_log_errno(SWAY_ERROR, "Failed to terminate swaybg %d", swaybg_pid);
		} else {
			waitpid(swaybg_pid, NULL, 0);
		}
	}
	swaybg_pid = new_swaybg_pid;

	sway_log(SWAY_INFO, "Spawned swaybg %d", swaybg_pid);
}
