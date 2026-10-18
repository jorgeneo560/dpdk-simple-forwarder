#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mntent.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

#include "hugepages.h"

#define HUGEPAGE_SYSFS "/sys/kernel/mm/hugepages"

static bool read_hugepage_count(const char *dir, const char *file, //ok
                                unsigned long *val) {
  char path[256];
  FILE *f;
  bool ok;

  snprintf(path, sizeof(path), HUGEPAGE_SYSFS "/%s/%s", dir, file);
  f = fopen(path, "r");
  if (f == NULL)
    return false;
  ok = fscanf(f, "%lu", val) == 1;
  fclose(f);
  return ok;
}

bool hugepages_check(int argc, char **argv) { //ok
  uint64_t free_bytes = 0;
  struct dirent *de;
  DIR *dir;

  for (int i = 1; i < argc && strcmp(argv[i], "--") != 0; i++) {
    if (strcmp(argv[i], "--no-huge") == 0) {
      return true;
    }
  }

  dir = opendir(HUGEPAGE_SYSFS);
  if (dir == NULL) {
    fprintf(stderr, "cannot read %s: %s\n", HUGEPAGE_SYSFS, strerror(errno));
    return false;
  }

  while ((de = readdir(dir)) != NULL) {
    unsigned long size_kb, total, free_pages;

    if (sscanf(de->d_name, "hugepages-%lukB", &size_kb) != 1)
      continue;
    if (!read_hugepage_count(de->d_name, "nr_hugepages", &total) ||
        !read_hugepage_count(de->d_name, "free_hugepages", &free_pages))
      continue;
    if (total == 0)
      continue;

    fprintf(stderr, "hugepages %lu kB: %lu allocated, %lu free\n", size_kb,
            total, free_pages);
    free_bytes += (uint64_t)free_pages * size_kb * 1024;
  }
  closedir(dir);

  if (free_bytes == 0) {
    fprintf(stderr, "no free hugepages. Allocate some, e.g.:\n"
                    "  sudo dpdk-hugepages.py -p 2M --setup 2G\n"
                    "or run with --no-huge for testing only.\n");
    return false;
  }
  return true;
}

static unsigned int remove_unused_maps(const char *mount_dir) {
  unsigned int removed = 0;
  struct dirent *de;
  DIR *dir = opendir(mount_dir);

  if (dir == NULL)
    return 0;

  while ((de = readdir(dir)) != NULL) {
    char path[PATH_MAX];
    int fd;

    if (strncmp(de->d_name, "rtemap_", 7) != 0)
      continue;
    snprintf(path, sizeof(path), "%s/%s", mount_dir, de->d_name);
    fd = open(path, O_RDWR);
    if (fd < 0)
      continue;
    // EAL holds a flock on files in use, so a free lock means no process uses
    // it.
    if (flock(fd, LOCK_EX | LOCK_NB) == 0 && unlink(path) == 0)
      removed++;
    close(fd);
  }
  closedir(dir);
  return removed;
}

void hugepages_cleanup(void) { //ok
  unsigned int removed = 0;
  struct mntent *m;
  FILE *f = setmntent("/proc/mounts", "r");

  if (f == NULL){
    return;
  }
    
  while ((m = getmntent(f)) != NULL) {
    if (strcmp(m->mnt_type, "hugetlbfs") == 0) {
      removed += remove_unused_maps(m->mnt_dir);
    }
  }
  endmntent(f);

  if (removed > 0)
    fprintf(stderr, "released %u unused hugepage file(s)\n", removed);
}
