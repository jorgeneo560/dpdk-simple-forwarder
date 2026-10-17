#ifndef FLOWFWD_HUGEPAGES_H
#define FLOWFWD_HUGEPAGES_H

#include <stdbool.h>

/*
 * Verifies free hugepages exist. Skipped when the EAL options contain
 * "--no-huge". Call before rte_eal_init().
 * Returns true on success, false on failure.
 */
bool hugepages_check(int argc, char **argv);

/*
 * Deletes leftover EAL hugepage files (rtemap_*) that no process is using.
 * Call after rte_eal_cleanup().
 */
void hugepages_cleanup(void);

#endif
