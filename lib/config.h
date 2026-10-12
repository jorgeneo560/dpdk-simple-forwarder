#ifndef FLOWFWD_CONFIG_H
#define FLOWFWD_CONFIG_H

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

#define MAX_QUEUES 16

struct app_config {
	uint16_t nb_queues;
	uint16_t in_port;
	uint16_t out_port;
	uint16_t rss_pattern;
	uint16_t log_level;	// 0=error 1=warning 2=info 3=debug
	uint16_t log_console;	// 1 = also print log lines to the console
	uint16_t export_interval;	// seconds between flow statistics exports
	uint16_t pass_arp;	// 1 = forward ARP frames (untracked), 0 = drop them
	char path[PATH_MAX];	// file the settings were read from
};

/*
 * Loads "flowfwd.conf" from the directory of the running binary into cfg,
 * and records its path in cfg->path. Fills defaults first, then overrides them
 * from an INI-style file: "[section]" headers and "key = value" lines
 * ('#' starts a comment). Unknown sections/keys and out-of-range values are errors.
 * Returns 0 on success, -1 on failure (message printed to stderr).
 */
int config_load(struct app_config *cfg);

/*
 * Writes the path of a file called name in the same directory as the loaded
 * config file into buf. Returns 0 on success, -1 if it does not fit.
 */
int config_dir_file(const struct app_config *cfg, const char *name, char *buf, size_t len);

#endif
