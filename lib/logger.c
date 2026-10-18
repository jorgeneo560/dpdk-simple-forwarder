#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <rte_common.h>
#include <rte_lcore.h>
#include <rte_log.h>

#include "common.h"
#include "logger.h"

#define QUEUE_LEN 1024
#define TEXT_LEN 256

struct log_msg {
  struct timespec ts;
  unsigned int lcore;
  size_t len;
  char text[TEXT_LEN];
};

// Circular queue between the threads that log and the logger thread; all
// guarded by lock.
static struct log_msg queue[QUEUE_LEN];
static unsigned int q_head, q_tail, q_count;
static unsigned long dropped;
static bool accepting;
static bool stopping;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;

static pthread_t thread;
static bool started;
static bool echo_console;
static bool line_open; // logger thread only: the last chunk written did not end
                       // its line
static FILE *log_file;
static FILE *stream;

// Called by stdio from whichever thread logs: only copies the text into the
// queue.
static ssize_t stream_write(void *cookie, const char *buf, size_t size) {
  struct log_msg *m;

  (void)cookie;
  pthread_mutex_lock(&lock);
  if (!accepting) {
    pthread_mutex_unlock(&lock);
    fwrite(buf, 1, size, stderr);
    return (ssize_t)size;
  }
  if (q_count == QUEUE_LEN) {
    dropped++;
    pthread_mutex_unlock(&lock);
    return (ssize_t)size;
  }

  m = &queue[q_tail];
  clock_gettime(CLOCK_REALTIME, &m->ts);
  m->lcore = rte_lcore_id();
  m->len = size < TEXT_LEN ? size : TEXT_LEN - 1;
  memcpy(m->text, buf, m->len);
  q_tail = (q_tail + 1) % QUEUE_LEN;
  q_count++;
  pthread_cond_signal(&wake);
  pthread_mutex_unlock(&lock);
  return (ssize_t)size;
}

static void write_msg(const struct log_msg *m) {
  bool ends_line = m->len > 0 && m->text[m->len - 1] == '\n';

  // A line written in several pieces (DPDK does this) gets one timestamp.
  if (!line_open) {
    char stamp[32], who[24];
    struct tm tm;

    localtime_r(&m->ts.tv_sec, &tm);
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm);
    if (m->lcore == LCORE_ID_ANY)
      snprintf(who, sizeof(who), "other");
    else
      snprintf(who, sizeof(who), "lcore %u", m->lcore);
    fprintf(log_file, "%s.%03ld [%s] ", stamp, m->ts.tv_nsec / 1000000, who);
  }
  fwrite(m->text, 1, m->len, log_file);
  fflush(log_file);
  line_open = !ends_line;

  if (echo_console)
    fwrite(m->text, 1, m->len, stderr);
}

static void *logger_main(void *arg) {
  struct log_msg m;

  (void)arg;
  for (;;) {
    pthread_mutex_lock(&lock);
    while (q_count == 0 && !stopping) {
      pthread_cond_wait(&wake, &lock);
    }
    if (q_count == 0) {
      accepting = false;
      pthread_mutex_unlock(&lock);
      return NULL;
    }
    m = queue[q_head];
    q_head = (q_head + 1) % QUEUE_LEN;
    q_count--;
    pthread_mutex_unlock(&lock);

    write_msg(&m);
  }
}

bool logger_start(const char *path, unsigned int level, bool console) {//check 
  static const uint32_t levels[] = {RTE_LOG_ERR, RTE_LOG_WARNING, RTE_LOG_INFO,
                                    RTE_LOG_DEBUG};
  cookie_io_functions_t fns = {.write = stream_write};
  int err;

  if (started) {
    fprintf(stderr, "logger already started\n");
    return false;
  }
  if (level >= RTE_DIM(levels)) {
    fprintf(stderr, "invalid log level %u (expected 0-%zu)\n", level,
            RTE_DIM(levels) - 1);
    return false;
  }

  log_file = fopen(path, "a");
  if (log_file == NULL) {
    fprintf(stderr, "cannot open log file %s: %s\n", path, strerror(errno));
    return false;
  }
  stream = fopencookie(NULL, "w", fns);
  if (stream == NULL) {
    fprintf(stderr, "cannot create the log stream: %s\n", strerror(errno));
    fclose(log_file);
    return false;
  }
  // Line buffered, so each RTE_LOG call reaches stream_write as whole lines
  // that fit one slot.
  setvbuf(stream, NULL, _IOLBF, TEXT_LEN);

  echo_console = console;
  accepting = true;
  stopping = false;
  err = pthread_create(&thread, NULL, logger_main, NULL);
  if (err != 0) {
    fprintf(stderr, "cannot start the logger thread: %s\n", strerror(err));
    fclose(stream);
    fclose(log_file);
    accepting = false;
    return false;
  }
  pthread_setname_np(thread, "flowfwd-log");
  started = true;

  // Only our own log type follows the configured level; DPDK libraries keep
  // theirs.
  rte_log_set_global_level(RTE_LOG_DEBUG);
  rte_log_set_level(RTE_LOGTYPE_FLOWFWD, levels[level]);
  rte_openlog_stream(stream);
  atexit(logger_stop);

  RTE_LOG(INFO, FLOWFWD, "logger started: file %s, level %u\n", path, level);
  return true;
}

void logger_stop(void) {
  if (!started)
    return;
  started = false;

  rte_openlog_stream(NULL);
  fclose(stream);

  pthread_mutex_lock(&lock);
  stopping = true;
  pthread_cond_signal(&wake);
  pthread_mutex_unlock(&lock);
  pthread_join(thread, NULL);

  if (line_open)
    fputc('\n', log_file);
  if (dropped > 0)
    fprintf(log_file, "%lu log message(s) dropped (queue full)\n", dropped);
  fclose(log_file);
}
