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
static FILE *log_file;
static FILE *stream;

// Called by stdio from whichever thread logs: only copies the text into the
// queue.
static ssize_t stream_write(void *cookie, const char *buf, size_t size) {
  struct log_msg *msg;

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

  msg = &queue[q_tail];
  clock_gettime(CLOCK_REALTIME, &msg->ts);
  msg->lcore = rte_lcore_id();
  msg->len = size < TEXT_LEN ? size : TEXT_LEN - 1;
  memcpy(msg->text, buf, msg->len);
  if (size >= TEXT_LEN) {
    msg->text[msg->len - 1] = '\n'; // a line cut to fit the slot still ends
  }
  q_tail = (q_tail + 1) % QUEUE_LEN;
  q_count++;
  pthread_cond_signal(&wake);
  pthread_mutex_unlock(&lock);
  return (ssize_t)size;
}

static void write_msg(const struct log_msg *msg) {
  char timestamp[32], source[24];
  struct tm local_time;

  localtime_r(&msg->ts.tv_sec, &local_time);
  strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &local_time);
  if (msg->lcore == LCORE_ID_ANY) {
    snprintf(source, sizeof(source), "other");
  } else {
    snprintf(source, sizeof(source), "lcore %u", msg->lcore);
  }
  fprintf(log_file, "%s.%03ld [%s] ", timestamp, msg->ts.tv_nsec / 1000000,
          source);
  fwrite(msg->text, 1, msg->len, log_file);
  fflush(log_file);

  if (echo_console) {
    fwrite(msg->text, 1, msg->len, stderr);
  }
}

static void *logger_main(void *arg) {
  struct log_msg msg;

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
    msg = queue[q_head];
    q_head = (q_head + 1) % QUEUE_LEN;
    q_count--;
    pthread_mutex_unlock(&lock);

    write_msg(&msg);
  }
}

bool logger_start(const char *path, unsigned int level, bool console) {
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
  if (!started) {
    return;
  }
  started = false;

  rte_openlog_stream(NULL);
  fclose(stream);

  pthread_mutex_lock(&lock);
  stopping = true;
  pthread_cond_signal(&wake);
  pthread_mutex_unlock(&lock);
  pthread_join(thread, NULL);

  if (dropped > 0) {
    fprintf(log_file, "%lu log message(s) dropped (queue full)\n", dropped);
  }
  fclose(log_file);
}
