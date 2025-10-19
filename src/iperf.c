/* iperf.c
   Minimal single-thread TCP throughput tester (client/server)
   Cross-platform: Windows / macOS / Linux
   - Server: receive and discard, print per-second and total rates
   - Client: send fixed-size chunks for given seconds, print per-second and total rates

   Build:
     Linux/macOS (clang/gcc):
       cc -O2 -Wall -Wextra -o iperf iperf.c
     Windows (MSVC):
       cl /O2 /W3 iperf.c ws2_32.lib

   Usage:
     Server: iperf s <port>
     Client: iperf c <host> <port> [seconds=10] [buf_kb=16]

   Notes:
     - Single TCP stream, blocking I/O, no TLS, no JSON.
     - Keeps CPU/memory low; suitable for retro/embedded peer.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
  #define _WINSOCK_DEPRECATED_NO_WARNINGS
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  typedef SOCKET sock_t;
  #define CLOSESOCK(s) closesocket(s)
  static void net_init(void){ WSADATA w; WSAStartup(MAKEWORD(2,2), &w); }
  static void net_fini(void){ WSACleanup(); }
  static void msleep(unsigned ms){ Sleep(ms); }
#else
  #include <unistd.h>
  #include <errno.h>
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  typedef int sock_t;
  #define CLOSESOCK(s) close(s)
  static void net_init(void){ }
  static void net_fini(void){ }
#endif

#ifdef __human68k__
#define SO_REUSEADDR (0)

#undef  CLOCKS_PER_SEC
#define CLOCKS_PER_SEC 100

static inline uint32_t trap_ontime_cs(void){
  uint32_t cs;
  __asm__ volatile(
    "moveq  #0x7F,%%d0 \n\t"  /* _ONTIME */
    "trap   #15        \n\t"  /* IOCS    */
    "move.l %%d0,%0    \n\t"
    : "=d"(cs)
    :
    : "d0","d1","a0","cc","memory"
  );
  return cs;
}
#endif

/* High-resolution wall clock seconds */
static double now_secs(void){
#ifdef _WIN32
  static LARGE_INTEGER freq = {0};
  LARGE_INTEGER t;
  if(!freq.QuadPart) QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&t);
  return (double)t.QuadPart / (double)freq.QuadPart;
#else

#ifdef __human68k__
  return (double)trap_ontime_cs() / (double)CLOCKS_PER_SEC;
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#endif

#endif /* _WIN32 */
}

static void human_rate(double bytes_per_sec, char* out, size_t outsz){
  /* Format as Mb/s and MB/s */
  double mbps = (bytes_per_sec * 8.0) / 1e6;
  double mBps = bytes_per_sec / 1e6;
  snprintf(out, outsz, "%.2f Mb/s (%.2f MB/s)", mbps, mBps);
}

static int run_server(const char* port_str){
  /* parse port */
  char* endp = NULL;
  unsigned long p = strtoul(port_str, &endp, 10);
  if(!port_str || *port_str == '\0' || (endp && *endp != '\0') || p == 0 || p > 65535){
    fprintf(stderr, "bad port: %s\n", port_str ? port_str : "(null)");
    return 1;
  }
  unsigned short port = (unsigned short)p;

  net_init();

  /* listen socket (IPv4 only) */
  sock_t ls = socket(AF_INET, SOCK_STREAM, 0);
  if(ls == (sock_t)-1
#ifdef _WIN32
     || ls == INVALID_SOCKET
#endif
  ){
    perror("socket");
    net_fini();
    return 1;
  }

  /* reuseaddr */
  int yes = 1;
  setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));

  /* bind to 0.0.0.0:port */
  struct sockaddr_in sa; memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_addr.s_addr = htonl(INADDR_ANY);
  sa.sin_port = htons(port);

  if(bind(ls, (struct sockaddr*)&sa, sizeof(sa)) < 0){
    perror("bind");
    CLOSESOCK(ls);
    net_fini();
    return 1;
  }

  if(listen(ls, 1) < 0){
    perror("listen");
    CLOSESOCK(ls);
    net_fini();
    return 1;
  }

  printf("[server] listening on port %u ...\n", (unsigned)port);

  /* accept one client */
  struct sockaddr_in cli; socklen_t clen = sizeof(cli);
  sock_t s = accept(ls, (struct sockaddr*)&cli, &clen);
  if(s == (sock_t)-1
#ifdef _WIN32
     || s == INVALID_SOCKET
#endif
  ){
    perror("accept");
    CLOSESOCK(ls);
    net_fini();
    return 1;
  }
  CLOSESOCK(ls);

  char rem[64] = {0};
  inet_ntop(AF_INET, &cli.sin_addr, rem, sizeof(rem));
  unsigned rport = ntohs(cli.sin_port);

  /* query the local address actually chosen for this connection */
  struct sockaddr_in loc; socklen_t llen = sizeof(loc);
  char locip[64] = {0};
  unsigned lport = 0;
  if(getsockname(s, (struct sockaddr*)&loc, &llen) == 0) {
    inet_ntop(AF_INET, &loc.sin_addr, locip, sizeof(locip));
    lport = ntohs(loc.sin_port);
    printf("[server] local=%s:%u  remote=%s:%u\n", locip, lport, rem, rport);
  } else {
    /* fallback: at least show remote */
    printf("[server] remote=%s:%u\n", rem, rport);
  }

  /* receive & discard */
  const size_t BUFSZ = 64 * 1024;
  char* buf = (char*)malloc(BUFSZ);
  if(!buf){
    fprintf(stderr, "oom\n");
    CLOSESOCK(s);
    net_fini();
    return 1;
  }

  size_t total = 0, interval = 0;
  double t0 = now_secs(), last = t0;

  for(;;){
    int n = (int)recv(s, buf, (int)BUFSZ, 0);
    if(n > 0){
      total += (size_t)n;
      interval += (size_t)n;
    }else if(n == 0){
      /* peer closed */
      break;
    }else{
#ifdef _WIN32
      int e = WSAGetLastError();
      if(e == WSAEINTR) continue;
      if(e == WSAEWOULDBLOCK) { msleep(1); continue; }
#else
      if(errno == EINTR) continue;
#endif
      perror("recv");
      break;
    }

    double now = now_secs();
    if(now - last >= 1.0){
      double bps = (double)interval / (now - last);
      char rate[64]; human_rate(bps, rate, sizeof(rate));
      printf("[server] %.0f-%.0fs: %zu bytes  %s\n",
             last - t0, now - t0, interval, rate);
      interval = 0;
      last = now;
    }
  }

  double t1 = now_secs();
  double dt = (t1 > t0) ? (t1 - t0) : 1e-6;
  char rate[64]; human_rate((double)total / dt, rate, sizeof(rate));
  printf("[server] TOTAL: %zu bytes in %.2fs  %s\n", total, dt, rate);

  free(buf);
  CLOSESOCK(s);
  net_fini();
  return 0;
}

static int run_client(const char* host, const char* port_str, int seconds, int buf_kb){
  if(seconds <= 0) seconds = 10;
  if(buf_kb <= 0) buf_kb = 16;

  /* parse port */
  char* endp = NULL;
  unsigned long p = strtoul(port_str, &endp, 10);
  if(!port_str || *port_str == '\0' || (endp && *endp != '\0') || p == 0 || p > 65535){
    fprintf(stderr, "bad port: %s\n", port_str ? port_str : "(null)");
    return 1;
  }
  unsigned short port = (unsigned short)p;

  net_init();

  /* create socket (IPv4 TCP) */
  sock_t s = socket(AF_INET, SOCK_STREAM, 0);
  if(s == (sock_t)-1
#ifdef _WIN32
     || s == INVALID_SOCKET
#endif
  ){
    perror("socket");
    net_fini();
    return 1;
  }

  /* resolve host (IPv4 only): try numeric, then DNS */
  struct sockaddr_in sa; memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port   = htons(port);

  int ok = 0;
  /* Try numeric dotted-quad first */
  if(inet_pton(AF_INET, host, &sa.sin_addr) == 1){
    ok = 1;
  }else{
    /* Fallback: gethostbyname() */
    struct hostent* he = gethostbyname(host);
    if(he && he->h_addr_list && he->h_addr_list[0]){
      memcpy(&sa.sin_addr, he->h_addr_list[0], he->h_length);
      ok = 1;
    }
  }
  if(!ok){
    fprintf(stderr, "resolve failed for host: %s\n", host);
    CLOSESOCK(s);
    net_fini();
    return 1;
  }

  printf("[client] connect %s:%u ...\n", host, (unsigned)port);
  if(connect(s, (struct sockaddr*)&sa, sizeof(sa)) < 0){
    perror("connect");
    CLOSESOCK(s);
    net_fini();
    return 1;
  }

  size_t BUFSZ = (size_t)buf_kb * 1024;
  char* buf = (char*)malloc(BUFSZ);
  if(!buf){
    fprintf(stderr, "oom\n");
    CLOSESOCK(s);
    net_fini();
    return 1;
  }
  memset(buf, 'A', BUFSZ);

  printf("[client] seconds=%d  buf=%dKB  (single TCP stream, IPv4)\n", seconds, buf_kb);

  size_t total = 0, interval = 0;
  double t0 = now_secs(), last = t0;
  double tend = t0 + (double)seconds;

  while(1){
    double now = now_secs();
    if(now >= tend) break;

    int n = (int)send(s, buf, (int)BUFSZ, 0);
    if(n > 0){
      total += (size_t)n;
      interval += (size_t)n;
    }else if(n == 0){
      /* unlikely on send; treat as end */
      break;
    }else{
#ifdef _WIN32
      int e = WSAGetLastError();
      if(e == WSAEINTR) continue;
      if(e == WSAEWOULDBLOCK) { msleep(1); continue; }
#else
      if(errno == EINTR) continue;
#endif
      perror("send");
      break;
    }

    now = now_secs();
    if(now - last >= 1.0){
      double bps = (double)interval / (now - last);
      char rate[64]; human_rate(bps, rate, sizeof(rate));
      printf("[client] %.0f-%.0fs: %zu bytes  %s\n",
             last - t0, now - t0, interval, rate);
      interval = 0;
      last = now;
    }
  }

  /* close write side then drain FIN politely (optional) */
#ifdef _WIN32
  shutdown(s, SD_SEND);
#else
  shutdown(s, SHUT_WR);
#endif
  char tmp[1024];
  while(recv(s, tmp, sizeof(tmp), 0) > 0) { /* discard */ }

  double t1 = now_secs();
  double dt = (t1 > t0) ? (t1 - t0) : 1e-6;
  char rate[64]; human_rate((double)total / dt, rate, sizeof(rate));
  printf("[client] TOTAL: %zu bytes in %.2fs  %s\n", total, dt, rate);

  free(buf);
  CLOSESOCK(s);
  net_fini();
  return 0;
}

int main(int argc, char** argv){
  if(argc < 3){
    fprintf(stderr,
      "iperf - minimal single-connection TCP tester\n"
      "Usage:\n"
      "  Server: %s s <port>\n"
      "  Client: %s c <host> <port> [seconds=10] [buf_kb=16]\n",
      argv[0], argv[0]);
    return 1;
  }

  if(argv[1][0] == 's'){
    /* server */
    const char* port = argv[2];
    return run_server(port);
  }else if(argv[1][0] == 'c'){
    if(argc < 4){
      fprintf(stderr, "client usage: %s c <host> <port> [seconds] [buf_kb]\n", argv[0]);
      return 1;
    }
    const char* host = argv[2];
    const char* port = argv[3];
    int seconds = (argc > 4) ? atoi(argv[4]) : 10;
    int buf_kb  = (argc > 5) ? atoi(argv[5]) : 16;
    return run_client(host, port, seconds, buf_kb);
  }else{
    fprintf(stderr, "unknown mode: %s (use 's' or 'c')\n", argv[1]);
    return 1;
  }
}
