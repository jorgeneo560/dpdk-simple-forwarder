#ifndef FLOWFWD_LOGGER_H
#define FLOWFWD_LOGGER_H

#include <stdbool.h>

/*
 * Starts a background thread that writes log output to the file at path
 * (appended) and, if console is set, also to stderr. All RTE_LOG output is
 * redirected to it, so callers never block on file I/O.
 * level applies to the FLOWFWD log type: 0=error, 1=warning, 2=info, 3=debug.
 * Call after rte_eal_init(). Returns true on success, false on failure.
 */
bool logger_start(const char *path, unsigned int level, bool console);

/* Writes out pending messages, stops the thread and closes the file. Safe to
 * call twice. */
void logger_stop(void);

#endif
