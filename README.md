# ECE348 — Distributed Systems

![C](https://img.shields.io/badge/C-systems-blue)
![Python](https://img.shields.io/badge/Python-distributed%20runtime-green)
![Distributed Systems](https://img.shields.io/badge/Distributed-systems-purple)
![Coursework](https://img.shields.io/badge/UTH-ECE348-teal)

Coursework repository for **ECE348 — Distributed Systems** at the **University of Thessaly**. The assignments build distributed services and runtimes from UDP request/reply protocols to reliable group communication, a cached network file service, and a SimpleScript execution environment with transparent tuple-space operations and thread migration.

The repository keeps the implementations, tests, and benchmark scripts for the four assignments.

## Quick overview

| Area | What is included |
| --- | --- |
| Request/reply protocols | UDP client/server API with at-most-once semantics, fault tolerance, service discovery, persistent state recovery, and multi-server load balancing. |
| Group communication | C-based TCP group management service, reliable multicast layers, causal/total ordering via sequencers, membership updates, and coordinator crash-recovery. |
| Network file system | C client/server file API over UDP with remote open/read/write/truncate, client-side LRU block cache, staleness validation, and telemetry benchmarking. |
| Distributed runtime | SimpleScript parser/interpreter, distributed tuple space (`OUT`/`RD`/`IN`), TCP peer messaging, UDP discovery, live thread migration, and load balancing. |

## SimpleScript Distributed Runtime (Lab 4)

Assignment 4 implements a distributed runtime for the SimpleScript language. It behaves like a decentralized mobile-code execution environment: programs run as managed threads, block on tuple-space operations, and can migrate between runtime nodes across the network.

- **Language runtime:** Parses `.ss` programs and executes arithmetic, branching, and tuple operations.
- **Distributed tuple space:** Supports local and remote `OUT`, non-destructive `RD`, and destructive `IN` with request forwarding, registration, and notification packets over TCP.
- **Node discovery:** Uses UDP multicast (`239.0.0.1:10000`) so nodes can dynamically discover peers entering the cluster.
- **Live migration:** Serializes active interpreter thread state and migrates it to another runtime node to resume execution.
- **Automatic load balancing:** Nodes exchange `COUNT` metrics to monitor peer CPU load, migrating threads when imbalanced.
- **Deadlock detection:** Background schedulers detect circular wait dependencies across the cluster and warn the user.

## Course contents

| Path | Assignment | Implementation summary | Documentation |
| --- | --- | --- | --- |
| [`Lab1_ClientServer_RPC/`](Lab1_ClientServer_RPC/) | Request-reply with custom reliability | C server and Python client over UDP, featuring a 4-way handshake, caching, persistent binary state recovery, and dynamic load balancing. | [README](Lab1_ClientServer_RPC/README.md), [Handout](docs/handouts/distrsys_s26_Assignment1.pdf), [Report](docs/reports/Assignment1.pdf) |
| [`Lab2_Group_Membership/`](Lab2_Group_Membership/) | Reliable causal-total group communication | C-based TCP GMS server and UDP group messaging for reliable multicast, causal-total ordering (coordinator sequencer), and fault tolerance. | [README](Lab2_Group_Membership/README.md), [Handout](docs/handouts/distrsys_s26_Assignment2.pdf), [Report](docs/reports/Assignment2.pdf) |
| [`Lab3_NFS/`](Lab3_NFS/) | Network File System | C client/server exposing NFS operations over custom RPC, featuring LRU block caching, freshness validation, and extensive telemetry. | [README](Lab3_NFS/README.md), [Handout](docs/handouts/distrsys_s26_Assignment3.pdf) |
| [`Lab4_Distributed_Runtime/`](Lab4_Distributed_Runtime/) | Distributed execution environment | Python Tuple Space runtime with `IN`/`OUT`/`RD` atomicity, TCP peer messaging, thread migration, load balancing, and a custom script interpreter. | [README](Lab4_Distributed_Runtime/README.md), [Handout](docs/handouts/distrsys_s26_Assignment4.pdf), [Report](docs/reports/Assignment4.pdf) |

## Selected results & Telemetry

Benchmark metrics and scripts are kept within their respective assignments:
- [`Lab1_ClientServer_RPC/distrsys1.m`](Lab1_ClientServer_RPC/distrsys1.m) — MATLAB script plotting the performance of the primality testing workers.
- [`Lab3_NFS/plot_results.m`](Lab3_NFS/plot_results.m) — MATLAB telemetry analysis of NFS cache hit rates, throughput, and stale invalidations.

## Requirements

| Assignment | Main requirements |
| --- | --- |
| Project 1 | Linux/macOS, C compiler, `make`, Python 3 |
| Project 2 | Linux (requires `sys/timerfd.h`), C compiler, `make` |
| Project 3 | Linux/macOS, C compiler, `make` |
| Project 4 | Python 3 |

## Build and run

**Project 1:** Client/Server RPC
```bash
cd Lab1_ClientServer_RPC
make
./server 8080 store.bin 2
# In another terminal:
python3 client_app.py -r requests.txt
```

**Project 2:** Group Membership (Linux only)
```bash
cd Lab2_Group_Membership
make
./gms 10000
# In another terminal:
./member_app localhost 10000 member1 mygroup 5
```

**Project 3:** Network File System
```bash
cd Lab3_NFS
make
./server ./rootdir 9000
```

**Project 4:** Distributed Tuple Space
```bash
cd Lab4_Distributed_Runtime
# Start daemon 1
python3 runtime.py 10000
# Start daemon 2
python3 runtime.py 10001
# At daemon 1's interactive prompt, run a distributed script:
>> run tests/race_producer.ss
```
