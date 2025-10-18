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
#define CLOCK_MONOTONIC (0)
#define SO_REUSEADDR (0)
void clock_gettime(/*clockid_t clockid*/int clockid, struct timespec *tp){}
const char* gai_strerror(int errcode) {
  static char str[1024];
  sprintf(str, "errcode=%d", errcode);
  return str;
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
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#endif
}

static void human_rate(double bytes_per_sec, char* out, size_t outsz){
  /* Format as Mb/s and MB/s */
  double mbps = (bytes_per_sec * 8.0) / 1e6;
  double mBps = bytes_per_sec / 1e6;
  snprintf(out, outsz, "%.2f Mb/s (%.2f MB/s)", mbps, mBps);
}

static int set_reuseaddr(sock_t s){
#ifdef __human68k__
  return 0;
#else
  int yes = 1;
  return setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
#endif
}

static int run_server(const char* port_str){
  net_init();

  /* Resolve and bind (IPv4 only for simplicity) */
  struct addrinfo hints; memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;

  struct addrinfo* ai = NULL;
  int rc = getaddrinfo(NULL, port_str, &hints, &ai);
  if(rc != 0 || !ai){ fprintf(stderr, "getaddrinfo: %s\n",
#ifdef _WIN32
    gai_strerrorA(rc)
#else
    gai_strerror(rc)
#endif
  ); net_fini(); return 1; }

  sock_t ls = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
  if(ls == (sock_t)-1 || ls == 0xFFFFFFFF){
    perror("socket"); freeaddrinfo(ai); net_fini(); return 1;
  }
  set_reuseaddr(ls);
  if(bind(ls, ai->ai_addr, (int)ai->ai_addrlen) < 0){
    perror("bind"); CLOSESOCK(ls); freeaddrinfo(ai); net_fini(); return 1;
  }
  freeaddrinfo(ai);

  if(listen(ls, 1) < 0){
    perror("listen"); CLOSESOCK(ls); net_fini(); return 1;
  }

  printf("[server] listening on port %s ...\n", port_str);

  struct sockaddr_storage ss; socklen_t slen = sizeof(ss);
  sock_t s = accept(ls, (struct sockaddr*)&ss, &slen);
  if(s == (sock_t)-1 || s == 0xFFFFFFFF){
    perror("accept"); CLOSESOCK(ls); net_fini(); return 1;
  }
  CLOSESOCK(ls);

  char addrbuf[64] = {0};
  if(ss.ss_family == AF_INET){
    struct sockaddr_in* si = (struct sockaddr_in*)&ss;
    inet_ntop(AF_INET, &si->sin_addr, addrbuf, sizeof(addrbuf));
    printf("[server] client: %s:%u\n", addrbuf, ntohs(si->sin_port));
  }

  const size_t BUFSZ = 64 * 1024;
  char* buf = (char*)malloc(BUFSZ);
  if(!buf){ fprintf(stderr, "oom\n"); CLOSESOCK(s); net_fini(); return 1; }

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

  net_init();

  /* Resolve */
  struct addrinfo hints; memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;            /* keep it simple: IPv4 */
  hints.ai_socktype = SOCK_STREAM;

  struct addrinfo* ai = NULL;
  int rc = getaddrinfo(host, port_str, &hints, &ai);
  if(rc != 0 || !ai){ fprintf(stderr, "getaddrinfo: %s\n",
#ifdef _WIN32
    gai_strerrorA(rc)
#else
    gai_strerror(rc)
#endif
  ); net_fini(); return 1; }

  sock_t s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
  if(s == (sock_t)-1 || s == 0xFFFFFFFF){
    perror("socket"); freeaddrinfo(ai); net_fini(); return 1;
  }

  printf("[client] connect %s:%s ...\n", host, port_str);
  if(connect(s, ai->ai_addr, (int)ai->ai_addrlen) < 0){
    perror("connect"); CLOSESOCK(s); freeaddrinfo(ai); net_fini(); return 1;
  }
  freeaddrinfo(ai);

  size_t BUFSZ = (size_t)buf_kb * 1024;
  char* buf = (char*)malloc(BUFSZ);
  if(!buf){ fprintf(stderr, "oom\n"); CLOSESOCK(s); net_fini(); return 1; }
  memset(buf, 'A', BUFSZ);

  printf("[client] seconds=%d  buf=%dKB  (single TCP stream)\n", seconds, buf_kb);

  size_t total = 0, interval = 0;
  double t0 = now_secs(), last = t0;
  double tend = t0 + (double)seconds;

  while(1){
    /* Time check before sending next chunk */
    double now = now_secs();
    if(now >= tend) break;

    int n = (int)send(s, buf, (int)BUFSZ, 0);
    if(n > 0){
      total += (size_t)n;
      interval += (size_t)n;
    }else if(n == 0){
      /* Shouldn't happen on send; treat as end */
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

  /* Politely shutdown write side, then drain any server FIN */
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
