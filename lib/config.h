#ifndef FLOWFWD_CONFIG_H
#define FLOWFWD_CONFIG_H

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Most queues (and workers) the application supports. */
#define MAX_QUEUES 16

/** Settings of the application, read from flowfwd.conf. */
struct app_config {
  unsigned int nb_queues;       ///< RX/TX queues per port, one worker each
  uint16_t in_port;             ///< DPDK port id of the input interface
  uint16_t out_port;            ///< DPDK port id of the output interface
  uint16_t rss_pattern;         ///< 16-bit pattern repeated to form the RSS key
  unsigned int log_level;       ///< 0=error 1=warning 2=info 3=debug
  unsigned int log_console;     ///< 1 = also print log lines to the console
  char log_file[PATH_MAX];      ///< file the log is written to
  unsigned int export_interval; ///< seconds between flow statistics exports
  char export_dir[PATH_MAX];    ///< directory of the flow statistics CSV files
  unsigned int pass_arp;        ///< 1 = forward ARP frames (untracked), 0 = drop
  unsigned int flow_timeout;    ///< seconds without packets before removal
  unsigned int cleanup_interval; ///< seconds between runs of the flow cleaner
  unsigned int table_size;       ///< flows each worker's table can hold
  uint8_t forward_dst_mac[6];    ///< destination MAC for input -> output, zero =
                                 ///< unchanged
  uint8_t reverse_dst_mac[6];    ///< the same for output -> input
  char path[PATH_MAX];           ///< file the settings were read from
};

/**
 * @brief Loads "flowfwd.conf" from the directory of the running binary.
 *
 * Fills the defaults first, then overrides them from an INI-style file:
 * "[section]" headers and "key = value" lines ('#' starts a comment). Unknown
 * sections or keys and out-of-range values are errors.
 *
 * @param cfg Receives the settings and the path of the file.
 * @return true on success, false on failure (message printed to stderr).
 */
bool config_load(struct app_config *cfg);

#endif
