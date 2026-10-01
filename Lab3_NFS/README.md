# Network File System (NFS)

A caching Network File System built in C. This project implements a remote file storage architecture utilizing client-side validation and LRU block caching to reduce network latency and increase throughput.

## Features

### 1. Caching & Validation
To reduce network bottlenecks, the client implements a block-based cache:
* **LRU Eviction**: Maintains a fixed number of cache blocks. When full, evicts the Least Recently Used block using a logical clock.
* **Freshness Timers (`freshT`)**: Cached blocks are considered unconditionally valid if they were fetched within the `freshT` window, requiring no network round-trips.
* **Staleness Revalidation**: If a block expires, the client sends a metadata fetch (`tmod`). If the server's modified timestamp matches the cache's timestamp, the client re-uses the cached payload instead of re-downloading the data.

### 2. File State Management
* **File Tables**: The server manages concurrent access using a global `file_table`, mapping `fid` to physical filesystem paths.
* **Client Tables**: The server tracks client sessions via `client_table`, enforcing security and maintaining state.

### 3. Experimental Telemetry & Benchmarking
The client includes a built-in telemetry engine (`mynfs_stats`) to track cache hits, misses, network bytes, and RPC calls. Performance was evaluated using a 10MB video file payload across different `BlockSize`, `CacheBlocks`, and `Freshness` parameters. The results were logged and plotted using the included `plot_results.m` MATLAB script.

---

## Packet Format

All packets share a fixed binary header structure over the wire.

### Request Header (`req_header`)
| Field | Size | Description |
|---|---|---|
| `type` | 2 bytes (short) | Command opcode: `OP_OPEN` (0), `OP_READ` (1), `OP_WRITE` (2), `OP_TRUNCATE` (3) |
| `fid` | 8 bytes (uint64) | File Identifier provided by the server. |
| `offset` | 8 bytes (off_t) | File seek offset. |
| `length` | 8 bytes (off_t) | For READ: Bytes to read. For WRITE: Payload size. |
| `tmod` | 8 bytes (time_t) | For READ: `-1` = cold miss, `>= 0` = cached `mtime` for conditional fetch. |

### Response Header (`resp_header`)
| Field | Size | Description |
|---|---|---|
| `type` | 2 bytes (short) | Command opcode (echoed back). |
| `status` | 2 bytes (short) | `ERR_OK` (0), `ERR_BADFID`, `ERR_NOENT`, `ERR_IO`, `ERR_NOTMODIFIED` |
| `fid` | 8 bytes (uint64) | File Identifier (assigned during `OP_OPEN`). |
| `length` | 8 bytes (off_t) | Payload size attached to this response (e.g., READ data). |
| `file_size` | 8 bytes (off_t) | Total physical file size on the server. |
| `tmod` | 8 bytes (time_t) | The current modified timestamp of the file. |

---

## API Structure

```c
// Connects to the NFS Server and allocates LRU cache blocks.
int mynfs_init(char *server_addr, int server_port, int blockSize, int maxCachedBlocks, int freshT);

int  mynfs_open(char *fname);
long mynfs_read(int fd, void *buf, long n);
long mynfs_write(int fd, void *buf, long n);
long mynfs_lseek(int fd, long pos, int whence);
int  mynfs_truncate(int fd, long len);
int  mynfs_close(int fd);
```

## Build & Run

### 1. Compile
```bash
make
```

### 2. Start the Server
```bash
./server <root_dir> [port]
```

### 3. Run the Benchmarks
```bash
./tests/test_freshness_bench <server_ip> <port> <file> <fresh_t> <max_cached> <num_passes>
```
