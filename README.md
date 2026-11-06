# UDP File Transfer in C++
A socket programming project that explores application-level reliability over UDP through file chunking, sequence numbers, and selective retransmission.

Developed for **USC EE 542 — Laboratory 2: Fast, Reliable File Transfer**.

## Overview
The client sends a file to a server over IPv4 UDP port **9000**. The server tracks received chunks and requests missing sequence numbers using negative acknowledgments (NACKs). The project was designed to study large file transfers under different network delays, packet loss rates, bandwidth limits, and interface MTUs.
This repository preserves the original coursework implementation. It is an experimental prototype; completion messages alone do not establish file integrity. Always compare file hashes after a transfer.

## Features
- File metadata handshake before transmitting data.
- Sequence numbers and file offsets for reconstructing chunks received out of order.
- NACK messages that request retransmission of missing chunks.
- Interface MTU lookup with a fallback of 1500 bytes.
- Threaded packet transmission and reception.
- Elapsed transfer time printed by each program.

## Repository contents
| File | Purpose |
| --- | --- |
| `client.cpp` | Reads the source file, sends chunks, and handles retransmission requests. |
| `server.cpp` | Receives chunks, writes the output file, and requests missing data. |
| `Makefile` | Builds and cleans the client and server binaries. |
| `.gitignore` | Excludes compiled programs, transfer data, and local editor files. |
| `.gitattributes` | Keeps source files, documentation, and Makefile line endings consistent. |

## Requirements
- Linux, such as Ubuntu, or an Ubuntu environment in WSL2.
- GNU g++ with C++17 support and POSIX threads.
- Network connectivity between the client and server; UDP port 9000 must be reachable.
- GNU Make if using the supplied Makefile.
- Optional network experiments: `iproute2`, `iperf3`, and administrator privileges.

The code uses Linux APIs and GNU headers, including `SIOCGIFMTU`, `<endian.h>`, and `<bits/stdc++.h>`. It does not build directly in Windows CMD with the standard Microsoft C++ compiler. Git CMD can still be used to manage and upload this repository.

On Ubuntu:
```bash
sudo apt update
sudo apt install build-essential
```

## Configure the network
Check the server address and interface names on each Linux machine:
```bash
ip -br addr
ip link show
```
Edit these constants in `client.cpp`:
```cpp
const string SERVER_IP = "192.168.10.100";
const string ETH_INTERFACE = "ens33";
```
Edit this constant in `server.cpp`:
```cpp
const string ETH_INTERFACE = "ens33";
```
Set `SERVER_IP` to the server's reachable IPv4 address. Each `ETH_INTERFACE` must refer to the interface on that machine. Use matching MTUs: the server calculates its own chunk size rather than negotiating it with the client. Rebuild after changing the constants.

## Build
Run in the repository directory on Linux:
```bash
make
```
Or compile directly:
```bash
g++ -std=c++17 -O2 -pthread client.cpp -o client
g++ -std=c++17 -O2 -pthread server.cpp -o server
```
Remove compiled programs with `make clean`.

## Run a transfer
Start with a small file before trying a large transfer.

**1. Start the receiver on the server:**
```bash
./server outputfile.bin
```
Use a new output filename: the server truncates an existing file at that path when it receives file metadata.

**2. On the client, generate a 1 MiB sample and send it:**
```bash
dd if=/dev/urandom of=sample.bin bs=1M count=1
./client sample.bin
```

**3. Compare SHA-256 hashes on the respective machines:**
Client:
```bash
sha256sum sample.bin
```
Server:
```bash
sha256sum outputfile.bin
```
The complete hash strings must match. Hash verification is an external check; the transfer protocol does not exchange or verify a whole-file hash.
For a larger experiment, generate **1 GiB**:
```bash
dd if=/dev/urandom of=bigfile.bin bs=1M count=1024
./client bigfile.bin
```
The client holds the entire file in memory, plus chunk and thread overhead. Leave sufficient memory and disk space on both machines. Generated transfer files are excluded from Git.

## Protocol messages
| Message | Value | Purpose |
| --- | --- | --- |
| `FILE_INFO` | 1 | Sends the file size and source basename. |
| `FILE_DATA` | 2 | Carries a sequence number and file bytes. |
| `FILE_END` | 3 | Marks the end of the initial transmission. |
| `NACK` | 4 | Requests missing chunk sequence numbers. |
| `ACK_DONE` | 5 | Acknowledges metadata or signals the end of repair. |

Every message has a packed 16-byte header containing four 32-bit fields: `type`, `seq`, `size`, and `totalpkts`. Header fields use network byte order; the metadata file size uses big-endian encoding. The server's command-line argument determines the output path.

The client sends metadata and waits for an acknowledgment, transmits file chunks, and repeats the end marker. The server then requests missing chunks in batches. The client resends requested chunks until it receives a completion acknowledgment or times out.

## Network experiment plan
The accompanying coursework considered the following target conditions, with interface MTUs of **1500** and **9001** bytes:

| Scenario | Target RTT | Configured loss | Link rate |
| --- | --- | --- | --- |
| Low delay with loss | 10 ms | 1% | 100 Mbit/s |
| High delay with loss | 200 ms | 20% | 100 Mbit/s |
| Reduced bandwidth | 200 ms | 0% | 80 Mbit/s on constrained links |

Use Linux `tc`/`netem` on the appropriate interfaces in your lab topology. Apply configuration only to the intended test interfaces. One-way delay and loss settings accumulate along the path; setting loss independently on multiple interfaces changes the end-to-end loss probability. Jumbo MTUs also require support throughout the path.

Useful baseline commands (run separately from file-transfer measurements):
```bash
# On the server:
iperf3 -s

# On the client; replace the address if necessary:
iperf3 -c 192.168.10.100 -t 10
iperf3 -u -c 192.168.10.100 -b 100M -t 10
ping -c 10 192.168.10.100
```
No measured throughput or reliability results are asserted in this README. Record the actual configuration, file size, elapsed time, and matching hashes for each experiment.

## Known limitations
- **MTU calculation:** chunks use `MTU - 28` bytes, but the program adds a 16-byte application header afterward. Full data datagrams therefore exceed the interface MTU and can cause IP fragmentation or send failures, depending on socket and path behavior. NACK batches can also exceed the MTU.
- **Completion is not proof of success:** the server sends `ACK_DONE` after its bounded repair loop, even if chunks remain missing. The client can also exit successfully after a timeout. Verify hashes rather than relying on exit status or completion messages.
- **Concurrency:** the server creates detached threads for packet writes and does not join them before shutdown. Some receive-state accesses are unsynchronized. Heavy traffic may cause resource exhaustion or incorrect behavior.
- **Resource usage:** the client stores all chunks in RAM and creates many short-lived threads. This limits scalability.
- **Control messages:** metadata, end markers, and completion acknowledgments do not form a fully loss-tolerant session protocol; transfers may stall or finish incompletely.
- **Input handling:** malformed packets, empty files, file I/O failures, and invalid retransmission sequence numbers are not comprehensively handled.
- **Scope:** one transfer per server invocation, with no authentication, encryption, resume support, congestion control, or multi-client session isolation.

## Contributors
- Jacinta Rose Menezes
- Yichen Zheng
- Junjun Liu
