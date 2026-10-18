#ifndef FLOWFWD_HUGEPAGES_H
#define FLOWFWD_HUGEPAGES_H

#include <stdbool.h>

/**
 * @brief Verifies that free hugepages exist.
 *
 * Skipped when the EAL options contain "--no-huge". Call before
 * rte_eal_init().
 *
 * @param argc Argument count of main().
 * @param argv Arguments of main(), searched for "--no-huge".
 * @return true on success, false on failure.
 */
bool hugepages_check(int argc, char **argv);

/**
 * @brief Deletes leftover EAL hugepage files (rtemap_*) that no process uses.
 *
 * Call after rte_eal_cleanup().
 */
void hugepages_cleanup(void);

#endif
