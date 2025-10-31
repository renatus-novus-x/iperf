[“ú–{Œê](./README.ja.md)

[![windows](https://github.com/renatus-novus-x/iperf/workflows/windows/badge.svg)](https://github.com/renatus-novus-x/iperf/actions?query=workflow%3Awindows)
[![macos](https://github.com/renatus-novus-x/iperf/workflows/macos/badge.svg)](https://github.com/renatus-novus-x/iperf/actions?query=workflow%3Amacos)
[![ubuntu](https://github.com/renatus-novus-x/iperf/workflows/ubuntu/badge.svg)](https://github.com/renatus-novus-x/iperf/actions?query=workflow%3Aubuntu)

<img src="https://raw.githubusercontent.com/renatus-novus-x/iperf/main/images/tether.png" title="tether" />

# iperf
   Minimal single-thread TCP throughput tester (client/server)
   Cross-platform: Windows / macOS / Linux / X68000
   - Server: receive and discard, print per-second and total rates
   - Client: send fixed-size chunks for given seconds, print per-second and total rates
## Download
- [iperf.exe (windows)](https://raw.githubusercontent.com/renatus-novus-x/iperf/main/bin/iperf.exe)
- [iperf (macos)](https://raw.githubusercontent.com/renatus-novus-x/iperf/main/bin/iperf)
- [iperf.x (x68000)](https://raw.githubusercontent.com/renatus-novus-x/iperf/main/bin/iperf.x)

## Example Workflow (LAN test)

### 1. Start the Server (Receiver)

On the **server machine** (Windows / Mac / Linux / X68000):

```bash
./iperf s 5201
```

Expected output:

```
[server] listening on port 5201 ...
[server] local=192.168.1.23:5201  remote=192.168.1.45:53422
[server] 0-1s: 752640 bytes  6.02 Mb/s (0.75 MB/s)
[server] TOTAL: 7532800 bytes in 10.00s  6.02 Mb/s (0.75 MB/s)
```

---

### 2. Start the Client (Sender)

On the **client machine** (e.g. Windows / Mac / Linux / X68000):

```bash
./iperf c <server-ip> 5201 10 64
```

- `<server-ip>` : IP address of the server (e.g. `192.168.1.23`)  
- `5201` : port number (must match the server)  
- `10` : test duration in seconds  
- `64` : buffer size in KB per send

Example:

```bash
./iperf c 192.168.1.23 5201 10 64
```

Expected output:

```
[client] connect 192.168.1.23:5201 ...
[client] seconds=10  buf=64KB  (single TCP stream, IPv4)
[client] 0-1s: 654321 bytes  5.23 Mb/s (0.65 MB/s)
[client] TOTAL: 6532100 bytes in 10.00s  5.23 Mb/s (0.65 MB/s)
```

---

### 3. Verify Throughput

Both server and client will display per-second and total throughput in:
- **Mb/s** (megabits per second)
- **MB/s** (megabytes per second)

This provides an easy **LAN speed test** between two machines using a single TCP connection.