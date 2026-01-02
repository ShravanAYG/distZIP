# distZIP

**distZIP** is a **network-based distributed file compression system** designed to offload and parallelize compression workloads across multiple machines.

Instead of compressing files on a single node, distZIP uses a **client–server architecture** where compression jobs are **coordinated by a central server** and **executed by distributed clients** over a controlled network.

This enables faster compression of large datasets, efficient use of idle machines, and flexible deployment across isolated subnetworks.

---

## Architecture Overview

### Server
- Acts as the **job coordinator**
- Splits compression tasks into manageable units
- Assigns jobs to available clients
- Collects and assembles compressed output

### Client
- Runs as a **listener daemon**
- Waits for compression jobs assigned by the server
- Performs compression locally using `libzip`
- Sends results back to the server

> Clients do **not** push jobs.  
> They remain idle and **listen for work dispatched by the server**.

---

## Key Features

- **Distributed compression** across multiple nodes
- **Subnet-isolated operation**
  - Nodes can be restricted to specific subnetworks
  - Prevents accidental cross-network task execution
- **Client listener model**
  - Server-driven scheduling
- **YAML-based configuration**
  - Clear separation of server and client settings
- **Modular and extensible design**
- **Explicit network interface binding**
  - Avoids ambiguity on multi-NIC systems

---

## Configuration

distZIP is configured using YAML files to define network behavior, compression settings, and node roles.

### `server.yml`

```yaml
network:
  interface: enp4s0f3u1
  bind_address: 10.220.178.1
  subnet: 10.220.178.0/24

compression:
  format: zip
  chunk_size_mb: 64

scheduler:
  max_clients: 8
  retry_failed_jobs: true
```
### `client.yml`

```yaml
client:
  id: client-01
  listen_address: 10.220.178.157
  interface: enp4s0f3u1

server:
  address: 10.220.178.1
  port: 9090

compression:
  temp_dir: /tmp/distzip
```

## Building

Dependencies:
- `libzip`
- `make`
- `GCC` or `Clang` toolchain

Building:

```bash
make
make install
```

