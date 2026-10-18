// DPDK packet forwarder with per-flow tracking.
// Step 2: EAL init, mempool, two ports with multi-queue RSS, forwarding
// workers.

#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_log.h>

#include "common.h"
#include "config.h"
#include "forward.h"
#include "hugepages.h"
#include "logger.h"
#include "nics.h"
#include "ports.h"

static struct app_config cfg;
static volatile sig_atomic_t quit_requested;

static void on_signal(int sig) {
  (void)sig;
  quit_requested = 1;
}

static void print_usage(const char *prog) {
  printf(
      "Usage: %s [EAL options]\n"
      "Settings are read from flowfwd.conf in the directory of the binary.\n",
      prog);
}

int main(int argc, char **argv) {
  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);

  if (!config_load(&cfg)){
    return EXIT_FAILURE;
  }
 
  if (hugepages_check(argc, argv)) {
    hugepages_cleanup();
  } else {
    RTE_LOG(ERR, FLOWFWD, "hugepages check failed\n");
    return EXIT_FAILURE;
  }

  if (!nics_check(argc, argv, NB_PORTS, &cfg)){
	return EXIT_FAILURE;
  }
    

  int ret = rte_eal_init(argc, argv);
  if (ret < 0){
	rte_exit(EXIT_FAILURE, "EAL init failed: %s\n", rte_strerror(rte_errno));
	return EXIT_FAILURE;
  }
  argc -= ret;
  argv += ret;

  if (argc > 1) {
    print_usage(argv[0]);
    rte_exit(EXIT_FAILURE, "unexpected arguments after EAL options\n");
	return EXIT_FAILURE;
  }

  if (!logger_start(cfg.log_file, cfg.log_level, cfg.log_console)) {
    rte_exit(EXIT_FAILURE, "cannot start the logger\n");
	return EXIT_FAILURE;
  }

  RTE_LOG(
      INFO, FLOWFWD,
      "config %s: queues=%u in_port=%u out_port=%u rss_pattern=0x%04x"
      " export_interval=%us pass_arp=%u flow_timeout=%us cleanup_interval=%us"
      " table_size=%u\n",
      cfg.path, cfg.nb_queues, cfg.in_port, cfg.out_port, cfg.rss_pattern,
      cfg.export_interval, cfg.pass_arp, cfg.flow_timeout, cfg.cleanup_interval,
      cfg.table_size);

  if (!ports_start(&cfg)){
    rte_exit(EXIT_FAILURE, "cannot set up the ports\n");
    return EXIT_FAILURE;
  }
    

  if (!forward_start(&cfg)){
    rte_exit(EXIT_FAILURE, "cannot start the forwarding workers\n");
    return EXIT_FAILURE;
  }

  RTE_LOG(INFO, FLOWFWD, "forwarding, press Ctrl+C to stop\n");

  char worker_load[256];
  while (!quit_requested) {
    sleep(1);
    forward_worker_usage(worker_load, sizeof(worker_load));
    RTE_LOG(INFO, FLOWFWD,
            "rx=%" PRIu64 " tx=%" PRIu64 " drops=%" PRIu64 " flows=%" PRIu64
            " load(rx/tx)=%s\n",
            forward_rx_packets(), forward_tx_packets(), forward_drops(&cfg),
            forward_flows(), worker_load);
  }

  forward_stop();
  RTE_LOG(INFO, FLOWFWD,
          "stopped: rx=%" PRIu64 " tx=%" PRIu64 " drops=%" PRIu64
          " flows=%" PRIu64 "\n",
          forward_rx_packets(), forward_tx_packets(), forward_drops(&cfg),
          forward_flows());

  ports_stop(&cfg);
  logger_stop();
  rte_eal_cleanup();
  hugepages_cleanup();
  return 0;
}
