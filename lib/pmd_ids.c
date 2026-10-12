#define _GNU_SOURCE
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pmd_ids.h"

#define PMD_INFO_TAG "PMD_INFO_STRING= "

static const char *const pmd_globs[] = {
	"/usr/lib/*/dpdk/pmds-*/librte_net_*.so",
	"/usr/lib64/dpdk/pmds-*/librte_net_*.so",
	"/usr/lib/dpdk/pmds-*/librte_net_*.so",
	"/usr/local/lib/*/dpdk/pmds-*/librte_net_*.so",
	"/usr/local/lib64/dpdk/pmds-*/librte_net_*.so",
	"/usr/local/lib/dpdk/pmds-*/librte_net_*.so",
};

struct pmd_entry {
	uint16_t vendor;
	uint16_t device;
	char name[32];
};

static struct pmd_entry *table;
static size_t table_len, table_cap;
static int loaded;

static void
add_entry(uint16_t vendor, uint16_t device, const char *name)
{
	if (table_len == table_cap) {
		size_t cap = table_cap ? table_cap * 2 : 256;
		struct pmd_entry *t = realloc(table, cap * sizeof(*t));
		if (t == NULL)
			return;
		table = t;
		table_cap = cap;
	}
	table[table_len].vendor = vendor;
	table[table_len].device = device;
	snprintf(table[table_len].name, sizeof(table[table_len].name), "%s", name);
	table_len++;
}

// Each JSON blob is NUL-terminated, so strstr() stays inside one PMD's info.
static void
parse_info(const char *json)
{
	char name[32] = "unknown";
	const char *p = strstr(json, "\"name\": \"");
	const char *ids = strstr(json, "\"pci_ids\": [");

	if (p != NULL) {
		size_t n = strcspn(p += 9, "\"");
		snprintf(name, sizeof(name), "%.*s", (int)n, p);
	}
	if (ids == NULL)
		return;

	for (const char *c = ids + 12;;) {
		char *end;
		unsigned long vendor, device;

		while (*c == ' ' || *c == ',')
			c++;
		if (*c != '[')
			break;
		vendor = strtoul(c + 1, &end, 10);
		c = end;
		while (*c == ' ' || *c == ',')
			c++;
		device = strtoul(c, &end, 10);
		c = end;
		while (*c != '\0' && *c != ']')
			c++;
		if (*c == ']')
			c++;
		add_entry((uint16_t)vendor, (uint16_t)device, name);
	}
}

static void
parse_library(const char *path)
{
	FILE *f = fopen(path, "rb");
	char *buf;
	long size;
	const char *p;

	if (f == NULL)
		return;
	if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) <= 0 || fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return;
	}
	buf = malloc((size_t)size + 1);
	if (buf == NULL || fread(buf, 1, (size_t)size, f) != (size_t)size) {
		free(buf);
		fclose(f);
		return;
	}
	fclose(f);
	buf[size] = '\0';

	for (p = buf; (p = memmem(p, (size_t)(buf + size - p), PMD_INFO_TAG,
				  sizeof(PMD_INFO_TAG) - 1)) != NULL;) {
		p += sizeof(PMD_INFO_TAG) - 1;
		parse_info(p);
	}
	free(buf);
}

static void
load_tables(void)
{
	loaded = 1;
	for (size_t i = 0; i < sizeof(pmd_globs) / sizeof(pmd_globs[0]); i++) {
		glob_t g;

		if (glob(pmd_globs[i], 0, NULL, &g) != 0)
			continue;
		for (size_t j = 0; j < g.gl_pathc; j++)
			parse_library(g.gl_pathv[j]);
		globfree(&g);
	}
}

int
pmd_ids_lookup(uint16_t vendor, uint16_t device, char *name, size_t len)
{
	if (!loaded)
		load_tables();
	if (table_len == 0)
		return -1;

	for (size_t i = 0; i < table_len; i++) {
		if (table[i].vendor == vendor && table[i].device == device) {
			snprintf(name, len, "%s", table[i].name);
			return 1;
		}
	}
	return 0;
}
