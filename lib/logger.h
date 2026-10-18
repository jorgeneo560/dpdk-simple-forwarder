#ifndef FLOWFWD_LOGGER_H
#define FLOWFWD_LOGGER_H

#include <stdbool.h>

/**
 * @brief Starts the logger thread.
 *
 * The thread writes log output to a file (appended) and, if console is set,
 * also to stderr. All RTE_LOG output is redirected to it, so callers never
 * block on file I/O. Call after rte_eal_init().
 *
 * @param path    Log file.
 * @param level   Level for the FLOWFWD log type: 0=error, 1=warning, 2=info,
 *                3=debug.
 * @param console true to also print to stderr.
 * @return true on success, false on failure.
 */
bool logger_start(const char *path, unsigned int level, bool console);

/**
 * @brief Writes out pending messages, stops the thread and closes the file.
 *
 * Safe to call twice.
 */
void logger_stop(void);

#endif
