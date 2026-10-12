#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"

#define CONFIG_FILE_NAME "flowfwd.conf"
#define MAX_SECTION_LEN 32

struct option {
	const char *section;
	const char *key;
	size_t offset;
	int base;
	unsigned long min;
	unsigned long max;
};

static const struct option options[] = {
	{ "ports", "in_port",  offsetof(struct app_config, in_port),     10, 0, UINT16_MAX },
	{ "ports", "out_port", offsetof(struct app_config, out_port),    10, 0, UINT16_MAX },
	{ "ports", "queues",   offsetof(struct app_config, nb_queues),   10, 1, MAX_QUEUES },
	// 0x0000 and 0xffff give a degenerate hash, so they are rejected.
	{ "rss",   "pattern",  offsetof(struct app_config, rss_pattern), 16, 1, 0xfffe },
	{ "logging", "level",   offsetof(struct app_config, log_level),   10, 0, 3 },
	{ "logging", "console", offsetof(struct app_config, log_console), 10, 0, 1 },
	{ "export",  "interval", offsetof(struct app_config, export_interval), 10, 1, 3600 },
	{ "filter", "pass_arp", offsetof(struct app_config, pass_arp), 10, 0, 1 },
};

#define NB_OPTIONS (sizeof(options) / sizeof(options[0]))

static char *
trim(char *s)
{
	char *end;

	while (isspace((unsigned char)*s))
		s++;
	end = s + strlen(s);
	while (end > s && isspace((unsigned char)end[-1]))
		end--;
	*end = '\0';
	return s;
}

static void
set_defaults(struct app_config *cfg)
{
	cfg->nb_queues = 1;
	cfg->in_port = 0;
	cfg->out_port = 1;
	cfg->rss_pattern = 0x6d5a;
	cfg->log_level = 2;
	cfg->log_console = 1;
	cfg->export_interval = 10;
	cfg->pass_arp = 1;
}

static int
is_known_section(const char *section)
{
	for (size_t i = 0; i < NB_OPTIONS; i++)
		if (strcmp(section, options[i].section) == 0)
			return 1;
	return 0;
}

static int
apply(const char *path, int line, const char *section, const char *key, const char *val,
      struct app_config *cfg)
{
	for (size_t i = 0; i < NB_OPTIONS; i++) {
		const struct option *o = &options[i];
		char *end;
		unsigned long v;

		if (strcmp(section, o->section) != 0 || strcmp(key, o->key) != 0)
			continue;

		errno = 0;
		v = strtoul(val, &end, o->base);
		if (!isxdigit((unsigned char)*val) || *end != '\0' || errno != 0 ||
		    v < o->min || v > o->max) {
			fprintf(stderr, o->base == 16 ?
				"%s:%d: invalid value '%s' for %s (expected 0x%lx-0x%lx)\n" :
				"%s:%d: invalid value '%s' for %s (expected %lu-%lu)\n",
				path, line, val, key, o->min, o->max);
			return -1;
		}
		*(uint16_t *)((char *)cfg + o->offset) = (uint16_t)v;
		return 0;
	}

	fprintf(stderr, "%s:%d: unknown option '%s' in section [%s]\n", path, line, key, section);
	return -1;
}

static int
load_file(const char *path, struct app_config *cfg)
{
	char buf[256], section[MAX_SECTION_LEN] = "";
	int line = 0, rc = 0;
	FILE *f = fopen(path, "r");

	if (f == NULL) {
		fprintf(stderr, "cannot open config file %s: %s (setup.sh copies it next to the binary)\n",
			path, strerror(errno));
		return -1;
	}

	set_defaults(cfg);

	while (fgets(buf, sizeof(buf), f) != NULL) {
		char *hash, *eq, *key, *val;

		line++;
		if (strchr(buf, '\n') == NULL && !feof(f)) {
			fprintf(stderr, "%s:%d: line too long\n", path, line);
			rc = -1;
			break;
		}

		hash = strchr(buf, '#');
		if (hash != NULL)
			*hash = '\0';
		key = trim(buf);
		if (*key == '\0')
			continue;

		if (*key == '[') {
			size_t n = strlen(key);

			if (n < 3 || key[n - 1] != ']' || n - 2 >= sizeof(section)) {
				fprintf(stderr, "%s:%d: malformed section header\n", path, line);
				rc = -1;
				break;
			}
			key[n - 1] = '\0';
			snprintf(section, sizeof(section), "%s", trim(key + 1));
			if (!is_known_section(section)) {
				fprintf(stderr, "%s:%d: unknown section [%s]\n", path, line, section);
				rc = -1;
				break;
			}
			continue;
		}

		eq = strchr(key, '=');
		if (eq == NULL) {
			fprintf(stderr, "%s:%d: expected 'key = value' or '[section]'\n", path, line);
			rc = -1;
			break;
		}
		if (section[0] == '\0') {
			fprintf(stderr, "%s:%d: option outside of a [section]\n", path, line);
			rc = -1;
			break;
		}
		*eq = '\0';
		val = trim(eq + 1);
		key = trim(key);

		if (apply(path, line, section, key, val, cfg) != 0) {
			rc = -1;
			break;
		}
	}

	fclose(f);
	return rc;
}

static int
default_path(char *buf, size_t len)
{
	char exe[PATH_MAX];
	ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	char *slash;

	if (n < 0)
		return -1;
	exe[n] = '\0';
	slash = strrchr(exe, '/');
	if (slash == NULL)
		return -1;
	*slash = '\0';
	if ((size_t)snprintf(buf, len, "%s/%s", exe, CONFIG_FILE_NAME) >= len)
		return -1;
	return 0;
}

int
config_load(struct app_config *cfg)
{
	if (default_path(cfg->path, sizeof(cfg->path)) != 0) {
		fprintf(stderr, "cannot locate the binary's directory\n");
		return -1;
	}
	return load_file(cfg->path, cfg);
}

int
config_dir_file(const struct app_config *cfg, const char *name, char *buf, size_t len)
{
	const char *slash = strrchr(cfg->path, '/');
	int dir_len = slash != NULL ? (int)(slash - cfg->path) : 0;

	if (slash == NULL)
		return -1;
	if ((size_t)snprintf(buf, len, "%.*s/%s", dir_len, cfg->path, name) >= len)
		return -1;
	return 0;
}
