// clokemidid: der MIDI-Dienst von ClokeOS. Verbindet die virtuellen MIDI-Endpunkte aller Programme miteinander
// (das macht unter macOS der MIDIServer). Protokoll: clokemidi-proto.h. Läuft als launchd-Dienst
// com.clokeos.midiserver als root, der Socket ist für alle Benutzer offen.
//   clokemidid [-v]     -v: jede Nachricht auf stderr
#include "clokemidi-proto.h"
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define MAXC 64
#define MAXE 256
#define MAXS 128

typedef struct { int fd; int32_t subs[MAXS]; int nsubs; } Client;
typedef struct { int32_t uid; int kind; int owner; char name[128]; } Endpoint;

static Client cl[MAXC]; static int ncl;
static Endpoint ep[MAXE]; static int nep;
static int verbose;

static int readall(int fd, void *b, size_t n) {
	size_t g = 0;
	while (g < n) { ssize_t r = read(fd, (char *)b + g, n - g); if (r <= 0) { if (r < 0 && errno == EINTR) continue; return -1; } g += r; }
	return 0;
}
static int writeall(int fd, const void *b, size_t n) {
	size_t g = 0;
	while (g < n) { ssize_t r = write(fd, (const char *)b + g, n - g); if (r <= 0) { if (r < 0 && errno == EINTR) continue; return -1; } g += r; }
	return 0;
}
static void sendmsg_to(int fd, const cm_hdr *h, const void *p) {
	if (writeall(fd, h, sizeof *h) == 0 && h->len) writeall(fd, p, h->len);
}

static Endpoint *findEp(int32_t uid) { for (int i = 0; i < nep; i++) if (ep[i].uid == uid) return &ep[i]; return NULL; }

static void announceAdd(Endpoint *e, int except) {
	cm_hdr h = { (uint32_t)strlen(e->name), CM_EP_ADD, (uint16_t)e->kind, e->uid, 0 };
	for (int i = 0; i < ncl; i++) if (cl[i].fd != except) sendmsg_to(cl[i].fd, &h, e->name);
}
static void announceRemove(int32_t uid, int except) {
	cm_hdr h = { 0, CM_EP_REMOVE, 0, uid, 0 };
	for (int i = 0; i < ncl; i++) if (cl[i].fd != except) sendmsg_to(cl[i].fd, &h, NULL);
}

static void dropClient(int k) {
	int fd = cl[k].fd;
	if (verbose) fprintf(stderr, "clokemidid: client %d gone\n", fd);
	for (int i = 0; i < nep; ) {
		if (ep[i].owner == fd) { int32_t uid = ep[i].uid; ep[i] = ep[--nep]; announceRemove(uid, fd); }
		else i++;
	}
	close(fd);
	cl[k] = cl[--ncl];
}

static void handle(int k, cm_hdr *h, char *p) {
	Client *c = &cl[k];
	if (verbose) fprintf(stderr, "clokemidid: fd %d type %u kind %u uid %d len %u\n", c->fd, h->type, h->kind, h->uid, h->len);
	switch (h->type) {
	case CM_ADD: {
		if (findEp(h->uid) || nep >= MAXE) break;
		Endpoint *e = &ep[nep++];
		e->uid = h->uid; e->kind = h->kind; e->owner = c->fd;
		size_t n = h->len < sizeof e->name - 1 ? h->len : sizeof e->name - 1;
		memcpy(e->name, p, n); e->name[n] = 0;
		announceAdd(e, c->fd);
		break; }
	case CM_REMOVE: {
		Endpoint *e = findEp(h->uid);
		if (!e || e->owner != c->fd) break;
		*e = ep[--nep];
		announceRemove(h->uid, c->fd);
		for (int i = 0; i < ncl; i++)
			for (int s = 0; s < cl[i].nsubs; ) { if (cl[i].subs[s] == h->uid) cl[i].subs[s] = cl[i].subs[--cl[i].nsubs]; else s++; }
		break; }
	case CM_SUB:
		for (int s = 0; s < c->nsubs; s++) if (c->subs[s] == h->uid) return;
		if (c->nsubs < MAXS) c->subs[c->nsubs++] = h->uid;
		break;
	case CM_UNSUB:
		for (int s = 0; s < c->nsubs; s++) if (c->subs[s] == h->uid) { c->subs[s] = c->subs[--c->nsubs]; break; }
		break;
	case CM_DATA: {             // MIDI aus einer Quelle: an alle Abonnenten
		Endpoint *e = findEp(h->uid);
		if (!e || e->owner != c->fd || e->kind != CM_SOURCE) break;
		for (int i = 0; i < ncl; i++) if (i != k)
			for (int s = 0; s < cl[i].nsubs; s++) if (cl[i].subs[s] == h->uid) { sendmsg_to(cl[i].fd, h, p); break; }
		break; }
	case CM_SEND: {             // MIDI an ein Ziel: an dessen Besitzer
		Endpoint *e = findEp(h->uid);
		if (e && e->kind == CM_DEST && e->owner != c->fd) sendmsg_to(e->owner, h, p);
		break; }
	}
}

int main(int argc, char **argv) {
	verbose = argc > 1 && !strcmp(argv[1], "-v");
	signal(SIGPIPE, SIG_IGN);
	unlink(CLOKEMIDI_SOCKET);
	int ls = socket(AF_UNIX, SOCK_STREAM, 0);
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	strncpy(a.sun_path, CLOKEMIDI_SOCKET, sizeof a.sun_path - 1);
	if (ls < 0 || bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 16)) { perror("clokemidid"); return 1; }
	chmod(CLOKEMIDI_SOCKET, 0666);
	static char buf[CLOKEMIDI_MAXPAYLOAD];
	for (;;) {
		struct pollfd pf[MAXC + 1];
		pf[0] = (struct pollfd){ ls, POLLIN, 0 };
		for (int i = 0; i < ncl; i++) pf[i + 1] = (struct pollfd){ cl[i].fd, POLLIN, 0 };
		if (poll(pf, ncl + 1, -1) < 0) { if (errno == EINTR) continue; perror("poll"); return 1; }
		// erst die Clients (Indizes ändern sich beim Entfernen, deshalb rückwärts)
		for (int i = ncl - 1; i >= 0; i--) {
			if (!(pf[i + 1].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) continue;
			cm_hdr h;
			if (readall(cl[i].fd, &h, sizeof h) || h.len > sizeof buf || (h.len && readall(cl[i].fd, buf, h.len))) { dropClient(i); continue; }
			handle(i, &h, buf);
		}
		if (pf[0].revents & POLLIN) {
			int fd = accept(ls, NULL, NULL);
			if (fd < 0) continue;
			if (ncl >= MAXC) { close(fd); continue; }
			int big = 1 << 20; setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &big, sizeof big);
			cl[ncl++] = (Client){ .fd = fd };
			if (verbose) fprintf(stderr, "clokemidid: client %d\n", fd);
			for (int e = 0; e < nep; e++) {
				cm_hdr h = { (uint32_t)strlen(ep[e].name), CM_EP_ADD, (uint16_t)ep[e].kind, ep[e].uid, 0 };
				sendmsg_to(fd, &h, ep[e].name);
			}
		}
	}
}
