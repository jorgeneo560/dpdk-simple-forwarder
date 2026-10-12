#ifndef FLOWFWD_PMD_IDS_H
#define FLOWFWD_PMD_IDS_H

#include <stddef.h>
#include <stdint.h>

/*
 * Looks up a PCI vendor:device in the ID tables embedded in the installed DPDK
 * PMD libraries. On a match copies the PMD name (e.g. "net_ixgbe") into name.
 * Returns 1 if found, 0 if no PMD lists the device, -1 if no PMD libraries
 * could be located (compatibility unknown).
 */
int pmd_ids_lookup(uint16_t vendor, uint16_t device, char *name, size_t len);

#endif
