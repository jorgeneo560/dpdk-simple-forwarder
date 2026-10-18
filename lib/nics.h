#ifndef FLOWFWD_NICS_H
#define FLOWFWD_NICS_H

#include "config.h"

/**
 * @brief Lists the PCI network devices and checks they can be used.
 *
 * Marks which device is the input and the output port according to cfg, and
 * verifies that at least min_ports devices are bound to a driver DPDK can use
 * (vfio-pci, uio_pci_generic, igb_uio). Skipped when the EAL options contain
 * "--vdev" or "--no-pci". Call before rte_eal_init().
 *
 * @param argc      Argument count of main().
 * @param argv      Arguments of main(), searched for "--vdev" and "--no-pci".
 * @param min_ports Number of usable devices required.
 * @param cfg       Configuration with the input and output port ids.
 * @return true on success, false on failure.
 */
bool nics_check(int argc, char **argv, unsigned int min_ports,
                const struct app_config *cfg);

#endif
