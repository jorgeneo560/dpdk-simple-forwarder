#ifndef FLOWFWD_CONFIG_H
#define FLOWFWD_CONFIG_H

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MAX_QUEUES 16

struct app_config {
  unsigned int nb_queues;
  uint16_t in_port;
  uint16_t out_port;
  uint16_t rss_pattern;         // 16-bit pattern repeated to form the RSS key
  unsigned int log_level;       // 0=error 1=warning 2=info 3=debug
  unsigned int log_console;     // 1 = also print log lines to the console
  unsigned int export_interval; // seconds between flow statistics exports
  unsigned int pass_arp; // 1 = forward ARP frames (untracked), 0 = drop them
  unsigned int flow_timeout; // seconds without packets before a flow is removed
  unsigned int
      cleanup_interval;       // seconds between runs of the flow table cleaner
  unsigned int table_size;    // flows each worker's table can hold
  uint8_t forward_dst_mac[6]; // destination MAC for IP packets input -> output,
                              // all zero = unchanged
  uint8_t reverse_dst_mac[6]; // the same for output -> input
  char path[PATH_MAX];        // file the settings were read from
};

/*
 * Loads "flowfwd.conf" from the directory of the running binary into cfg,
 * and records its path in cfg->path. Fills defaults first, then overrides them
 * from an INI-style file: "[section]" headers and "key = value" lines
 * ('#' starts a comment). Unknown sections/keys and out-of-range values are
 * errors. Returns true on success, false on failure (message printed to
 * stderr).
 */
bool config_load(struct app_config *cfg);

/*
 * Writes the path of a file called name in the same directory as the loaded
 * config file into buf. Returns true on success, false if it does not fit.
 */
bool config_dir_file(const struct app_config *cfg, const char *name, char *buf,
                     size_t len);

#endif
