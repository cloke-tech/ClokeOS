// clokeos-livecall: Live-Stick meldet sich bei einem Mac, damit der Mac per SSH ansetzen kann.
//
// Läuft als Launch-Daemon auf dem Stick (recovery/usb.sh live). Sobald eine Netzwerkschnittstelle eine
// IPv4-Adresse hat, verbindet es sich mit TCP-Port 8222 der Hosts aus /etc/clokeos-livecall.conf (eine
// Adresse oder ein Name pro Zeile), sonst mit jedem Rechner im eigenen /24. Es sendet "HELLO ip=…" und
// beim ersten Mal die Hardware (PCI-Geräte aus ioreg) und die Netzwerkschnittstellen. Danach meldet es sich
// alle 30 Sekunden kurz wieder. Keine Anmeldedaten, nur Hardware-Auskunft; angemeldet wird per SSH-Schlüssel.
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define PORT 8222
#define CONF "/etc/clokeos-livecall.conf"

static char myip[64], ifname[32], masknet[64];

static int find_ip(void) {
  struct ifaddrs *ifa, *p;
  if (getifaddrs(&ifa)) return 0;
  int ok = 0;
  for (p = ifa; p; p = p->ifa_next) {
    if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
    if (p->ifa_flags & IFF_LOOPBACK) continue;
    if (!(p->ifa_flags & IFF_UP)) continue;
    struct sockaddr_in *a = (struct sockaddr_in *)p->ifa_addr;
    uint32_t ip = ntohl(a->sin_addr.s_addr);
    if ((ip >> 16) == 0xA9FE) continue; // 169.254.x.x ist keine echte Adresse
    inet_ntop(AF_INET, &a->sin_addr, myip, sizeof myip);
    snprintf(ifname, sizeof ifname, "%s", p->ifa_name);
    snprintf(masknet, sizeof masknet, "%u.%u.%u", (ip >> 24) & 255, (ip >> 16) & 255, (ip >> 8) & 255);
    ok = 1;
    break;
  }
  freeifaddrs(ifa);
  return ok;
}

// verbindet mit ip:PORT, höchstens ms Millisekunden; gibt den Socket zurück oder -1
static int dial(const char *host, int ms) {
  struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM}, *res;
  char port[8];
  snprintf(port, sizeof port, "%d", PORT);
  if (getaddrinfo(host, port, &hints, &res)) return -1;
  int s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) { freeaddrinfo(res); return -1; }
  fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
  int r = connect(s, res->ai_addr, res->ai_addrlen);
  freeaddrinfo(res);
  if (r < 0 && errno != EINPROGRESS) { close(s); return -1; }
  if (r < 0) {
    fd_set w;
    FD_ZERO(&w);
    FD_SET(s, &w);
    struct timeval tv = {ms / 1000, (ms % 1000) * 1000};
    if (select(s + 1, 0, &w, 0, &tv) <= 0) { close(s); return -1; }
    int e = 0;
    socklen_t l = sizeof e;
    getsockopt(s, SOL_SOCKET, SO_ERROR, &e, &l);
    if (e) { close(s); return -1; }
  }
  fcntl(s, F_SETFL, fcntl(s, F_GETFL) & ~O_NONBLOCK);
  return s;
}

static void send_all(int s, const char *b, size_t n) {
  while (n) {
    ssize_t w = write(s, b, n);
    if (w <= 0) return;
    b += w;
    n -= (size_t)w;
  }
}

static void run_to(int s, const char *title, const char *cmd) {
  char h[128];
  snprintf(h, sizeof h, "\n=== %s ===\n", title);
  send_all(s, h, strlen(h));
  FILE *f = popen(cmd, "r");
  if (!f) return;
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) send_all(s, buf, n);
  pclose(f);
}

static void hello(int s, int full) {
  char line[256], host[128] = "?";
  gethostname(host, sizeof host);
  snprintf(line, sizeof line, "HELLO ip=%s if=%s host=%s full=%d\n", myip, ifname, host, full);
  send_all(s, line, strlen(line));
  if (!full) return;
  run_to(s, "uname", "/usr/bin/uname -a 2>&1 || /bin/uname -a 2>&1");
  run_to(s, "ipconfig", "/usr/sbin/ipconfig getsummary " "en0 2>&1 | head -40");
  run_to(s, "pci", "/usr/sbin/ioreg -r -c IOPCIDevice -l -w0 2>&1");
  run_to(s, "network", "/usr/sbin/ioreg -r -c IONetworkInterface -l -w0 2>&1 | head -200");
  run_to(s, "usb", "/usr/sbin/ioreg -r -c IOUSBDevice -l -w0 2>&1 | head -200");
  run_to(s, "dmesg", "/bin/dmesg 2>&1 | tail -300");
}

// ein Ziel nach dem anderen aus der Konfiguration, dann der eigene /24
static int connect_any(void) {
  FILE *f = fopen(CONF, "r");
  char host[256];
  if (f) {
    while (fgets(host, sizeof host, f)) {
      host[strcspn(host, "\r\n")] = 0;
      if (!host[0] || host[0] == '#') continue;
      int s = dial(host, 1500);
      if (s >= 0) { fclose(f); return s; }
    }
    fclose(f);
  }
  for (int i = 1; i < 255; i++) {
    snprintf(host, sizeof host, "%s.%d", masknet, i);
    if (!strcmp(host, myip)) continue;
    int s = dial(host, 120);
    if (s >= 0) return s;
  }
  return -1;
}

int main(void) {
  int full = 1;
  for (;;) {
    if (!find_ip()) { sleep(3); continue; }
    int s = connect_any();
    if (s < 0) { sleep(10); continue; }
    hello(s, full);
    full = 0;
    close(s);
    sleep(30);
  }
}
