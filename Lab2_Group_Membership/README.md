# Reliable Causal-Total Group Communication

A distributed library implemented in C that provides **Group Membership Management (GMS)** and **Reliable Causal-Total Multicast**. This project ensures that messages are delivered in identical order across nodes in a distributed system, handling node crashes, packet loss, duplicate packets, and dynamic topologies.

## Features

### 1. Group Membership System (GMS)
* **Centralized Coordination**: A dedicated TCP/IP GMS server manages cluster state.
* **Dynamic Topologies**: Nodes can `grp_join` or `grp_leave` multiple isolated groups concurrently.
* **View Synchronization**: When membership changes, the GMS pushes the new global view down to all nodes, maintaining consensus on active members and tracking unique PIDs for causal routing.

### 2. Sequencer-Based Causal-Total Multicast
* **P2P UDP Messaging**: Message payload propagation uses UDP unicasts.
* **Coordinated Ordering**: One node acts as the Coordinator. Upon receiving a `MSG`, the coordinator assigns it a strict sequence number and broadcasts a `DELIVER_NR` packet.
* **Delivery Guarantees**: Nodes buffer incoming `MSG` payloads until they receive the matching `DELIVER_NR`, ensuring absolute total ordering.
* **Reliability Layer**: Sender nodes track an `acked_by` array for every active member. Background retransmission threads continuously resend un-ACKed messages until full delivery is confirmed.

### 3. Fault Tolerance & Crash Recovery
* **Liveness Checking**: The GMS runs a `timerfd` heartbeat loop. Dead nodes are purged and a new view is published.
* **Coordinator Handoff**: If the group coordinator crashes, the next alive member takes over. The cluster enters an `is_recovering` state where the new coordinator pulls `COORD_STATE_REQ/REPLY` from all members to reconstruct the highest delivered sequence number before resuming normal traffic.

---

## Architecture & Protocols

### GMS TCP Packets
| Constant | Direction | Description |
|---|---|---|
| `MSG_JOIN` / `_OK` / `_FAIL` | Both | Node requests to join a group. Server replies with a unique PID. |
| `MSG_LEAVE` / `_OK` | Both | Node requests graceful exit. |
| `MSG_VIEW` | Server → Node | Broadcasts the updated group topology and active PIDs. |
| `MSG_PING` / `MSG_PONG` | Both | Server liveness checking. |

### P2P UDP Packets
| Constant | Direction | Description |
|---|---|---|
| `MSG` | Sender → All | The raw application payload. Buffered by receivers. |
| `MSG_ACK` | Receiver → Sender | Confirms payload receipt to stop payload retransmission. |
| `DELIVER_NR` | Coord → All | Binds a globally unique sequence number to a `MSG` ID. |
| `DELIVER_NR_ACK` | Receiver → Coord | Confirms receipt of the sequencer number. |
| `COORD_STATE_REQ/REPLY` | New Coord ↔ All | State synchronization during crash recovery handoff. |
| `RECOVERY_DONE` | New Coord → All | Unblocks the cluster to resume traffic. |

---

## API Structure

```c
// Binds the node to the TCP GMS server and starts the UDP background listener
int grp_init(char *gms_addr, int gms_port);

// Contacts the GMS, acquires a unique PID, and synchronizes the global view
int grp_join(char *grpname, char *myid);

// Safely leaves the cluster, triggering a view update to all other members
int grp_leave(int g);

// Pushes a payload to all members in causal-total order via UDP
int grp_send(int g, void *msg, int len);

// Blocks and fetches the next globally ordered message from the delivery queue
int grp_recv(int g, void *msg, int *len, int block);
```

## Build & Run

### 1. Compile
```bash
make
```

### 2. Start the GMS Server
```bash
./gms <port>
```

### 3. Run the Demonstration App
```bash
./member_app <gms_ip> <gms_port> <my_id> <group_name> <msgs_to_send>
```
