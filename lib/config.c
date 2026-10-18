#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"

#define CONFIG_FILE_NAME "flowfwd.conf"
#define MAX_SECTION_LEN 32

#define DEFAULT_NB_QUEUES 1
#define DEFAULT_IN_PORT 0
#define DEFAULT_OUT_PORT 1
#define DEFAULT_RSS_PATTERN 0x6d5a
#define DEFAULT_LOG_LEVEL 2
#define DEFAULT_LOG_CONSOLE 1
#define DEFAULT_LOG_FILE "log.txt"
#define DEFAULT_EXPORT_INTERVAL 10 // seconds
#define DEFAULT_EXPORT_DIR "."
#define DEFAULT_PASS_ARP 1
#define DEFAULT_FLOW_TIMEOUT 60     // seconds
#define DEFAULT_CLEANUP_INTERVAL 10 // seconds
#define DEFAULT_TABLE_SIZE 65536    // flows per worker

#define MIN_TABLE_SIZE 8 // the smallest table the DPDK hash library accepts
#define MAX_TABLE_SIZE (1u << 20)

struct option {
  const char *section;
  const char *key;
  size_t offset;
  size_t size; // of the field in struct app_config, 2 or 4 bytes
  int base;
  unsigned long min;
  unsigned long max;
};

#define OPTION(section, key, field, base, min, max)                            \
  {                                                                            \
    section, key, offsetof(struct app_config, field),                          \
        sizeof(((struct app_config *)0)->field), base, min, max                \
  }

static const struct option options[] = {
    OPTION("ports", "in_port", in_port, 10, 0, UINT16_MAX),
    OPTION("ports", "out_port", out_port, 10, 0, UINT16_MAX),
    OPTION("ports", "queues", nb_queues, 10, 1, MAX_QUEUES),
    // 0x0000 and 0xffff give a degenerate hash, so they are rejected.
    OPTION("rss", "pattern", rss_pattern, 16, 1, 0xfffe),
    OPTION("logging", "level", log_level, 10, 0, 3),
    OPTION("logging", "console", log_console, 10, 0, 1),
    OPTION("export", "interval", export_interval, 10, 1, 3600),
    OPTION("filter", "pass_arp", pass_arp, 10, 0, 1),
    OPTION("flows", "timeout", flow_timeout, 10, 1, UINT16_MAX),
    OPTION("flows", "cleanup_interval", cleanup_interval, 10, 1, 3600),
    OPTION("flows", "table_size", table_size, 10, MIN_TABLE_SIZE,
           MAX_TABLE_SIZE),
};

#define NB_OPTIONS (sizeof(options) / sizeof(options[0]))

struct mac_option {
  const char *section;
  const char *key;
  size_t offset;
};

static const struct mac_option mac_options[] = {
    {"rewrite", "forward_dst_mac",
     offsetof(struct app_config, forward_dst_mac)},
    {"rewrite", "reverse_dst_mac",
     offsetof(struct app_config, reverse_dst_mac)},
};

#define NB_MAC_OPTIONS (sizeof(mac_options) / sizeof(mac_options[0]))

struct string_option {
  const char *section;
  const char *key;
  size_t offset;
  size_t size; // of the field in struct app_config
};

#define STRING_OPTION(section, key, field)                                     \
  {                                                                            \
    section, key, offsetof(struct app_config, field),                          \
        sizeof(((struct app_config *)0)->field)                                \
  }

static const struct string_option string_options[] = {
    STRING_OPTION("logging", "file", log_file),
    STRING_OPTION("export", "directory", export_dir),
};

#define NB_STRING_OPTIONS (sizeof(string_options) / sizeof(string_options[0]))

static char *trim(char *s) {
  char *end;

  while (isspace((unsigned char)*s)) {
    s++;
  }
  end = s + strlen(s);
  while (end > s && isspace((unsigned char)end[-1])) {
    end--;
  }
  *end = '\0';
  return s;
}

static void set_defaults(struct app_config *cfg) {//ok
  cfg->nb_queues = DEFAULT_NB_QUEUES;
  cfg->in_port = DEFAULT_IN_PORT;
  cfg->out_port = DEFAULT_OUT_PORT;
  cfg->rss_pattern = DEFAULT_RSS_PATTERN;
  cfg->log_level = DEFAULT_LOG_LEVEL;
  cfg->log_console = DEFAULT_LOG_CONSOLE;
  snprintf(cfg->log_file, sizeof(cfg->log_file), "%s", DEFAULT_LOG_FILE);
  cfg->export_interval = DEFAULT_EXPORT_INTERVAL;
  snprintf(cfg->export_dir, sizeof(cfg->export_dir), "%s", DEFAULT_EXPORT_DIR);
  cfg->pass_arp = DEFAULT_PASS_ARP;
  cfg->flow_timeout = DEFAULT_FLOW_TIMEOUT;
  cfg->cleanup_interval = DEFAULT_CLEANUP_INTERVAL;
  cfg->table_size = DEFAULT_TABLE_SIZE;
  memset(cfg->forward_dst_mac, 0, sizeof(cfg->forward_dst_mac));
  memset(cfg->reverse_dst_mac, 0, sizeof(cfg->reverse_dst_mac));
}

static bool is_known_section(const char *section) {
  for (size_t i = 0; i < NB_OPTIONS; i++) {
    if (strcmp(section, options[i].section) == 0) {
      return true;
    }
  }
  for (size_t i = 0; i < NB_MAC_OPTIONS; i++) {
    if (strcmp(section, mac_options[i].section) == 0) {
      return true;
    }
  }
  return false;
}

static bool parse_mac(const char *text, uint8_t *mac) {
  if (strlen(text) != 17) {
    return false;
  }
  for (int i = 0; i < 6; i++) {
    const char *p = text + i * 3;
    char byte[3] = {p[0], p[1], '\0'};

    if (!isxdigit((unsigned char)p[0]) || !isxdigit((unsigned char)p[1]) ||
        (i < 5 && p[2] != ':')) {
      return false;
    }
    mac[i] = (uint8_t)strtoul(byte, NULL, 16);
  }
  return true;
}

static void store_value(const struct option *o, struct app_config *cfg,
                        unsigned long v) {
  char *field = (char *)cfg + o->offset;

  if (o->size == sizeof(unsigned int)) {
    *(unsigned int *)field = (unsigned int)v;
  } else {
    *(uint16_t *)field = (uint16_t)v;
  }
}

static bool apply_option(const char *path, int line, const char *section,
                         const char *key, const char *val,
                         struct app_config *cfg) {
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
      fprintf(stderr,
              o->base == 16
                  ? "%s:%d: invalid value '%s' for %s (expected 0x%lx-0x%lx)\n"
                  : "%s:%d: invalid value '%s' for %s (expected %lu-%lu)\n",
              path, line, val, key, o->min, o->max);
      return false;
    }
    store_value(o, cfg, v);
    return true;
  }

  for (size_t i = 0; i < NB_MAC_OPTIONS; i++) {
    const struct mac_option *o = &mac_options[i];

    if (strcmp(section, o->section) != 0 || strcmp(key, o->key) != 0) {
      continue;
    }
    if (!parse_mac(val, (uint8_t *)cfg + o->offset)) {
      fprintf(stderr,
              "%s:%d: invalid value '%s' for %s (expected aa:bb:cc:dd:ee:ff)\n",
              path, line, val, key);
      return false;
    }
    return true;
  }

  for (size_t i = 0; i < NB_STRING_OPTIONS; i++) {
    const struct string_option *o = &string_options[i];

    if (strcmp(section, o->section) != 0 || strcmp(key, o->key) != 0) {
      continue;
    }
    if (*val == '\0' || strlen(val) >= o->size) {
      fprintf(stderr, "%s:%d: invalid value for %s (expected 1-%zu characters)\n",
              path, line, key, o->size - 1);
      return false;
    }
    snprintf((char *)cfg + o->offset, o->size, "%s", val);
    return true;
  }

  fprintf(stderr, "%s:%d: unknown option '%s' in section [%s]\n", path, line,
          key, section);
  return false;
}

static bool load_file(const char *path, struct app_config *cfg) {//ok
  char buf[256], section[MAX_SECTION_LEN] = "";
  int line = 0;
  bool ok = true;
  FILE *f = fopen(path, "r");

  if (f == NULL) {
    fprintf(stderr,
            "cannot open config file %s: %s (setup.sh copies it next to the "
            "binary)\n",
            path, strerror(errno));
    return false;
  }

  set_defaults(cfg);

  while (fgets(buf, sizeof(buf), f) != NULL) {
    char *hash, *eq, *key, *val;

    line++;
    if (strchr(buf, '\n') == NULL && !feof(f)) {
      fprintf(stderr, "%s:%d: line too long\n", path, line);
      ok = false;
      break;
    }

    hash = strchr(buf, '#');
    if (hash != NULL){
      *hash = '\0';
    }
    key = trim(buf);
    if (*key == '\0'){
      continue;
    }

    if (*key == '[') {
      size_t n = strlen(key);

      if (n < 3 || key[n - 1] != ']' || n - 2 >= sizeof(section)) {
        fprintf(stderr, "%s:%d: malformed section header\n", path, line);
        ok = false;
        break;
      }
      key[n - 1] = '\0';
      snprintf(section, sizeof(section), "%s", trim(key + 1));
      if (!is_known_section(section)) {
        fprintf(stderr, "%s:%d: unknown section [%s]\n", path, line, section);
        ok = false;
        break;
      }
      continue;
    }

    eq = strchr(key, '=');
    if (eq == NULL) {
      fprintf(stderr, "%s:%d: expected 'key = value' or '[section]'\n", path,
              line);
      ok = false;
      break;
    }
    if (section[0] == '\0') {
      fprintf(stderr, "%s:%d: option outside of a [section]\n", path, line);
      ok = false;
      break;
    }
    *eq = '\0';
    val = trim(eq + 1);
    key = trim(key);

    if (!apply_option(path, line, section, key, val, cfg)) {
      ok = false;
      break;
    }
  }

  fclose(f);
  return ok;
}

static bool default_path(char *buf, size_t len) {//ok
  char binary[PATH_MAX];
  ssize_t n =
      readlink("/proc/self/exe", binary,
               sizeof(binary) - 1); // get the path of the current executable
  

  if (n < 0 || (size_t)n == sizeof(binary) - 1){
    return false;
  }

  char *slash;
  binary[n] = '\0';
  slash = strrchr(binary, '/');
  if (slash == NULL){
    return false;
  }
    
  *slash = '\0';
  return (size_t)snprintf(buf, len, "%s/%s", binary, CONFIG_FILE_NAME) < len;
}

bool config_load(struct app_config *cfg) {//ok
  if (!default_path(cfg->path, sizeof(cfg->path))) {
    fprintf(stderr, "cannot locate the binary's directory\n");
    return false;
  }
  return load_file(cfg->path, cfg);
}
