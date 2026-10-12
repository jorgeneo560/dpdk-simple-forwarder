#ifndef FLOWFWD_HUGEPAGES_H
#define FLOWFWD_HUGEPAGES_H

/*
 * Verifies free hugepages exist. Call before rte_eal_init().
 * Returns 0 on success, -1 on failure.
 */
int hugepages_check(void);

/*
 * Deletes leftover EAL hugepage files (rtemap_*) that no process is using.
 * Call after rte_eal_cleanup().
 */
void hugepages_cleanup(void);

#endif
