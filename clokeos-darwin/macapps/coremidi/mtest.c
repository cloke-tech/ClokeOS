// Prüfprogramm für CoreMIDI: läuft gegen Apples CoreMIDI (Mac) und gegen macapps/coremidi (ClokeOS);
// die Ausgaben müssen gleich sein. Nur deterministische Werte (keine Refs, keine IDs).
#include <CoreMIDI/CoreMIDI.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void hex(const char *tag, const Byte *d, int n) {
	printf("%s:", tag); for (int i = 0; i < n; i++) printf(" %02x", d[i]); printf("\n");
}
static void words(const char *tag, const MIDIEventList *el) {
	const MIDIEventPacket *p = &el->packet[0];
	printf("%s proto=%d n=%u", tag, (int)el->protocol, el->numPackets);
	for (UInt32 i = 0; i < el->numPackets; i++) {
		printf(" [t=%llu", p->timeStamp);
		for (UInt32 k = 0; k < p->wordCount; k++) printf(" %08x", p->words[k]);
		printf("]"); p = MIDIEventPacketNext(p);
	}
	printf("\n");
}
static void pkts(const char *tag, const MIDIPacketList *pl) {
	const MIDIPacket *p = &pl->packet[0];
	printf("%s n=%u", tag, pl->numPackets);
	for (UInt32 i = 0; i < pl->numPackets; i++) {
		printf(" [t=%llu", p->timeStamp);
		for (int k = 0; k < p->length; k++) printf(" %02x", p->data[k]);
		printf("]"); p = MIDIPacketNext(p);
	}
	printf("\n");
}
static void spin(double s) { CFRunLoopRunInMode(kCFRunLoopDefaultMode, s, false); }

static int sawAdded, sawRemoved, sawSetup;
static MIDIEndpointRef watch;
static void notifyProc(const MIDINotification *m, void *ref) {
	(void)ref;
	if (m->messageID == kMIDIMsgSetupChanged) sawSetup = 1;
	if (m->messageID == kMIDIMsgObjectAdded && ((const MIDIObjectAddRemoveNotification *)m)->child == watch) sawAdded = 1;
	if (m->messageID == kMIDIMsgObjectRemoved && ((const MIDIObjectAddRemoveNotification *)m)->child == watch) sawRemoved = 1;
}
static void readProc(const MIDIPacketList *pl, void *readRef, void *connRef) {
	printf("readProc ref=%s conn=%s ", (char *)readRef, connRef ? (char *)connRef : "-");
	pkts("pkts", pl); fflush(stdout);
}
static void destProc(const MIDIPacketList *pl, void *readRef, void *connRef) {
	(void)connRef; printf("destProc ref=%s ", (char *)readRef); pkts("pkts", pl); fflush(stdout);
}

int main(void) {
	setvbuf(stdout, NULL, _IOLBF, 0);
	// 1. Paketlisten
	Byte buf[1024]; MIDIPacketList *pl = (MIDIPacketList *)buf;
	MIDIPacket *p = MIDIPacketListInit(pl);
	Byte on[] = {0x90, 60, 100}, cc[] = {0xB0, 7, 99}, sx[] = {0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7}, off[] = {0x80, 60, 0};
	p = MIDIPacketListAdd(pl, sizeof buf, p, 0, 3, on);
	p = MIDIPacketListAdd(pl, sizeof buf, p, 0, 3, cc);
	p = MIDIPacketListAdd(pl, sizeof buf, p, 0, 6, sx);
	p = MIDIPacketListAdd(pl, sizeof buf, p, 0, 3, off);
	p = MIDIPacketListAdd(pl, sizeof buf, p, 5, 3, on);
	pkts("list", pl);
	printf("gap=%ld\n", (long)((Byte *)MIDIPacketNext(&pl->packet[0]) - (Byte *)&pl->packet[0]));
	Byte small[40]; MIDIPacketList *sp = (MIDIPacketList *)small; MIDIPacket *q = MIDIPacketListInit(sp);
	q = MIDIPacketListAdd(sp, sizeof small, q, 1, 3, on); printf("small1=%s\n", q ? "ok" : "NULL");
	q = q ? MIDIPacketListAdd(sp, sizeof small, q, 2, 20, buf) : NULL; printf("small2=%s\n", q ? "ok" : "NULL");
	// 2. Eventlisten
	Byte eb[1024]; MIDIEventList *el = (MIDIEventList *)eb;
	MIDIEventPacket *e = MIDIEventListInit(el, kMIDIProtocol_1_0);
	UInt32 w1 = 0x20903c64, w2[2] = {0x40903c00, 0xc8000000};
	e = MIDIEventListAdd(el, sizeof eb, e, 0, 1, &w1);
	e = MIDIEventListAdd(el, sizeof eb, e, 0, 2, w2);
	e = MIDIEventListAdd(el, sizeof eb, e, 9, 1, &w1);
	words("evlist", el);

	// 3. Client, virtuelle Quelle und Ziel
	MIDIClientRef cl = 0, cl2 = 0;
	printf("client=%d\n", (int)MIDIClientCreate(CFSTR("mtest"), notifyProc, NULL, &cl));
	printf("client2=%d\n", (int)MIDIClientCreate(CFSTR("mtest2"), NULL, NULL, &cl2));
	ItemCount ns0 = MIDIGetNumberOfSources(), nd0 = MIDIGetNumberOfDestinations();
	MIDIEndpointRef src = 0, dst = 0, dst2 = 0;
	printf("source=%d\n", (int)MIDISourceCreate(cl2, CFSTR("ClokeTest Src"), &src));
	watch = src;
	printf("dest=%d\n", (int)MIDIDestinationCreate(cl2, CFSTR("ClokeTest Dst"), destProc, "D", &dst));
	printf("dest2=%d\n", (int)MIDIDestinationCreateWithProtocol(cl2, CFSTR("ClokeTest Dst2"), kMIDIProtocol_2_0, &dst2,
		^(const MIDIEventList *l, void *c) { (void)c; words("dest2", l); }));
	spin(0.5);
	printf("sources+%ld dests+%ld\n", (long)(MIDIGetNumberOfSources() - ns0), (long)(MIDIGetNumberOfDestinations() - nd0));
	int found = 0; for (ItemCount i = 0; i < MIDIGetNumberOfSources(); i++) if (MIDIGetSource(i) == src) found = 1;
	printf("source listed=%d\n", found);
	printf("notify added=%d setup=%d\n", sawAdded, sawSetup);

	// 4. Eigenschaften
	CFStringRef s = NULL; char c[128];
	OSStatus r = MIDIObjectGetStringProperty(src, kMIDIPropertyName, &s);
	CFStringGetCString(s, c, sizeof c, kCFStringEncodingUTF8); printf("name=%d %s\n", (int)r, c); CFRelease(s);
	r = MIDIObjectGetStringProperty(src, kMIDIPropertyDisplayName, &s);
	if (!r) { CFStringGetCString(s, c, sizeof c, kCFStringEncodingUTF8); CFRelease(s); } else strcpy(c, "-");
	printf("displayName=%d %s\n", (int)r, c);
	SInt32 uid = 0; r = MIDIObjectGetIntegerProperty(src, kMIDIPropertyUniqueID, &uid);
	printf("uniqueID=%d nonzero=%d\n", (int)r, uid != 0);
	MIDIObjectRef fo = 0; MIDIObjectType ft = -9;
	r = MIDIObjectFindByUniqueID(uid, &fo, &ft); printf("find=%d same=%d type=%d\n", (int)r, fo == src, (int)ft);
	SInt32 v = 0; r = MIDIObjectGetIntegerProperty(src, kMIDIPropertyOffline, &v); printf("offline=%d %d\n", (int)r, (int)v);
	r = MIDIObjectGetIntegerProperty(src, CFSTR("clokeNoSuchProperty"), &v); printf("unknown=%d\n", (int)r);
	r = MIDIObjectSetIntegerProperty(src, CFSTR("clokeTest"), 42); MIDIObjectGetIntegerProperty(src, CFSTR("clokeTest"), &v);
	printf("setint=%d %d\n", (int)r, (int)v);
	r = MIDIObjectGetIntegerProperty(src, kMIDIPropertyName, &v); printf("wrongtype=%d\n", (int)r);
	MIDIEntityRef ent = 1; r = MIDIEndpointGetEntity(src, &ent); printf("entity=%d ent=%u\n", (int)r, (unsigned)ent);

	// 5. Eingangsports an der Quelle
	MIDIPortRef in1 = 0, in2 = 0, in3 = 0, out = 0;
	MIDIInputPortCreate(cl, CFSTR("in1"), readProc, "P1", &in1);
	MIDIInputPortCreateWithProtocol(cl, CFSTR("in2"), kMIDIProtocol_1_0, &in2, ^(const MIDIEventList *l, void *cr) { printf("in2 conn=%s ", cr ? (char *)cr : "-"); words("ev", l); });
	MIDIInputPortCreateWithProtocol(cl, CFSTR("in3"), kMIDIProtocol_2_0, &in3, ^(const MIDIEventList *l, void *cr) { (void)cr; words("in3", l); });
	printf("connect=%d %d %d\n", (int)MIDIPortConnectSource(in1, src, "C1"), (int)MIDIPortConnectSource(in2, src, "C2"), (int)MIDIPortConnectSource(in3, src, NULL));
	printf("connect-dest=%d\n", (int)MIDIPortConnectSource(in1, dst, NULL) != 0);
	MIDIPacket *pp = MIDIPacketListInit(pl);
	Byte pb[] = {0xE1, 0x00, 0x40}, prg[] = {0xC2, 5};
	pp = MIDIPacketListAdd(pl, sizeof buf, pp, 0, 3, on);
	pp = MIDIPacketListAdd(pl, sizeof buf, pp, 0, 3, pb);
	pp = MIDIPacketListAdd(pl, sizeof buf, pp, 0, 2, prg);
	pp = MIDIPacketListAdd(pl, sizeof buf, pp, 0, 6, sx);
	r = MIDIReceived(src, pl);
	spin(0.5);
	printf("received=%d\n", (int)r);
	// 6. Senden an Ziele
	MIDIOutputPortCreate(cl, CFSTR("out"), &out);
	pp = MIDIPacketListInit(pl); pp = MIDIPacketListAdd(pl, sizeof buf, pp, 0, 3, cc);
	r = MIDISend(out, dst, pl); spin(0.3); printf("send=%d\n", (int)r);
	r = MIDISend(out, dst2, pl); spin(0.3); printf("send2=%d\n", (int)r);
	printf("send-src=%d\n", (int)MIDISend(out, src, pl) != 0);
	printf("send-badport=%d\n", (int)MIDISend(in1, dst, pl) != 0);
	e = MIDIEventListInit(el, kMIDIProtocol_1_0); e = MIDIEventListAdd(el, sizeof eb, e, 0, 1, &w1);
	r = MIDISendEventList(out, dst, el); spin(0.3); printf("sendev=%d\n", (int)r);
	printf("disconnect=%d again=%d\n", (int)MIDIPortDisconnectSource(in1, src), MIDIPortDisconnectSource(in1, src) != 0);
	// 7. Aufräumen
	printf("dispose=%d\n", (int)MIDIEndpointDispose(src));
	spin(0.5);
	printf("sources+%ld notify removed=%d\n", (long)(MIDIGetNumberOfSources() - ns0), sawRemoved);
	printf("clientdispose=%d\n", (int)MIDIClientDispose(cl2));
	spin(0.3);
	printf("dests+%ld\n", (long)(MIDIGetNumberOfDestinations() - nd0));
	printf("badclient=%d\n", MIDIOutputPortCreate(12345, CFSTR("x"), &out) != 0);
	printf("done\n");
	return 0;
}
