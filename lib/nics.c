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
#include "pmd_ids.h"

#define PCI_SYSFS       "/sys/bus/pci/devices"
#define PCI_CLASS_NET   0x02
#define MAX_NICS        64

#define COLOR_GREEN  "\033[32m"
#define COLOR_YELLOW "\033[33m"
#define COLOR_RED    "\033[31m"
#define COLOR_RESET  "\033[0m"

enum compat { COMPAT_FULL, COMPAT_PARTIAL, COMPAT_NONE, COMPAT_UNKNOWN };

struct nic {
	char addr[16];
	uint16_t vendor;
	uint16_t device;
	char drv[64];
	char pmd[32];
	enum compat compat;
	bool bound;
	int port;	// predicted DPDK port id, -1 if DPDK will not use it
};

static const char *const dpdk_drivers[] = { "vfio-pci", "uio_pci_generic", "igb_uio" };

// PMDs that exist but are experimental or feature-limited.
static const char *const limited_pmds[] = { "net_r8169" };

static enum compat
classify(uint16_t vendor, uint16_t device, char *pmd, size_t len)
{
	int found = pmd_ids_lookup(vendor, device, pmd, len);

	if (found < 0)
		return COMPAT_UNKNOWN;
	if (found == 0)
		return COMPAT_NONE;
	for (size_t i = 0; i < sizeof(limited_pmds) / sizeof(limited_pmds[0]); i++)
		if (strcmp(pmd, limited_pmds[i]) == 0)
			return COMPAT_PARTIAL;
	return COMPAT_FULL;
}

static const char *
compat_label(enum compat c, const char **color)
{
	switch (c) {
	case COMPAT_FULL:
		*color = COLOR_GREEN;
		return "compatible";
	case COMPAT_PARTIAL:
		*color = COLOR_YELLOW;
		return "partially compatible";
	case COMPAT_NONE:
		*color = COLOR_RED;
		return "not compatible";
	default:
		*color = "";
		return "compatibility unknown";
	}
}

static int
read_attr(const char *dev, const char *attr, char *buf, size_t len)
{
	char path[PATH_MAX];
	FILE *f;

	snprintf(path, sizeof(path), PCI_SYSFS "/%s/%s", dev, attr);
	f = fopen(path, "r");
	if (f == NULL)
		return -1;
	if (fgets(buf, (int)len, f) == NULL) {
		fclose(f);
		return -1;
	}
	fclose(f);
	buf[strcspn(buf, "\n")] = '\0';
	return 0;
}

static int
read_driver(const char *dev, char *buf, size_t len)
{
	char path[PATH_MAX], target[PATH_MAX];
	ssize_t n;
	const char *base;

	snprintf(path, sizeof(path), PCI_SYSFS "/%s/driver", dev);
	n = readlink(path, target, sizeof(target) - 1);
	if (n < 0)
		return -1;
	target[n] = '\0';
	base = strrchr(target, '/');
	snprintf(buf, len, "%.*s", (int)len - 1, base ? base + 1 : target);
	return 0;
}

static bool
is_dpdk_driver(const char *name)
{
	for (size_t i = 0; i < sizeof(dpdk_drivers) / sizeof(dpdk_drivers[0]); i++)
		if (strcmp(name, dpdk_drivers[i]) == 0)
			return true;
	return false;
}

static int
cmp_addr(const void *a, const void *b)
{
	return strcmp(((const struct nic *)a)->addr, ((const struct nic *)b)->addr);
}

// Returns the number of network devices found, or -1 if the PCI bus cannot be read.
static int
scan_nics(struct nic *nics)
{
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
		if (read_attr(de->d_name, "class", cls, sizeof(cls)) != 0 ||
		    (strtoul(cls, NULL, 16) >> 16) != PCI_CLASS_NET)
			continue;
		if (read_attr(de->d_name, "vendor", vendor, sizeof(vendor)) != 0 ||
		    read_attr(de->d_name, "device", device, sizeof(device)) != 0)
			continue;

		memset(nic, 0, sizeof(*nic));
		snprintf(nic->addr, sizeof(nic->addr), "%.15s", de->d_name);
		nic->vendor = (uint16_t)strtoul(vendor, NULL, 16);
		nic->device = (uint16_t)strtoul(device, NULL, 16);
		if (read_driver(de->d_name, nic->drv, sizeof(nic->drv)) != 0)
			snprintf(nic->drv, sizeof(nic->drv), "none");
		nic->compat = classify(nic->vendor, nic->device, nic->pmd, sizeof(nic->pmd));
		nic->bound = is_dpdk_driver(nic->drv);
		nic->port = -1;
		n++;
	}
	closedir(dir);
	return n;
}

static void
print_nic(const struct nic *nic, const struct app_config *cfg, bool use_color)
{
	const char *color;
	const char *label = compat_label(nic->compat, &color);
	char role[48] = "";
	char tail[96];

	if (nic->port >= 0)
		snprintf(role, sizeof(role), ", port %d%s", nic->port,
			 nic->port == cfg->in_port ? ": INPUT" :
			 nic->port == cfg->out_port ? ": OUTPUT" : "");
	snprintf(tail, sizeof(tail), "%s%s", nic->bound ? ", bound to DPDK" : ", not bound", role);

	fprintf(stderr, "  %s [%04x:%04x] driver %-15s %s%s%s%s%s%s%s\n",
		nic->addr, nic->vendor, nic->device, nic->drv,
		use_color ? color : "", label, use_color ? COLOR_RESET : "",
		nic->pmd[0] ? " (" : "", nic->pmd, nic->pmd[0] ? ")" : "", tail);
}

static void
print_bind_help(unsigned int ready, unsigned int min_ports,
		const struct nic *nics, int nb_nics)
{
	unsigned int nb_candidates = 0;

	fprintf(stderr, "found %u usable DPDK network device(s), need %u.\n", ready, min_ports);

	for (int i = 0; i < nb_nics; i++) {
		const struct nic *n = &nics[i];

		if (n->bound || (n->compat != COMPAT_FULL && n->compat != COMPAT_PARTIAL))
			continue;
		if (nb_candidates++ == 0)
			fprintf(stderr, "compatible devices not yet bound to DPDK:\n");
		fprintf(stderr, "  %s\n", n->addr);
	}

	if (nb_candidates == 0) {
		fprintf(stderr, "no compatible devices found.\n");
	} else {
		fprintf(stderr, "bind them with:\n"
			"  sudo modprobe vfio-pci\n"
			"  sudo dpdk-devbind.py -b vfio-pci");
		for (int i = 0; i < nb_nics; i++) {
			const struct nic *n = &nics[i];

			if (!n->bound && (n->compat == COMPAT_FULL || n->compat == COMPAT_PARTIAL))
				fprintf(stderr, " %s", n->addr);
		}
		fprintf(stderr, "\n");
	}
	fprintf(stderr, "or use virtual devices (--vdev=...) for testing.\n");
}

int
nics_check(int argc, char **argv, unsigned int min_ports, const struct app_config *cfg)
{
	struct nic nics[MAX_NICS];
	bool use_color = isatty(STDERR_FILENO);
	unsigned int ready = 0;
	int nb_nics;

	for (int i = 1; i < argc && strcmp(argv[i], "--") != 0; i++)
		if (strncmp(argv[i], "--vdev", 6) == 0 || strcmp(argv[i], "--no-pci") == 0)
			return 0;

	nb_nics = scan_nics(nics);
	if (nb_nics < 0)
		return -1;
	qsort(nics, (size_t)nb_nics, sizeof(nics[0]), cmp_addr);

	// DPDK probes devices in PCI address order and numbers its ports in that order.
	for (int i = 0; i < nb_nics; i++)
		if (nics[i].bound && nics[i].compat != COMPAT_NONE)
			nics[i].port = (int)ready++;

	fprintf(stderr, "PCI network devices (DPDK port ids follow PCI address order):\n");
	for (int i = 0; i < nb_nics; i++)
		print_nic(&nics[i], cfg, use_color);

	if (ready < min_ports) {
		print_bind_help(ready, min_ports, nics, nb_nics);
		return -1;
	}
	return 0;
}
