# flowfwd: a DPDK packet forwarder with per-flow tracking

`flowfwd` receives packets on one network interface, forwards them to another one, and keeps
statistics for every flow (TCP and UDP 5-tuple). Traffic is spread over several CPU cores with
RSS, and every flow stays on one core. Each core periodically appends its flows to its own CSV
file, and flows that stop sending are removed after a timeout.

It is written in C (gnu11) on top of DPDK and is meant to run on Linux with two NICs bound to
a DPDK driver.

## Contents

1. [How it works](#how-it-works)
2. [Dependencies](#dependencies)
3. [Build and run](#build-and-run)
4. [Configuration](#configuration)
5. [Flow tracking](#flow-tracking)
6. [Export format](#export-format)
7. [Expected output and how to verify it](#expected-output-and-how-to-verify-it)
8. [Tests](#tests)
9. [Design decisions and limitations](#design-decisions-and-limitations)
10. [Project layout](#project-layout)

## How it works

The program is built from independent lanes. Lane `q` owns queue `q` of both NICs.

```
          NIC 0 (input) RX queue q                 NIC 1 (output) RX queue q
                    |                                         |
                    v                                         v
        +-----------------------------------------------------------------+
        |  RX thread q  (one core)                                        |
        |  parse headers, drop non-IP, optional destination MAC rewrite,  |
        |  count the packet as received in flow table q                   |
        +-----------------------------------------------------------------+
                    |                                         |
             forward ring q                            reverse ring q
                    |                                         |
                    v                                         v
        +-----------------------------------------------------------------+
        |  TX thread q  (one core)                                        |
        |  send the packets, count them as sent in flow table q           |
        +-----------------------------------------------------------------+
                    |                                         |
                    v                                         v
          NIC 1 (output) TX queue q                 NIC 0 (input) TX queue q
```

- **RSS** (done by the NIC) hashes the 5-tuple of every packet and picks the queue. The RSS key
  repeats a 16-bit pattern (`0x6d5a`), which makes the hash symmetric: both directions of a
  connection get the same hash and reach the same queue. So a connection is always handled
  by one RX thread and one TX thread, and its flow entries are in one table.
- **RX thread `q`** polls queue `q` of both NICs, filters and counts the packets, and puts them
  in a ring: the forward ring for packets that came from the input NIC, the reverse ring for the
  others.
- **TX thread `q`** takes the packets from both rings, sends them out through the other NIC and
  counts them as sent.
- **Rings** have exactly one producer and one consumer, so they need no locks. They also keep
  the order of the packets of a flow.
- **Flow table `q`** is changed only by the RX thread of its lane. The TX thread adds only to
  the "sent" counters, which are kept in a separate array. So the datapath uses no locks.
- **Export writer thread** writes the CSV files, so a forwarding thread never waits for the disk.
- **Logger thread** writes `log.txt`, for the same reason.
- The **main thread** starts everything and prints one statistics line per second.

A lane needs two cores, so `queues = N` needs `2 * N + 1` cores (the `+ 1` is the main core).
The cores are taken in pairs from the EAL `-l` list: the first of each pair is the RX thread,
the second one is the TX thread.

## Dependencies

| Needed | Notes |
|---|---|
| DPDK 23.11 (`libdpdk`, found with `pkg-config`) | developed and tested with 23.11.4 (Ubuntu 24.04 packages `dpdk`, `dpdk-dev`) |
| gcc with C11/gnu11, `make`, `pkg-config` | the build treats all warnings as errors |
| Hugepages | 2 MB pages; `run.sh` reserves 2 GB with `dpdk-hugepages.py` |
| Two NICs bound to `vfio-pci`, `uio_pci_generic` or `igb_uio` | `dpdk-devbind.py` (part of the DPDK package) does the binding |
| `sudo` | DPDK needs root (hugepages, device access) |
| Optional: `tshark` | to inspect the pcap files in the virtual-device test |

Developed on an AMD Ryzen 7 7800X3D with an Intel X540-AT2 (two 10 GbE ports,
`uio_pci_generic`).

```
sudo apt install dpdk dpdk-dev build-essential pkg-config
```

## Build and run

```
./setup.sh      # build, run the unit tests, copy flowfwd.conf next to the binary
./run.sh        # reserve hugepages, bind the two NICs, run flowfwd as root
```

`setup.sh` produces `build/flowfwd`. The binary reads `flowfwd.conf` from its own directory.

`run.sh` is written for this machine: the two PCI addresses of the NICs are in the `NICS`
variable at the top, and the driver is `uio_pci_generic`. Edit them for another machine. Extra
arguments are passed to DPDK (EAL options). Without arguments it runs with `-l 0-8`, which is
enough for the default `queues = 4`. `run.sh` starts the program in the directory of the binary,
so `log.txt` and the CSV files appear in `build/`.

To run by hand:

```
sudo ./build/flowfwd -l 0-8          # the main core 0, plus 8 cores for 4 queues
```

Stop with Ctrl+C. The program stops the receive threads, sends what is still in the rings,
writes a final export and closes everything.

`make` builds only, `make test` runs the unit tests, `make clean` removes `build/`,
`make V=1` shows the full compiler commands.

### Which ports are input and output

The startup log lists the PCI devices and the DPDK port ids, in PCI address order. `in_port`
and `out_port` in the config select which of them is the input and the output. With the
default ids 0 and 1 and the two X540 ports, `0000:09:00.0` is the input and `0000:09:00.1` is
the output.

### Running without hardware

Virtual devices work for functional tests. The pcap driver has no RSS, so use `queues = 1`:

```
printf "[ports]\nqueues = 1\n[export]\ninterval = 2\n" > build/flowfwd.conf
cd build
./flowfwd -l 0-2 --no-huge -m 512 --no-pci \
  --vdev "net_pcap0,rx_pcap=in0.pcap,tx_pcap=out0.pcap" \
  --vdev "net_pcap1,rx_pcap=in1.pcap,tx_pcap=out1.pcap"
```

Each `rx_pcap` file is played into the input of that port, and the forwarded packets are written
to the `tx_pcap` file of the other one (`tshark -r out1.pcap` shows them). The `net_null`
driver can be used to check start-up and shutdown with several queues.

## Configuration

`flowfwd.conf` is an INI file. Unknown sections or keys and out-of-range values stop the program
at start with the file name and line number.

| Section | Key | Default | Meaning |
|---|---|---|---|
| `[ports]` | `in_port`, `out_port` | 0, 1 | DPDK port ids of the input and the output interface |
| `[ports]` | `queues` | 1 (the file shipped here sets 4) | Queues per NIC, 1 to 16. Each queue needs two cores. |
| `[rss]` | `pattern` | `0x6d5a` | 16-bit pattern repeated to build the RSS key. `0x0000` and `0xffff` are refused. |
| `[logging]` | `level` | 2 | 0 error, 1 warning, 2 info, 3 debug |
| `[logging]` | `console` | 1 | 1 also prints log lines on the terminal |
| `[logging]` | `file` | `log.txt` | Log file (appended). A relative path starts in the directory the program runs from. |
| `[export]` | `interval` | 10 | Seconds between exports, 1 to 3600 |
| `[export]` | `directory` | `.` | Directory of the CSV files |
| `[filter]` | `pass_arp` | 1 | 1 forwards ARP frames (untracked), 0 drops them like any other non-IP frame |
| `[flows]` | `timeout` | 60 | Seconds without packets before a flow is removed. Must be larger than `[export] interval`. |
| `[flows]` | `cleanup_interval` | 10 | Seconds between runs of the cleaner |
| `[flows]` | `table_size` | 65536 | Flows each queue can track (8 to 1048576) |
| `[rewrite]` | `forward_dst_mac` | unchanged | Destination MAC written into IP packets going from input to output |
| `[rewrite]` | `reverse_dst_mac` | unchanged | The same for output to input |

## Flow tracking

- **Flow key:** source IP, destination IP, source port, destination port and protocol (and the IP
  version). IPv4 and IPv6 are both supported.
- **What is tracked:** TCP and UDP packets that carry ports. Other IP traffic (for example
  ICMP) is forwarded but not tracked. Non-first IP fragments have no ports, so they are
  forwarded and not tracked.
- **Each direction is its own flow.** A TCP connection appears as two flows: client to server
  and server to client. Both are in the same table, handled by the same thread.
- **Counters per flow:** `rx_packets` and `rx_bytes` are counted when the RX thread receives the
  packet. `tx_packets` and `tx_bytes` are counted by the TX thread after the NIC accepts the
  packet. A packet dropped before sending is received but never sent. Bytes are the length of
  the frame on the wire.
- **Real time:** the counters are updated per burst of up to 32 packets, as the packets pass.
- **Timeout and cleanup:** every `cleanup_interval` seconds the cleaner removes the flows that had
  no packets for `timeout` seconds. TCP flags are not interpreted: a FIN or RST is counted and
  forwarded like any other packet, and the flow disappears after the timeout. The timeout is
  longer than the export interval so that the final counters of a flow are always exported
  before it is removed.
- **Capacity:** each table holds `table_size` flows. When a table is full, packets of flows that
  are not in it are dropped and counted in `drops=`, while the packets of known flows keep
  flowing. The flag clears when the cleaner frees room.
- **Affinity:** guaranteed by the symmetric RSS key (the same queue for both directions of a
  connection) and one private table per lane.

## Export format

Every RX thread appends to its own file, named after its core: `flow_stats_core_<lcore>.csv` (in
`[export] directory`). The header is written once, when the file is empty. Every `interval`
seconds one row per flow is appended, all rows of one export with the same timestamp, so the
same flow has one row per interval. A final export is written when the program stops.

```
timestamp,src_ip,dst_ip,src_port,dst_port,protocol,rx_bytes,tx_bytes,rx_packets,tx_packets
1792417550.976,10.0.0.2,10.0.0.1,80,40000,6,4216,4216,4,4
1792417550.976,10.0.0.1,10.0.0.2,5353,53,17,216,216,3,3
1792417550.976,10.0.0.1,10.0.0.2,40000,80,6,770,770,5,5
```

- `timestamp`: Unix time in seconds with milliseconds.
- `protocol`: the IP protocol number (6 is TCP, 17 is UDP).
- The counters are totals since the flow was created, not per interval.

## Expected output and how to verify it

A short run with the virtual pcap ports above (5 TCP and 3 UDP packets in the forward direction,
4 TCP packets in the reverse direction):

```
FLOWFWD: logger started: file log.txt, level 2
FLOWFWD: config .../flowfwd.conf: queues=1 in_port=0 out_port=1 rss_pattern=0x6d5a export_interval=2s pass_arp=1 flow_timeout=5s cleanup_interval=10s table_size=65536
FLOWFWD: mbuf pool: 6976 mbufs
FLOWFWD: port 0 (net_pcap0) started: MAC 02:70:63:61:70:00, 1 queue(s), rx/tx desc 1024/1024
FLOWFWD: port 1 (net_pcap1) started: MAC 02:70:63:61:70:01, 1 queue(s), rx/tx desc 1024/1024
FLOWFWD: input port 0 (net_pcap0), output port 1 (net_pcap1)
FLOWFWD: lcore 1: flow statistics go to ./flow_stats_core_1.csv every 2s
FLOWFWD: destination MAC of IP packets: input -> output unchanged, output -> input unchanged
FLOWFWD: queue 0: RX thread on lcore 1, TX thread on lcore 2
FLOWFWD: forwarding, press Ctrl+C to stop
FLOWFWD: rx=12 tx=12 drops=0 flows=3 load(rx/tx)=0%/0%
FLOWFWD: stopped: rx=12 tx=12 drops=0 flows=3
```

The statistics line, printed every second:

| Field | Meaning |
|---|---|
| `rx`, `tx` | Packets received from the interfaces and packets sent out, summed over all queues |
| `drops` | Packets lost: dropped by the NICs (receive queue full, no buffers), dropped because a flow table was full, because a ring was full, or because the NIC did not accept them for sending |
| `flows` | Flows currently in the tables |
| `load(rx/tx)` | For each queue, the share of time its RX and TX threads spent handling packets. Near 100% means that thread is about to fall behind: use more queues. |

`rx` is larger than `tx` by the number of packets that were filtered out on purpose (non-IP
frames) plus the dropped ones.

The log file has one line per message with a timestamp and the core that logged it:

```
2026-10-19 17:45:48.961 [lcore 0] FLOWFWD: logger started: file log.txt, level 2
```

**To check that it works**
1. The startup log shows both ports started with the number of queues you configured, and one
   `queue N: RX thread on lcore X, TX thread on lcore Y` line per queue.
2. With traffic flowing, `rx` and `tx` grow together and `drops` stays at or near zero.
3. Each CSV file receives rows every `interval` seconds, and the counters of a flow grow.
   The `rx_packets` of the flows add up to the `rx` of the statistics line (minus the
   untracked packets), and `tx_packets` add up to `tx` in the same way.
4. With several queues, the `load(rx/tx)` values of the queues are of the same order, which
   shows that RSS spreads the flows.
5. Ctrl+C prints `stopped: ...` and returns to the shell without hanging.

## Tests

`make test` (also run by `setup.sh`) builds and runs three small programs that need no DPDK:

- `tests/test_parse.c`: the packet parser (Ethernet, VLAN and double VLAN, IPv4 with options,
  IPv6, ports of TCP and UDP, fragments, ARP, malformed frames).
- `tests/test_export.c`: the CSV writer (header, rows, IPv4 and IPv6 addresses, errors).
- `tests/test_rss.c`: the RSS key, including the symmetry of the hash when source and
  destination are swapped.

The forwarding path was also checked with virtual devices: pcap replays with exact expected
counters (including UDP, ICMP, both directions and a full table), a load of 200,000 packets
in 1,000 flows where the CSV totals match the statistics line, and start and stop with several
queues. This repository does not include throughput measurements on real hardware.

## Design decisions and limitations

**Decisions**
- Packets of one flow stay on one lane, so the flow table needs no locks.
- The RSS key repeats `0x6d5a` to make the hash symmetric. This works on NICs that accept a
  custom key (the Intel X540 does). If a NIC cannot take a key, the program prints a warning
  because the two directions of a flow may then reach different queues.
- Receive and send are separate threads connected by lock-free rings (the optional
  multi-core bonus). The order of the packets inside a flow is preserved.
- Nothing slow runs on a forwarding thread: files are written by the export writer thread and
  the log by the logger thread. Counters have a single writer each, and the statistics of
  each thread sit in their own cache lines.
- Packets are handled in bursts of 32 and their headers are prefetched, so the work per packet
  is small.
- `pass_arp = 1` by default: ARP is not IP, but if it were dropped, the hosts on both sides of
  the forwarder could not find each other's MAC addresses. Set `pass_arp = 0` to drop every
  non-IP frame.
- Promiscuous mode is always on, because a transparent forwarder must accept frames that are
  not addressed to its own MAC.

**Limitations**
- Only TCP and UDP flows are tracked (see above). TCP flags are not interpreted, so a closed
  connection stays in the table until the timeout.
- The per-flow `tx` counters are written by the TX thread. A generation number per table slot
  protects them when a slot is given to a new flow, but a stalled TX thread (stopped for as
  long as the flow timeout) could still count a few sent packets in the wrong flow.
- At most 16 queues. Packets larger than a standard 1500-byte MTU frame are not supported
  (no jumbo or multi-segment frames).
- IPv6 extension headers are not walked: ports are read only when TCP or UDP follows the fixed
  IPv6 header directly.
- The NIC check at start-up recognises the drivers `vfio-pci`, `uio_pci_generic` and `igb_uio`.
  Cards that work without being rebound (for example Mellanox) are not recognised by this check.
- The program needs `1 + 2 * queues` cores.

## Project layout

```
src/main.c          start-up, the one-second statistics loop, shutdown
lib/config.*        flowfwd.conf loader and validation
lib/hugepages.*     start-up check of the hugepages, clean-up of leftover files
lib/nics.*          start-up check that two NICs are bound to a DPDK driver
lib/ports.*         port and queue setup, RSS key, mbuf pool
lib/rss.*           builds the symmetric RSS key
lib/parse.*         Ethernet, VLAN, IPv4, IPv6, TCP and UDP header parser
lib/flow_table.*    per-queue flow table: filter, tracking, cleaner, export iterator
lib/forward.*       RX and TX threads, rings, statistics, shutdown order
lib/flow_export.*   export slots and the writer thread
lib/export.*        CSV writer
lib/logger.*        asynchronous logger
lib/ip_addr.h       IPv4 or IPv6 address type
tests/              unit tests
flowfwd.conf        configuration
setup.sh, run.sh    build and run scripts
```
