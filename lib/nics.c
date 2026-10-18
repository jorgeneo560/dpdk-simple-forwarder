#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "nics.h"

#define PCI_SYSFS "/sys/bus/pci/devices"
#define PCI_CLASS_NET 0x02
#define MAX_NICS 64

struct nic {
  char addr[16];
  uint16_t vendor;
  uint16_t device;
  char drv[64];
  bool bound; // driver is one DPDK can use
  int port;   // predicted DPDK port id, -1 if DPDK will not use it
};

static const char *const dpdk_drivers[] = {"vfio-pci", "uio_pci_generic",
                                           "igb_uio"};

static bool read_attr(const char *dev, const char *attr, char *buf,
                      size_t len) {
  char path[PATH_MAX];
  FILE *f;

  snprintf(path, sizeof(path), PCI_SYSFS "/%s/%s", dev, attr);
  f = fopen(path, "r");
  if (f == NULL)
    return false;
  if (fgets(buf, (int)len, f) == NULL) {
    fclose(f);
    return false;
  }
  fclose(f);
  buf[strcspn(buf, "\n")] = '\0';
  return true;
}

static bool read_driver(const char *dev, char *buf, size_t len) {
  char path[PATH_MAX], target[PATH_MAX];
  ssize_t n;
  const char *base;

  snprintf(path, sizeof(path), PCI_SYSFS "/%s/driver", dev);
  n = readlink(path, target, sizeof(target) - 1);
  if (n < 0)
    return false;
  target[n] = '\0';
  base = strrchr(target, '/');
  snprintf(buf, len, "%.*s", (int)len - 1, base ? base + 1 : target);
  return true;
}

static bool is_dpdk_driver(const char *name) {//ok
  for (size_t i = 0; i < sizeof(dpdk_drivers) / sizeof(dpdk_drivers[0]); i++) {
    if (strcmp(name, dpdk_drivers[i]) == 0) {
      return true;
    }
  }
  return false;
}

static int cmp_addr(const void *a, const void *b) { //ok
  return strcmp(((const struct nic *)a)->addr, ((const struct nic *)b)->addr);
}

// Returns the number of network devices found, or -1 if the PCI bus cannot be
// read.
static int scan_nics(struct nic *nics) { //ok
  int n = 0;
  struct dirent *de;
  DIR *dir = opendir(PCI_SYSFS);

  if (dir == NULL) {
    fprintf(stderr, "cannot read %s: %s\n", PCI_SYSFS, strerror(errno));
    return -1;
  }

  while (n < MAX_NICS && (de = readdir(dir)) != NULL) {
    char cls[32], vendor[32], device[32];
    struct nic *nic = &nics[n];

    if (de->d_name[0] == '.')
      continue;
    if (!read_attr(de->d_name, "class", cls, sizeof(cls)) ||
        (strtoul(cls, NULL, 16) >> 16) != PCI_CLASS_NET)
      continue;
    if (!read_attr(de->d_name, "vendor", vendor, sizeof(vendor)) ||
        !read_attr(de->d_name, "device", device, sizeof(device)))
      continue;

    memset(nic, 0, sizeof(*nic));
    snprintf(nic->addr, sizeof(nic->addr), "%.15s", de->d_name);
    nic->vendor = (uint16_t)strtoul(vendor, NULL, 16);
    nic->device = (uint16_t)strtoul(device, NULL, 16);
    if (!read_driver(de->d_name, nic->drv, sizeof(nic->drv)))
      snprintf(nic->drv, sizeof(nic->drv), "none");
    nic->bound = is_dpdk_driver(nic->drv);
    nic->port = -1;
    n++;
  }
  closedir(dir);
  return n;
}

static void print_nic(const struct nic *nic, const struct app_config *cfg) { //ok
  fprintf(stderr, "  %s [%04x:%04x] driver %-15s", nic->addr, nic->vendor,
          nic->device, nic->drv);
  if (nic->port >= 0)
    fprintf(stderr, " port %d%s", nic->port,
            nic->port == cfg->in_port    ? ": INPUT"
            : nic->port == cfg->out_port ? ": OUTPUT"
                                         : "");
  fprintf(stderr, "\n");
}

static void print_bind_help(unsigned int ready, unsigned int min_ports) {
  fprintf(stderr,
          "found %u network device(s) bound to a DPDK driver, need %u.\n",
          ready, min_ports);
  fprintf(stderr, "bind two with: sudo dpdk-devbind.py -b "
                  "<vfio-pci|uio_pci_generic> <PCI address>\n"
                  "or use virtual devices (--vdev=...) for testing.\n");
}

bool nics_check(int argc, char **argv, unsigned int min_ports, //ok
                const struct app_config *cfg) {
  struct nic nics[MAX_NICS];
  unsigned int ready = 0;
  int nb_nics;

  for (int i = 1; i < argc && strcmp(argv[i], "--") != 0; i++) {
    if (strncmp(argv[i], "--vdev", 6) == 0 ||
        strcmp(argv[i], "--no-pci") == 0) {
      return true;
    }
  }

  nb_nics = scan_nics(nics);
  if (nb_nics < 0){
    fprintf(stderr, "failed to scan network devices\n");
    return false;
  }

  //readdir returns the directory entries in no guaranteed order.
  qsort(nics, (size_t)nb_nics, sizeof(nics[0]), cmp_addr);

  // DPDK probes devices in PCI address order and numbers its ports in that
  // order.
  for (int i = 0; i < nb_nics; i++) {
    if (nics[i].bound) {
      nics[i].port = (int)ready++;
    }
  }

  fprintf(stderr,
          "PCI network devices (DPDK port ids follow PCI address order):\n");
  for (int i = 0; i < nb_nics; i++) {
    print_nic(&nics[i], cfg);
  }

  if (ready < min_ports) {
    print_bind_help(ready, min_ports);
    return false;
  }
  return true;
}
