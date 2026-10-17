#ifndef FLOWFWD_NICS_H
#define FLOWFWD_NICS_H

#include "config.h"

/*
 * Lists the PCI network devices, and marks which one is the input and the
 * output port according to cfg. Verifies at least min_ports devices are bound
 * to a driver DPDK can use (vfio-pci,
 * uio_pci_generic, igb_uio). Skipped when the EAL options contain "--vdev" or
 * "--no-pci". Call before rte_eal_init(). Returns true on success, false on
 * failure.
 */
bool nics_check(int argc, char **argv, unsigned int min_ports,
                const struct app_config *cfg);

#endif
