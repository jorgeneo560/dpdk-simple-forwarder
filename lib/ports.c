#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <rte_common.h>
#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_log.h>
#include <rte_mbuf.h>

#include "common.h"
#include "ports.h"
#include "rss.h"

#define RX_DESC 1024
#define TX_DESC 1024
#define MBUF_CACHE 256
#define MBUF_DATA_LEN RTE_MBUF_DEFAULT_BUF_SIZE

static struct rte_mempool *mbuf_pool;

// A key that repeats every 2 bytes makes RSS hash symmetric (A->B and B->A land
// on one queue).
static uint8_t rss_key[128];

static bool start_port(uint16_t port, const struct app_config *cfg) {
  const uint16_t nb_queues = (uint16_t)cfg->nb_queues;
  struct rte_eth_conf conf;
  struct rte_eth_dev_info info;
  uint16_t nb_rxd = RX_DESC, nb_txd = TX_DESC;
  int ret;

  memset(&conf, 0, sizeof(conf));

  ret = rte_eth_dev_info_get(port, &info);
  if (ret != 0) {
    RTE_LOG(ERR, FLOWFWD, "port %u: dev_info_get failed: %s\n", port,
            strerror(-ret));
    return false;
  }

  if (nb_queues > info.max_rx_queues || nb_queues > info.max_tx_queues) {
    RTE_LOG(ERR, FLOWFWD, "port %u: supports at most %u rx / %u tx queues\n",
            port, info.max_rx_queues, info.max_tx_queues);
    return false;
  }

  if (info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE) {
    conf.txmode.offloads |= RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE;
  }

  if (nb_queues > 1) {
    uint64_t rss_hf = (RTE_ETH_RSS_IP | RTE_ETH_RSS_TCP | RTE_ETH_RSS_UDP) &
                      info.flow_type_rss_offloads;
    if (rss_hf == 0) {
      RTE_LOG(ERR, FLOWFWD, "port %u: no RSS support for IP/TCP/UDP\n", port);
      return false;
    }
    conf.rxmode.mq_mode = RTE_ETH_MQ_RX_RSS;
    conf.rx_adv_conf.rss_conf.rss_hf = rss_hf;
    if (info.hash_key_size > 0 && info.hash_key_size <= sizeof(rss_key)) {
      conf.rx_adv_conf.rss_conf.rss_key = rss_key;
      conf.rx_adv_conf.rss_conf.rss_key_len = info.hash_key_size;
    } else {
      RTE_LOG(WARNING, FLOWFWD,
              "port %u: the RSS key cannot be set, so both directions "
              "of a flow may reach different workers\n",
              port);
    }
  }

  ret = rte_eth_dev_configure(port, nb_queues, nb_queues, &conf);
  if (ret != 0) {
    RTE_LOG(ERR, FLOWFWD, "port %u: configure failed: %s\n", port,
            rte_strerror(-ret));
    return false;
  }

  ret = rte_eth_dev_adjust_nb_rx_tx_desc(port, &nb_rxd, &nb_txd);
  if (ret != 0) {
    return false;
  }

  int socket = rte_eth_dev_socket_id(port);
  struct rte_eth_txconf txconf = info.default_txconf;
  txconf.offloads = conf.txmode.offloads;

  for (uint16_t q = 0; q < nb_queues; q++) {
    ret = rte_eth_rx_queue_setup(port, q, nb_rxd, socket, NULL, mbuf_pool);
    if (ret < 0) {
      RTE_LOG(ERR, FLOWFWD, "port %u rxq %u: setup failed: %s\n", port, q,
              rte_strerror(-ret));
      return false;
    }
    ret = rte_eth_tx_queue_setup(port, q, nb_txd, socket, &txconf);
    if (ret < 0) {
      RTE_LOG(ERR, FLOWFWD, "port %u txq %u: setup failed: %s\n", port, q,
              rte_strerror(-ret));
      return false;
    }
  }

  ret = rte_eth_dev_start(port);
  if (ret < 0) {
    RTE_LOG(ERR, FLOWFWD, "port %u: start failed: %s\n", port,
            rte_strerror(-ret));
    return false;
  }

  ret = rte_eth_promiscuous_enable(port);
  if (ret != 0) {
    RTE_LOG(WARNING, FLOWFWD, "port %u: promiscuous enable failed: %s\n", port,
            rte_strerror(-ret));
  }

  struct rte_ether_addr mac;
  if (rte_eth_macaddr_get(port, &mac) == 0) {
    char buf[RTE_ETHER_ADDR_FMT_SIZE];
    rte_ether_format_addr(buf, sizeof(buf), &mac);
    RTE_LOG(INFO, FLOWFWD,
            "port %u (%s) started: MAC %s, %u queue(s), rx/tx desc %u/%u\n",
            port, rte_dev_name(info.device), buf, nb_queues, nb_rxd, nb_txd);
  }

  return true;
}

static void stop_port(uint16_t port) {
  rte_eth_dev_stop(port);
  rte_eth_dev_close(port);
}

static bool check_selection(const struct app_config *cfg) {
  if (rte_eth_dev_count_avail() < NB_PORTS) {
    RTE_LOG(ERR, FLOWFWD, "need %d ports, found %u\n", NB_PORTS,
            rte_eth_dev_count_avail());
    return false;
  }
  if (cfg->in_port == cfg->out_port ||
      !rte_eth_dev_is_valid_port(cfg->in_port) ||
      !rte_eth_dev_is_valid_port(cfg->out_port)) {
    RTE_LOG(ERR, FLOWFWD, "invalid port selection: input %u, output %u\n",
            cfg->in_port, cfg->out_port);
    return false;
  }
  return true;
}

// Buffers can be in the RX ring, in the TX ring, in the ring between the RX and
// the TX thread (one per port, as one per direction) and in a burst being
// handled. Every lcore also keeps a private cache: an RX and a TX thread per
// queue, plus the main lcore.
static bool create_mbuf_pool(const struct app_config *cfg) {
  const unsigned int nb_mbufs =
      NB_PORTS * cfg->nb_queues * (RX_DESC + TX_DESC + RING_SIZE + BURST_SIZE) +
      MBUF_CACHE * (2 * cfg->nb_queues + 1);

  mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL", nb_mbufs, MBUF_CACHE, 0,
                                      MBUF_DATA_LEN, (int)rte_socket_id());
  if (mbuf_pool == NULL) {
    RTE_LOG(ERR, FLOWFWD, "mbuf pool creation failed: %s\n",
            rte_strerror(rte_errno));
    return false;
  }
  RTE_LOG(INFO, FLOWFWD, "mbuf pool: %u mbufs\n", nb_mbufs);
  return true;
}

static bool log_port_roles(const struct app_config *cfg) {
  char in_name[RTE_ETH_NAME_MAX_LEN], out_name[RTE_ETH_NAME_MAX_LEN];

  if (rte_eth_dev_get_name_by_port(cfg->in_port, in_name) != 0 ||
      rte_eth_dev_get_name_by_port(cfg->out_port, out_name) != 0) {
    RTE_LOG(ERR, FLOWFWD, "cannot get the name of port %u or port %u\n",
            cfg->in_port, cfg->out_port);
    return false;
  }
  RTE_LOG(INFO, FLOWFWD, "input port %u (%s), output port %u (%s)\n",
          cfg->in_port, in_name, cfg->out_port, out_name);
  return true;
}

bool ports_start(const struct app_config *cfg) {
  if (!check_selection(cfg)) {
    return false;
  }

  rss_fill_key(rss_key, sizeof(rss_key), cfg->rss_pattern);

  if (!create_mbuf_pool(cfg)) {
    return false;
  }

  if (!start_port(cfg->in_port, cfg)) {
    RTE_LOG(ERR, FLOWFWD, "cannot initialise input port %u\n", cfg->in_port);
    return false;
  }
  if (!start_port(cfg->out_port, cfg)) {
    RTE_LOG(ERR, FLOWFWD, "cannot initialise output port %u\n", cfg->out_port);
    return false;
  }

  return log_port_roles(cfg);
}

void ports_stop(const struct app_config *cfg) {
  stop_port(cfg->in_port);
  stop_port(cfg->out_port);
}
