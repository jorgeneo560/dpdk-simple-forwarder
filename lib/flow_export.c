#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <rte_log.h>

#include "common.h"
#include "flow_export.h"
#include "writer.h"

#define WRITER_POLL_NS                                                         \
  50000000 // the writer looks for new snapshots every 50 ms

// One per worker: the worker fills recs and sets ready, the writer writes it
// and clears ready.
struct slot {
  atomic_bool ready;
  size_t n;
  struct timespec ts;
  struct flow_record recs[FLOW_EXPORT_MAX];
};

static struct slot slots[MAX_QUEUES];
static struct writer writers[MAX_QUEUES];
static unsigned int nb_slots;
static pthread_t thread;
static atomic_bool stopping;
static bool started;

static void *writer_main(void *arg) {
  const struct timespec pause = {.tv_sec = 0, .tv_nsec = WRITER_POLL_NS};

  (void)arg;
  for (;;) {
    // Read before scanning, so a stop request still gets one last full scan.
    bool stop = atomic_load(&stopping);

    for (unsigned int i = 0; i < nb_slots; i++) {
      struct slot *s = &slots[i];

      if (!atomic_load_explicit(&s->ready, memory_order_acquire)) {
        continue;
      }
      if (!writer_write(&writers[i], &s->ts, s->recs, s->n)) {
        RTE_LOG(ERR, FLOWFWD, "export write failed for worker %u: %s\n", i,
                strerror(errno));
      }
      atomic_store_explicit(&s->ready, false, memory_order_release);
    }
    if (stop) {
      return NULL;
    }
    nanosleep(&pause, NULL);
  }
}

static void close_exporters(unsigned int n) {
  for (unsigned int i = 0; i < n; i++) {
    writer_close(&writers[i]);
  }
}

bool flow_export_start(const struct app_config *cfg, const unsigned int *lcores,
                       unsigned int n) {
  if (started) {
    RTE_LOG(ERR, FLOWFWD, "flow export already started\n");
    return false;
  }

  if (n > MAX_QUEUES) {
    RTE_LOG(ERR, FLOWFWD, "too many queues for flow export: %u\n", n);
    return false;
  }

  for (unsigned int i = 0; i < n; i++) {
    char name[48], path[PATH_MAX];

    snprintf(name, sizeof(name), "flow_stats_core_%u.csv", lcores[i]);
    if ((size_t)snprintf(path, sizeof(path), "%s/%s", cfg->export_dir, name) >=
        sizeof(path)) {
      RTE_LOG(ERR, FLOWFWD, "export file name too long: %s\n", name);
      close_exporters(i);
      return false;
    }

    if (!writer_open(&writers[i], path)) {
      RTE_LOG(ERR, FLOWFWD, "cannot open export file %s: %s\n", path,
              strerror(errno));
      close_exporters(i);
      return false;
    }
    atomic_store(&slots[i].ready, false);
    RTE_LOG(INFO, FLOWFWD, "lcore %u: flow statistics go to %s every %us\n",
            lcores[i], path, cfg->export_interval);
  }

  nb_slots = n;
  atomic_store(&stopping, false);
  if (pthread_create(&thread, NULL, writer_main, NULL) != 0) {
    close_exporters(n);
    return false;
  }
  pthread_setname_np(thread, "flowfwd-export");
  started = true;
  return true;
}

struct flow_record *flow_export_begin(unsigned int worker) {
  struct slot *s = &slots[worker];

  if (atomic_load_explicit(&s->ready, memory_order_acquire)) {
    return NULL;
  }
  return s->recs;
}

void flow_export_commit(unsigned int worker, const struct timespec *ts,
                        size_t n) {
  struct slot *s = &slots[worker];

  s->ts = *ts;
  s->n = n < FLOW_EXPORT_MAX ? n : FLOW_EXPORT_MAX;
  atomic_store_explicit(&s->ready, true, memory_order_release);
}

void flow_export_stop(void) {
  if (!started) {
    return;
  }
  started = false;

  atomic_store(&stopping, true);
  pthread_join(thread, NULL);
  close_exporters(nb_slots);
}
