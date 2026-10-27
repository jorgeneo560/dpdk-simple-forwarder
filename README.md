# flowfwd: a DPDK packet forwarder with per-flow tracking

`flowfwd` receives packets on one network interface, forwards them to another one, and keeps
statistics for every flow (TCP and UDP 5-tuple). Traffic is spread over several CPU cores with
RSS, and every flow stays on one core. Each worker core periodically appends the flows of its
queue to its own CSV file, and flows that stop sending are removed after a timeout.

It is written in C on top of DPDK and is meant to run on Linux with two NICs bound to
a DPDK driver.

> **The best way to see how the project is built is the Doxygen documentation**: it has the
> architecture diagrams, the life of a packet and the description of every file and function in
> one browsable place. See [Code documentation](#code-documentation): the generated HTML is in
> `docs/html/`, so after cloning you can open `docs/html/index.html` directly.

## Contents

1. [How it works](#how-it-works)
2. [Dependencies](#dependencies)
3. [Build and run](#build-and-run)
4. [Configuration](#configuration)
5. [Flow tracking](#flow-tracking)
6. [Export format](#export-format)
7. [Expected output and how to verify it](#expected-output-and-how-to-verify-it)
8. [Performance](#performance)
9. [Verification](#verification)
10. [Design decisions and limitations](#design-decisions-and-limitations)
11. [Project layout](#project-layout)

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
            in_to_out_ring q                          out_to_in_ring q
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
  in a ring: the in-to-out ring for packets that came from the input NIC, the out-to-in ring for the
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
| Optional: `doxygen`, `graphviz` | to build the HTML code documentation (see [Code documentation](#code-documentation)) |

Developed on an AMD Ryzen 7 7800X3D with an Intel X540-AT2 (two 10 GbE ports,
`uio_pci_generic`).

```
sudo apt install dpdk dpdk-dev build-essential pkg-config
```

## Build and run

```
./setup.sh      # build, copy flowfwd.conf next to the binary
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

`make` builds only, `make clean` removes `build/`,
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

### Code documentation

The sources are commented for Doxygen, and the generated HTML is the best way to read the
project: the diagrams are drawn, and every file, function and structure is linked. The generated
HTML is stored in the repository (`docs/html/`), so it can be opened right after cloning. To
build it again:

```
sudo apt install doxygen graphviz
doxygen Doxyfile
```

Open `docs/html/index.html`. It contains this README, an architecture page (lanes and threads,
the life of a packet as a sequence diagram, what each source file does and its functions) and
the reference of every file, function and structure. The HTML in `docs/` is generated from the
sources and `doc/`, so run `doxygen Doxyfile` again after changing them. The diagrams are also
kept as Mermaid sources in `doc/*.mmd`.

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
| `[export]` | `interval` | 1 | Seconds between exports, 1 to 3600 |
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

Every worker core has its own file, named after its core: `flow_stats_core_<lcore>.csv` (in
`[export] directory`). A queue is served by two cores, an RX core and a TX core, so it has two
files. The RX core owns the flow table of the queue and takes the snapshots; the TX core only
adds its `tx` counters to the flows. Both files of a queue hold the same rows, so for totals
count the files of the RX cores or the files of the TX cores, not both. The header is written
once, when the file is empty. Every `interval` seconds one row per flow is appended, all rows
of one export with the same timestamp, so the same flow has one row per interval. A final
export is written when the program stops.

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

Example. A short run with a few TCP and UDP packets in each direction prints this:

```
FLOWFWD: logger started: file log.txt, level 2
FLOWFWD: config .../flowfwd.conf: queues=1 in_port=0 out_port=1 rss_pattern=0x6d5a export_interval=2s pass_arp=1 flow_timeout=5s cleanup_interval=10s table_size=65536
FLOWFWD: mbuf pool: 6976 mbufs
FLOWFWD: port 0 (net_pcap0) started: MAC 02:70:63:61:70:00, 1 queue(s), rx/tx desc 1024/1024
FLOWFWD: port 1 (net_pcap1) started: MAC 02:70:63:61:70:01, 1 queue(s), rx/tx desc 1024/1024
FLOWFWD: input port 0 (net_pcap0), output port 1 (net_pcap1)
FLOWFWD: lcore 1: flow statistics go to ./flow_stats_core_1.csv every 2s
FLOWFWD: lcore 2: flow statistics go to ./flow_stats_core_2.csv every 2s
FLOWFWD: destination MAC of IP packets: input -> output unchanged, output -> input unchanged
FLOWFWD: queue 0: RX thread on lcore 1, TX thread on lcore 2
FLOWFWD: forwarding, press Ctrl+C to stop
FLOWFWD: rx=12 tx=12 drops=0 flows=3 load(rx/tx)=0%/0%
FLOWFWD: queue 0: RX lcore 1 rx=12 table_drops=0 ring_drops=0 | TX lcore 2 tx=12 tx_drops=0
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
   untracked packets), and `tx_packets` add up to `tx` in the same way (using one file per
   queue, see the export format). When the program stops, one line per queue shows what its
   RX thread and its TX thread did.
4. With several queues, the `load(rx/tx)` values of the queues are of the same order, which
   shows that RSS spreads the flows.
5. Ctrl+C prints `stopped: ...` and returns to the shell without hanging.

## Performance

The measurement below uses real traffic on real hardware.

**Setup.** AMD Ryzen 7 7800X3D, two Intel X540 ports (both links negotiated at 1 Gbit/s),
DPDK 23.11.4, default configuration with `queues = 4` (9 cores) and no MAC rewrite. The forwarder
was placed between a router and the onboard NIC of the test PC, and the PC itself generated the
traffic: web browsing, a YouTube video and the download of a Linux ISO. The run lasted 156 s.

| Result | Value |
|---|---|
| Packets received / sent | 5,003,596 / 5,003,555 |
| Packets dropped (`drops=`) | 0 |
| Data in tracked flows | 6.90 GB |
| Distinct 5-tuples seen | 1,899 (854 still in the table at the end) |
| Peak rate (5 s average) | 978 Mbit/s |
| Rate during the ISO download | about 825 Mbit/s on average between 70 s and 115 s (600 to 978 in single 5 s windows) |
| Peak packet rate (one second, statistics line) | 84,732 packets/s |
| Thread load during the ISO download (statistics line) | busiest RX thread 1.8% on average, 3% at most; its TX thread 1% at most; the other six threads 0% |

**Packet sizes.** Flows grouped by their average packet size (`rx_bytes / rx_packets`), as
measured from the CSV files:

| Average packet size | Flows | Packets | Share of bytes |
|---|---|---|---|
| below 100 B | 603 | 362,109 | 0.4% |
| 100 to 299 B | 908 | 10,725 | 0.0% |
| 300 to 799 B | 325 | 14,683 | 0.1% |
| 800 to 1299 B | 62 | 30,722 | 0.6% |
| 1300 B and above | 17 | 4,584,841 | 98.9% |

The ISO download is one TCP flow of 6.80 GB with an average data frame of 1,488 B (Ethernet
header included); its ACKs, in the other direction, average 83 B. The QUIC video flows average
1,270 to 1,320 B.

**What the numbers show**
- No packet was lost: `drops=0`, and for every tracked flow the received and sent counters
  are equal (5,003,080 packets in both directions).
- `rx` is 41 packets higher than `tx`: these are frames the filter removed on purpose
  (not IP or malformed), which are not counted in `drops=`.
- Flow affinity: both directions of the ISO download were handled by one queue (RX core 5, TX
  core 6). That queue carried 4.94 million of the 5.00 million packets, the other three
  7,911 to 42,384 each (the per-queue summary at the end of the log). A single flow cannot use
  more than one core, by design.
- Export: every RX core wrote its record set once per second for the whole run (152 to 154
  exports in 156 s, at most 1.06 s apart), and the file of each TX core is identical to the
  file of its RX core (checked with `cmp`).

**Limits of this measurement.** The peak of 978 Mbit/s is close to the line rate of the 1 Gbit/s
link, so the table shows that the forwarder keeps up with the link without loss, not the
maximum rate of the program: at that rate the busiest thread spent about 3% of its time
handling packets (the `load(rx/tx)` values are in `log.txt`, one line per second; a thread
counts as busy in every pass of its loop that handled at least one packet). How to repeat a
measurement: run `./run.sh`, generate traffic, stop with Ctrl+C, and add up the per-flow counter
changes between consecutive exports in the CSV files of the RX cores.

The log of this run (181 lines, one statistics line per second) is in
[doc/performance_run.log](doc/performance_run.log), copied unchanged from `build/log.txt`.
The eight CSV files of the run are in [doc/performance_run/](doc/performance_run/), compressed
with gzip (`zcat doc/performance_run/flow_stats_core_5.csv.gz | head` shows one). The two
files of each queue are identical (1 and 2, 3 and 4, 5 and 6, 7 and 8).

## Verification

The forwarding path was also checked with virtual devices: pcap replays with exact expected
counters (including UDP, ICMP, both directions and a full table), a load of 200,000 packets
in 1,000 flows where the CSV totals match the statistics line, and start and stop with several
queues. The real-hardware measurement is in [Performance](#performance).

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
lib/writer.*        CSV file writer
lib/logger.*        asynchronous logger
lib/ip_addr.h       IPv4 or IPv6 address type
flowfwd.conf        configuration
setup.sh, run.sh    build and run scripts
Doxyfile, doc/      Doxygen configuration, architecture pages and diagram sources
```
