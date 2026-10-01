# Distributed Tuple Space Runtime

A Python implementation of the **Tuple Space** distributed shared memory paradigm (originally popularized by Linda). This project provides a fully distributed, synchronized runtime environment where independent processes can communicate, share state, schedule tasks, and synchronize execution by reading and writing immutable tuples into a shared virtual space.

## Features

### 1. The Tuple Space Paradigm
Unlike traditional message passing (where Process A sends data to Process B), Tuple Space uses generative communication:
* Process A produces data and places a tuple `("job", 123)` into the space. Process A can then safely terminate.
* Process B later asks the space for a tuple matching `("job", @int)`. The space atomically hands the tuple to Process B.

### 2. Decentralized Peer-to-Peer Runtime
* **Auto-Discovery**: Runtimes use UDP Multicast (`239.0.0.1:10000`) to discover other nodes in the cluster.
* **Distributed Tuple Matching**: If a local runtime does not possess a requested tuple, it registers interest via a `REGISTER` packet to all peers. When another node spawns a matching tuple, it pushes a `NOTIFY` packet back to wake the blocked process.
* **Thread Migration**: Runtimes feature a Load Balancer thread that monitors peer CPU load via `COUNT` packets. Heavily loaded runtimes can serialize a running interpreter thread and `MIGRATE` it to an idle peer over TCP.

### 3. Distributed Race-Condition Prevention
* **Read-Only (`RD`)**: Safe for multiple nodes to read simultaneously; the `NOTIFY` message includes the actual tuple data directly.
* **Consume (`IN`)**: To prevent two threads from inadvertently consuming the same tuple simultaneously, the `NOTIFY` acts as a wake-up ping. The awakened thread must send a follow-up `QUERY` message to lock and extract the tuple atomically.

### 4. Deadlock Detection
A background scheduler loop checks for circular wait dependencies across the cluster. If the entire network is frozen (all threads on all peers are blocked waiting for tuples), the load balancer prints a `[DEADLOCK]` warning to the console.

---

## Distributed Networking Protocol

Nodes communicate using JSON payloads over TCP sockets, routing atomic Tuple Space operations across the cluster.

| Command | Description |
|---|---|
| `HELLO` / `HELLO_REPLY` | UDP Multicast discovery packets. |
| `REGISTER` | Node A tells Node B: "Wake me up if you ever see a tuple matching this pattern." |
| `NOTIFY` | Node B tells Node A: "I found a match for your pattern. Here it is (or ping for IN)." |
| `QUERY` | Node A asks Node B to extract and return a tuple matching a pattern. Used to pull tuples after a `NOTIFY`. |
| `MIGRATE` | Serializes an active interpreter thread state and sends it to another node to resume execution. |
| `COUNT` | Load balancing metric fetch. |
| `REMOTE_OUT` | Pushes a tuple directly into a remote runtime's local space (used during graceful shutdown to offload tuples). |

---

## API & SimpleScript

The `interpreter.py` parses `.ss` (SimpleScript) programs and executes them step-by-step against the remote Tuple Space daemon.

### Supported Operations

**Tuple space:**
* `OUT "string" 42 3.14`: Places a tuple into the space.
* `IN "string" @var_name 3.14`: **Blocking.** Searches the space for a matching tuple. If found, removes it from the space and binds the matched value into the local variable `@var_name` (accessed later as `$var_name`).
* `RD "string" @var_name 3.14`: **Blocking.** Same as `IN`, but reads the tuple without removing it from the space.

**Arithmetic & control flow:**
* `SET @var value`: Assigns a value (or resolved variable) to `@var` (accessed later as `$var`).
* `ADD` / `SUB` / `MUL` / `DIV` / `MOD @var val1 val2`: Arithmetic ops; result stored in `@var` (accessed later as `$var`).
* `BGT` / `BGE` / `BLT` / `BLE` / `BEQ val1 val2 #label`: Conditional branch to `#label`.
* `BRA #label`: Unconditional branch.

**Misc:**
* `SLP val`: Suspends the thread for `val` seconds.
* `PRN val1 val2 ...`: Prints the resolved values, space-separated.
* `RET`: Ends the thread.

*By leveraging blocking `IN` calls, the Tuple Space acts as a distributed lock/semaphore.*

## Build & Run

### 1. Start the Background Runtimes (Daemons)
Spawn as many of these as you want across different terminals (or machines on the same network). They will auto-discover each other.
```bash
python3 runtime.py <port>
```

### 2. Execute a Distributed Script
At a running daemon's interactive prompt, use the `run` command to load a `.ss` script as a new managed thread.
```bash
>> run tests/race_producer.ss
```
