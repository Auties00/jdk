// Phase 3 (JDK-8365196). Guest agent of the measurement VMs: plain HTTP/1.0 over winsock to the host
// (reached through QEMU's guestfwd; the guest has no other network).
//   p3agent loop <name>            forever: GET /job/<name>; run it with cmd; POST its output to /out/<name>
//   p3agent get <path> <file>      download /file/<path> to <file>
//   p3agent put <name> <file>      upload a file to /out/<name>
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HOST "10.0.2.100"

static SOCKET open_host(void) {
  struct sockaddr_in a;
  SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_port = htons(80);
  a.sin_addr.s_addr = inet_addr(HOST);
  if (connect(s, (struct sockaddr*)&a, sizeof(a)) != 0) {
    closesocket(s);
    return INVALID_SOCKET;
  }
  return s;
}

static int send_all(SOCKET s, const char* p, int n) {
  while (n > 0) {
    int k = send(s, p, n, 0);
    if (k <= 0) return -1;
    p += k;
    n -= k;
  }
  return 0;
}

// Returns the HTTP status, or -1; the body goes to the file.
static int http_get(const char* path, const char* file) {
  static char buf[1 << 16];
  char req[512];
  int n, have = 0, status = -1, header_done = 0;
  FILE* f;
  SOCKET s = open_host();
  if (s == INVALID_SOCKET) return -1;
  sprintf(req, "GET %s HTTP/1.0\r\nHost: " HOST "\r\n\r\n", path);
  if (send_all(s, req, (int)strlen(req)) != 0) { closesocket(s); return -1; }
  f = fopen(file, "wb");
  if (f == NULL) { closesocket(s); return -1; }
  while ((n = recv(s, buf + have, (int)sizeof(buf) - have, 0)) > 0) {
    if (!header_done) {
      int i;
      have += n;
      for (i = 0; i + 3 < have; i++) {
        if (memcmp(buf + i, "\r\n\r\n", 4) == 0) {
          header_done = 1;
          status = atoi(buf + 9);
          fwrite(buf + i + 4, 1, have - i - 4, f);
          have = 0;
          break;
        }
      }
      if (!header_done && have == (int)sizeof(buf)) break;
    } else {
      fwrite(buf, 1, n, f);
    }
  }
  fclose(f);
  closesocket(s);
  return status;
}

static int http_post(const char* path, const char* file) {
  static char buf[1 << 16];
  char req[512];
  long size;
  size_t n;
  SOCKET s;
  FILE* f = fopen(file, "rb");
  if (f == NULL) return -1;
  fseek(f, 0, SEEK_END);
  size = ftell(f);
  fseek(f, 0, SEEK_SET);
  s = open_host();
  if (s == INVALID_SOCKET) { fclose(f); return -1; }
  sprintf(req, "POST %s HTTP/1.0\r\nHost: " HOST "\r\nContent-Length: %ld\r\n\r\n", path, size);
  send_all(s, req, (int)strlen(req));
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
    if (send_all(s, buf, (int)n) != 0) break;
  }
  fclose(f);
  recv(s, buf, sizeof(buf), 0);
  closesocket(s);
  return 0;
}

int main(int argc, char** argv) {
  WSADATA w;
  char path[300];
  WSAStartup(MAKEWORD(2, 2), &w);
  if (argc >= 4 && strcmp(argv[1], "get") == 0) {
    int st;
    sprintf(path, "/file/%s", argv[2]);
    st = http_get(path, argv[3]);
    printf("p3agent get %s -> %d\n", argv[2], st);
    return st == 200 ? 0 : 1;
  }
  if (argc >= 4 && strcmp(argv[1], "put") == 0) {
    sprintf(path, "/out/%s", argv[2]);
    return http_post(path, argv[3]) == 0 ? 0 : 1;
  }
  if (argc >= 3 && strcmp(argv[1], "loop") == 0) {
    for (;;) {
      int st;
      sprintf(path, "/job/%s", argv[2]);
      st = http_get(path, "C:\\p3\\job.cmd");
      if (st == 200) {
        system("cmd /c C:\\p3\\job.cmd > C:\\p3\\job.out 2>&1");
        sprintf(path, "/out/%s", argv[2]);
        http_post(path, "C:\\p3\\job.out");
      }
      Sleep(st == 200 ? 1000 : 5000);
    }
  }
  return 2;
}
