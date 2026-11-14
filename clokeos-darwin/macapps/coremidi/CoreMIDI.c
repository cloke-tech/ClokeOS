// CoreMIDI für ClokeOS: Clients, Ports, virtuelle Quellen und Ziele, Paket- und Eventlisten, Eigenschaften.
//
// Es gibt keinen MIDI-Server und keine Hardware-Treiber: Alles läuft im Prozess. Virtuelle Endpunkte, die ein
// Programm anlegt, erscheinen wie bei macOS in MIDIGetNumberOfSources/Destinations; MIDIReceived an einer
// virtuellen Quelle geht an alle Eingangsports, die mit ihr verbunden sind, MIDISend an ein virtuelles Ziel an
// dessen Lese-Funktion. Zwischen MIDI 1.0 (MIDIPacketList) und UMP (MIDIEventList) wird bei Bedarf umgewandelt.
// Zustellung sofort und synchron (Zeitstempel in der Zukunft werden nicht verzögert); Geräte und Entities gibt es
// keine (MIDIGetNumberOfDevices = 0). Benachrichtigungen (Objekt hinzugefügt/entfernt, Setup geändert) laufen über
// den Haupt-Runloop. CLOKE_MIDI_TRACE=1 meldet Aufrufe auf stderr.
#include <CoreMIDI/CoreMIDI.h>
#include <CoreFoundation/CoreFoundation.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <Block.h>

#define EXPORT __attribute__((visibility("default")))

// ---------------------------------------------------------------- Eigenschaftsnamen (Apples Werte)
EXPORT const CFStringRef kMIDIPropertyName = CFSTR("name");
EXPORT const CFStringRef kMIDIPropertyManufacturer = CFSTR("manufacturer");
EXPORT const CFStringRef kMIDIPropertyModel = CFSTR("model");
EXPORT const CFStringRef kMIDIPropertyUniqueID = CFSTR("uniqueID");
EXPORT const CFStringRef kMIDIPropertyDeviceID = CFSTR("deviceID");
EXPORT const CFStringRef kMIDIPropertyReceiveChannels = CFSTR("receiveChannels");
EXPORT const CFStringRef kMIDIPropertyTransmitChannels = CFSTR("transmitChannels");
EXPORT const CFStringRef kMIDIPropertyMaxSysExSpeed = CFSTR("maxSysExSpeed");
EXPORT const CFStringRef kMIDIPropertyAdvanceScheduleTimeMuSec = CFSTR("scheduleAheadMuSec");
EXPORT const CFStringRef kMIDIPropertyIsEmbeddedEntity = CFSTR("embedded");
EXPORT const CFStringRef kMIDIPropertyIsBroadcast = CFSTR("isBroadcast");
EXPORT const CFStringRef kMIDIPropertySingleRealtimeEntity = CFSTR("singleRealtimeEntity");
EXPORT const CFStringRef kMIDIPropertyConnectionUniqueID = CFSTR("connUniqueID");
EXPORT const CFStringRef kMIDIPropertyOffline = CFSTR("offline");
EXPORT const CFStringRef kMIDIPropertyPrivate = CFSTR("private");
EXPORT const CFStringRef kMIDIPropertyDriverOwner = CFSTR("driver");
EXPORT const CFStringRef kMIDIPropertyFactoryPatchNameFile = CFSTR("factoryPatchNameFile");
EXPORT const CFStringRef kMIDIPropertyUserPatchNameFile = CFSTR("userPatchNameFile");
EXPORT const CFStringRef kMIDIPropertyNameConfiguration = CFSTR("nameConfiguration");
EXPORT const CFStringRef kMIDIPropertyNameConfigurationDictionary = CFSTR("nameConfigurationDictionary");
EXPORT const CFStringRef kMIDIPropertyImage = CFSTR("image");
EXPORT const CFStringRef kMIDIPropertyDriverVersion = CFSTR("driverVersion");
EXPORT const CFStringRef kMIDIPropertySupportsGeneralMIDI = CFSTR("supportsGeneralMIDI");
EXPORT const CFStringRef kMIDIPropertySupportsMMC = CFSTR("supportsMMC");
EXPORT const CFStringRef kMIDIPropertyCanRoute = CFSTR("canRoute");
EXPORT const CFStringRef kMIDIPropertyReceivesClock = CFSTR("receivesClock");
EXPORT const CFStringRef kMIDIPropertyReceivesMTC = CFSTR("receivesMTC");
EXPORT const CFStringRef kMIDIPropertyReceivesNotes = CFSTR("receivesNotes");
EXPORT const CFStringRef kMIDIPropertyReceivesProgramChanges = CFSTR("receivesProgramChanges");
EXPORT const CFStringRef kMIDIPropertyReceivesBankSelectMSB = CFSTR("receivesBankSelectMSB");
EXPORT const CFStringRef kMIDIPropertyReceivesBankSelectLSB = CFSTR("receivesBankSelectLSB");
EXPORT const CFStringRef kMIDIPropertyTransmitsBankSelectMSB = CFSTR("transmitsBankSelectMSB");
EXPORT const CFStringRef kMIDIPropertyTransmitsBankSelectLSB = CFSTR("transmitsBankSelectLSB");
EXPORT const CFStringRef kMIDIPropertyTransmitsClock = CFSTR("transmitsClock");
EXPORT const CFStringRef kMIDIPropertyTransmitsMTC = CFSTR("transmitsMTC");
EXPORT const CFStringRef kMIDIPropertyTransmitsNotes = CFSTR("transmitsNotes");
EXPORT const CFStringRef kMIDIPropertyTransmitsProgramChanges = CFSTR("transmitsProgramChanges");
EXPORT const CFStringRef kMIDIPropertyPanDisruptsStereo = CFSTR("panDisruptsStereo");
EXPORT const CFStringRef kMIDIPropertyIsSampler = CFSTR("isSampler");
EXPORT const CFStringRef kMIDIPropertyIsDrumMachine = CFSTR("isDrumMachine");
EXPORT const CFStringRef kMIDIPropertyIsMixer = CFSTR("isMixer");
EXPORT const CFStringRef kMIDIPropertyIsEffectUnit = CFSTR("isEffectUnit");
EXPORT const CFStringRef kMIDIPropertyMaxReceiveChannels = CFSTR("maxReceiveChannels");
EXPORT const CFStringRef kMIDIPropertyMaxTransmitChannels = CFSTR("maxTransmitChannels");
EXPORT const CFStringRef kMIDIPropertyDriverDeviceEditorApp = CFSTR("driverDeviceEditorApp");
EXPORT const CFStringRef kMIDIPropertySupportsShowControl = CFSTR("supportsShowControl");
EXPORT const CFStringRef kMIDIPropertyDisplayName = CFSTR("displayName");
EXPORT const CFStringRef kMIDIPropertyProtocolID = CFSTR("protocol");
EXPORT const CFStringRef kMIDIPropertyUMPActiveGroupBitmap = CFSTR("UMPActiveGroupBitmap");
EXPORT const CFStringRef kMIDIPropertyUMPCanTransmitGroupless = CFSTR("UMPCanTransmitGroupless");
EXPORT const CFStringRef kMIDIPropertyAssociatedEndpoint = CFSTR("associatedEndpoint");

// ---------------------------------------------------------------- Objekte
enum { O_CLIENT = 1, O_INPORT, O_OUTPORT, O_SOURCE, O_DEST };

typedef struct Conn { MIDIEndpointRef src; void *refcon; struct Conn *next; } Conn;

typedef struct Obj {
	MIDIObjectRef ref;
	int kind;
	MIDIClientRef client;             // Besitzer (Ports, Endpunkte)
	CFMutableDictionaryRef props;     // Name, uniqueID, … wie bei Apple als Eigenschaften
	MIDIProtocolID protocol;          // 0 = MIDI-1.0-Pakete (Proc/ReadBlock), sonst Eventlisten
	MIDINotifyProc notify; void *notifyRef; MIDINotifyBlock notifyBlock;   // Client
	MIDIReadProc read; void *readRef; MIDIReadBlock readBlock; MIDIReceiveBlock recvBlock;   // Eingangsport, Ziel
	Conn *conns;                      // Eingangsport: verbundene Quellen
	struct Obj *next;
} Obj;

static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
static Obj *objs;
static MIDIObjectRef nextRef = 0x10000;
static int trace = -1;

static void tr(const char *fmt, ...) {
	if (trace < 0) trace = getenv("CLOKE_MIDI_TRACE") != NULL;
	if (!trace) return;
	va_list ap; va_start(ap, fmt);
	fputs("[coremidi] ", stderr); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
	va_end(ap);
}

static Obj *find(MIDIObjectRef r) { for (Obj *o = objs; o; o = o->next) if (o->ref == r) return o; return NULL; }

static SInt32 newUniqueID(void) {
	for (;;) {
		SInt32 id = (SInt32)arc4random();
		if (id == 0 || id == -1) continue;
		int dup = 0;
		for (Obj *o = objs; o; o = o->next) {
			CFNumberRef n = o->props ? CFDictionaryGetValue(o->props, kMIDIPropertyUniqueID) : NULL;
			SInt32 v; if (n && CFNumberGetValue(n, kCFNumberSInt32Type, &v) && v == id) { dup = 1; break; }
		}
		if (!dup) return id;
	}
}

static void setInt(CFMutableDictionaryRef d, CFStringRef k, SInt32 v) {
	CFNumberRef n = CFNumberCreate(NULL, kCFNumberSInt32Type, &v);
	CFDictionarySetValue(d, k, n); CFRelease(n);
}

// Anlegen (unter lk)
static Obj *mk(int kind, MIDIClientRef client, CFStringRef name) {
	Obj *o = calloc(1, sizeof *o);
	o->ref = nextRef++; o->kind = kind; o->client = client;
	o->props = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	if (name) { CFStringRef c = CFStringCreateCopy(NULL, name); CFDictionarySetValue(o->props, kMIDIPropertyName, c); CFRelease(c); }
	if (kind == O_SOURCE || kind == O_DEST) setInt(o->props, kMIDIPropertyUniqueID, newUniqueID());
	o->next = objs; objs = o;
	return o;
}

static void unlink_obj(Obj *o) {
	for (Obj **p = &objs; *p; p = &(*p)->next) if (*p == o) { *p = o->next; break; }
}

static void freeObj(Obj *o) {
	for (Conn *c = o->conns, *n; c; c = n) { n = c->next; free(c); }
	if (o->props) CFRelease(o->props);
	if (o->readBlock) Block_release(o->readBlock);
	if (o->recvBlock) Block_release(o->recvBlock);
	if (o->notifyBlock) Block_release(o->notifyBlock);
	free(o);
}

// ---------------------------------------------------------------- Benachrichtigungen (Haupt-Runloop)
typedef struct { MIDIClientRef client; MIDINotificationMessageID id; MIDIObjectRef obj; MIDIObjectType type; } Note;

static void deliverNotes(Note *n, int count) {
	for (int i = 0; i < count; i++) {
		pthread_mutex_lock(&lk);
		Obj *c = find(n[i].client);
		MIDINotifyProc proc = c ? c->notify : NULL; void *ref = c ? c->notifyRef : NULL;
		MIDINotifyBlock blk = c && c->notifyBlock ? Block_copy(c->notifyBlock) : NULL;
		pthread_mutex_unlock(&lk);
		if (!proc && !blk) continue;
		if (n[i].id == kMIDIMsgSetupChanged) {
			MIDINotification m = { kMIDIMsgSetupChanged, sizeof(MIDINotification) };
			if (proc) proc(&m, ref);
			if (blk) blk(&m);
		} else {
			MIDIObjectAddRemoveNotification m = { n[i].id, sizeof m, 0, kMIDIObjectType_Other, n[i].obj, n[i].type };
			if (proc) proc((const MIDINotification *)&m, ref);
			if (blk) blk((const MIDINotification *)&m);
		}
		if (blk) Block_release(blk);
	}
}

// meldet allen Clients außer dem Besitzer, dass ein Endpunkt hinzukam oder wegfiel, danach SetupChanged
static void announce(MIDIObjectRef obj, MIDIObjectType type, int added) {
	int n = 0;
	pthread_mutex_lock(&lk);
	for (Obj *o = objs; o; o = o->next) if (o->kind == O_CLIENT) n++;
	Note *v = calloc(n * 2 + 1, sizeof *v); int k = 0;
	for (Obj *o = objs; o; o = o->next) if (o->kind == O_CLIENT) {
		v[k++] = (Note){ o->ref, added ? kMIDIMsgObjectAdded : kMIDIMsgObjectRemoved, obj, type };
		v[k++] = (Note){ o->ref, kMIDIMsgSetupChanged, 0, 0 };
	}
	pthread_mutex_unlock(&lk);
	if (!k) { free(v); return; }
	CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, ^{ deliverNotes(v, k); free(v); });
	CFRunLoopWakeUp(CFRunLoopGetMain());
}

// ---------------------------------------------------------------- MIDI 1.0 <-> UMP
#ifndef paramErr
#define paramErr (-50)
#endif
// Hochskalieren nach der MIDI-2.0-Spezifikation (Min-Center-Max), Werte wie Apples MIDIUInt7ToUInt16 usw.
static UInt16 up7to16(Byte v) {
	UInt16 r = (UInt16)v << 9;
	if (v <= 64) return r;
	UInt16 rep = v & 0x3F;
	return r | (UInt16)(rep << 3) | (UInt16)(rep >> 3);
}
static UInt32 up7to32(Byte v) {
	UInt32 r = (UInt32)v << 25;
	if (v <= 64) return r;
	UInt32 rep = v & 0x3F;
	return r | rep << 19 | rep << 13 | rep << 7 | rep << 1 | rep >> 5;
}
static UInt32 up14to32(UInt16 v) {
	UInt32 r = (UInt32)v << 18;
	if (v <= 0x2000) return r;
	UInt32 rep = v & 0x1FFF;
	return r | rep << 5 | rep >> 8;
}
static int msgLen(Byte st) {
	if (st < 0xF0) { switch (st & 0xF0) { case 0xC0: case 0xD0: return 2; default: return 3; } }
	switch (st) { case 0xF1: case 0xF3: return 2; case 0xF2: return 3; case 0xF6: case 0xF8: case 0xFA: case 0xFB: case 0xFC: case 0xFE: case 0xFF: return 1; }
	return 1;
}

// MIDI-1.0-Bytes eines Pakets als UMP-Wörter (Gruppe 0); proto 2.0: Kanalnachrichten als MIDI-2.0-Kanalnachrichten
static int bytesToUMP(const Byte *d, int len, MIDIProtocolID proto, UInt32 *w, int max) {
	int n = 0, i = 0;
	while (i < len && n + 2 <= max) {
		Byte st = d[i];
		if (st == 0xF0) {                 // SysEx: 7-Bit-Daten in 64-Bit-Paketen (Typ 3)
			int j = i + 1; while (j < len && d[j] != 0xF7) j++;
			const Byte *p = d + i + 1; int cnt = j - (i + 1), first = 1;
			do {
				int chunk = cnt > 6 ? 6 : cnt; int status;
				if (first && cnt <= 6) status = 0; else if (first) status = 1; else if (cnt <= 6) status = 3; else status = 2;
				Byte b[6] = {0}; memcpy(b, p, chunk);
				if (n + 2 > max) break;
				w[n++] = (0x3u << 28) | ((UInt32)status << 20) | ((UInt32)chunk << 16) | ((UInt32)b[0] << 8) | b[1];
				w[n++] = ((UInt32)b[2] << 24) | ((UInt32)b[3] << 16) | ((UInt32)b[4] << 8) | b[5];
				p += chunk; cnt -= chunk; first = 0;
			} while (cnt > 0);
			i = j < len ? j + 1 : len;
			continue;
		}
		if (!(st & 0x80)) { i++; continue; }      // Daten ohne Status (laufender Status kommt in Paketlisten nicht vor)
		int l = msgLen(st);
		Byte d1 = i + 1 < len && l > 1 ? d[i + 1] : 0, d2 = i + 2 < len && l > 2 ? d[i + 2] : 0;
		if (st >= 0xF0) {
			w[n++] = (0x1u << 28) | ((UInt32)st << 16) | ((UInt32)d1 << 8) | d2;
		} else if (proto == kMIDIProtocol_2_0) {
			Byte op = st & 0xF0, ch = st & 0x0F;
			UInt32 w0 = (0x4u << 28) | ((UInt32)op << 16) | ((UInt32)ch << 16), w1 = 0;
			switch (op) {
				case 0x80: case 0x90: w0 |= (UInt32)d1 << 8; w1 = (UInt32)(d2 ? up7to16(d2) : 0) << 16;
					if (op == 0x90 && d2 == 0) { w0 = (w0 & ~0x00F00000u) | 0x00800000u; w1 = (UInt32)up7to16(64) << 16; }
					break;
				case 0xA0: case 0xB0: w0 |= (UInt32)d1 << 8; w1 = up7to32(d2); break;
				case 0xC0: w1 = (UInt32)d1 << 24; break;
				case 0xD0: w1 = up7to32(d1); break;
				case 0xE0: w1 = up14to32((UInt16)(d1 | (d2 << 7))); break;
			}
			w[n++] = w0; w[n++] = w1;
		} else {
			w[n++] = (0x2u << 28) | ((UInt32)st << 16) | ((UInt32)d1 << 8) | d2;
		}
		i += l;
	}
	return n;
}

// UMP-Wörter als MIDI-1.0-Bytes (MIDI-2.0-Kanalnachrichten werden heruntergerechnet); gibt Byte-Anzahl zurück
static int umpToBytes(const UInt32 *w, int count, Byte *out, int max) {
	int n = 0;
	for (int i = 0; i < count; ) {
		UInt32 x = w[i]; int type = x >> 28;
		static const int umpWords[16] = { 1, 1, 1, 2, 2, 4, 1, 1, 2, 2, 2, 3, 3, 4, 4, 4 };
		int words = umpWords[type];
		if (i + words > count) break;
		Byte st = (x >> 16) & 0xFF, d1 = (x >> 8) & 0x7F, d2 = x & 0x7F;
		if (type == 1 || type == 2) {
			int l = msgLen(st);
			if (n + l > max) break;
			out[n++] = st; if (l > 1) out[n++] = d1; if (l > 2) out[n++] = d2;
		} else if (type == 3) {
			int status = (x >> 20) & 0xF, cnt = (x >> 16) & 0xF; if (cnt > 6) cnt = 6;
			Byte b[6] = { (x >> 8) & 0x7F, x & 0x7F, (w[i + 1] >> 24) & 0x7F, (w[i + 1] >> 16) & 0x7F, (w[i + 1] >> 8) & 0x7F, w[i + 1] & 0x7F };
			if (n + cnt + 2 > max) break;
			if (status == 0 || status == 1) out[n++] = 0xF0;
			memcpy(out + n, b, cnt); n += cnt;
			if (status == 0 || status == 3) out[n++] = 0xF7;
		} else if (type == 4) {
			UInt32 y = w[i + 1]; Byte op = st & 0xF0, ch = st & 0x0F;
			if (n + 3 > max) break;
			switch (op) {
				case 0x80: case 0x90: { Byte v = (Byte)(y >> 25); if (op == 0x90 && v == 0) v = 1;
					out[n++] = op | ch; out[n++] = d1; out[n++] = v; break; }
				case 0xA0: case 0xB0: out[n++] = op | ch; out[n++] = d1; out[n++] = (Byte)(y >> 25); break;
				case 0xC0: out[n++] = 0xC0 | ch; out[n++] = (Byte)(y >> 24) & 0x7F; break;
				case 0xD0: out[n++] = 0xD0 | ch; out[n++] = (Byte)(y >> 25); break;
				case 0xE0: { UInt32 v = y >> 18; out[n++] = 0xE0 | ch; out[n++] = v & 0x7F; out[n++] = (v >> 7) & 0x7F; break; }
			}
		}
		i += words;
	}
	return n;
}

// Umwandeln mit erhaltenen Paketgrenzen (wie Apple; MIDIPacketListAdd würde gleiche Zeitstempel zusammenlegen)
static MIDIPacket *appendPkt(MIDIPacketList *pl, MIDIPacket *cur, Byte *end, MIDITimeStamp t, int n, const Byte *d) {
	if (pl->numPackets) cur = MIDIPacketNext(cur);
	if ((Byte *)&cur->data[n] > end) return NULL;
	cur->timeStamp = t; cur->length = (UInt16)n; memcpy(cur->data, d, n); pl->numPackets++;
	return cur;
}
static MIDIEventPacket *appendEvt(MIDIEventList *el, MIDIEventPacket *cur, Byte *end, MIDITimeStamp t, int n, const UInt32 *w) {
	if (el->numPackets) cur = MIDIEventPacketNext(cur);
	if (n > 64 || (Byte *)&cur->words[n] > end) return NULL;
	cur->timeStamp = t; cur->wordCount = (UInt32)n; memcpy(cur->words, w, n * 4); el->numPackets++;
	return cur;
}

// Ergebnis mit free() freigeben
static MIDIPacketList *evtToPkt(const MIDIEventList *el) {
	size_t sz = 64 + (size_t)el->numPackets * (sizeof(MIDIPacket) + 16);
	MIDIPacketList *pl = malloc(sz); MIDIPacket *p = MIDIPacketListInit(pl);
	const MIDIEventPacket *ep = &el->packet[0];
	for (UInt32 i = 0; i < el->numPackets && p; i++) {
		Byte b[256]; int n = umpToBytes(ep->words, ep->wordCount, b, sizeof b);
		if (n) p = appendPkt(pl, p, (Byte *)pl + sz, ep->timeStamp, n, b);
		ep = MIDIEventPacketNext(ep);
	}
	return pl;
}

static MIDIEventList *pktToEvt(const MIDIPacketList *pl, MIDIProtocolID proto) {
	size_t sz = 64 + (size_t)pl->numPackets * sizeof(MIDIEventPacket) * 4;
	MIDIEventList *el = malloc(sz); MIDIEventPacket *ep = MIDIEventListInit(el, proto);
	const MIDIPacket *p = &pl->packet[0];
	for (UInt32 i = 0; i < pl->numPackets && ep; i++) {
		UInt32 w[256]; int n = bytesToUMP(p->data, p->length, proto, w, 256);
		for (int k = 0; k < n && ep; k += 64)   // mehr als 64 Wörter (lange SysEx): auf Pakete verteilen
			ep = appendEvt(el, ep, (Byte *)el + sz, p->timeStamp, n - k > 64 ? 64 : n - k, w + k);
		p = MIDIPacketNext(p);
	}
	return el;
}

static size_t pktSize(const MIDIPacketList *pl) {
	const MIDIPacket *p = &pl->packet[0];
	for (UInt32 i = 0; i < pl->numPackets; i++) p = MIDIPacketNext(p);
	return (size_t)((const Byte *)p - (const Byte *)pl) + sizeof(MIDIPacket);
}
static size_t evtSize(const MIDIEventList *el) {
	const MIDIEventPacket *p = &el->packet[0];
	for (UInt32 i = 0; i < el->numPackets; i++) p = MIDIEventPacketNext(p);
	return (size_t)((const Byte *)p - (const Byte *)el) + sizeof(MIDIEventPacket);
}

// ---------------------------------------------------------------- Zustellung
// Wie bei Apple auf einem eigenen Thread (der "MIDI-Empfangsthread"), in Reihenfolge, damit MIDIReceived/MIDISend
// sofort zurückkehren und ein Programm, das zugleich Quelle und Empfänger ist, sich nicht selbst blockiert.
typedef struct { MIDIReadProc read; void *readRef; MIDIReadBlock readBlock; MIDIReceiveBlock recvBlock; MIDIProtocolID proto; void *connRef; } Target;
typedef struct Job { Target *t; int n; MIDIPacketList *pl; MIDIEventList *el; struct Job *next; } Job;

static void deliver(Target *t, int n, const MIDIPacketList *pl, const MIDIEventList *el) {
	for (int i = 0; i < n; i++) {
		if (t[i].recvBlock) {                     // Empfänger will Eventlisten
			if (el && el->protocol == t[i].proto) t[i].recvBlock(el, t[i].connRef);
			else {
				MIDIPacketList *tmp = el ? evtToPkt(el) : NULL;   // anderes Protokoll: über MIDI 1.0 umrechnen
				MIDIEventList *e = pktToEvt(tmp ? tmp : pl, t[i].proto);
				t[i].recvBlock(e, t[i].connRef); free(e); free(tmp);
			}
			Block_release(t[i].recvBlock);
		} else {
			MIDIPacketList *tmp = el ? evtToPkt(el) : NULL;
			const MIDIPacketList *use = tmp ? tmp : pl;
			if (t[i].read) t[i].read(use, t[i].readRef, t[i].connRef);
			if (t[i].readBlock) { t[i].readBlock(use, t[i].connRef); Block_release(t[i].readBlock); }
			free(tmp);
		}
	}
}

static pthread_mutex_t qlk = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t qcv = PTHREAD_COND_INITIALIZER;
static Job *qhead, *qtail;
static int qthread;

static void *receiver(void *arg) {
	(void)arg;
	pthread_setname_np("com.apple.coremidi.receive");
	for (;;) {
		pthread_mutex_lock(&qlk);
		while (!qhead) pthread_cond_wait(&qcv, &qlk);
		Job *j = qhead; qhead = j->next; if (!qhead) qtail = NULL;
		pthread_mutex_unlock(&qlk);
		deliver(j->t, j->n, j->pl, j->el);
		free(j->t); free(j->pl); free(j->el); free(j);
	}
	return NULL;
}

static void enqueue(Target *t, int n, const MIDIPacketList *pl, const MIDIEventList *el) {
	if (!n) { free(t); return; }
	Job *j = calloc(1, sizeof *j); j->t = t; j->n = n;
	if (pl) { size_t sz = pktSize(pl); j->pl = malloc(sz); memcpy(j->pl, pl, sz); }
	if (el) { size_t sz = evtSize(el); j->el = malloc(sz); memcpy(j->el, el, sz); }
	pthread_mutex_lock(&qlk);
	if (!qthread) {
		pthread_t th; pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
		struct sched_param sp = { .sched_priority = 47 }; pthread_attr_setschedparam(&a, &sp);
		if (pthread_create(&th, &a, receiver, NULL) == 0) qthread = 1;
		pthread_attr_destroy(&a);
	}
	if (qtail) qtail->next = j; else qhead = j;
	qtail = j;
	pthread_cond_signal(&qcv);
	pthread_mutex_unlock(&qlk);
	if (!qthread) {                               // ohne Thread (sollte nicht vorkommen): direkt
		pthread_mutex_lock(&qlk); qhead = qtail = NULL; pthread_mutex_unlock(&qlk);
		deliver(j->t, j->n, j->pl, j->el); free(j->t); free(j->pl); free(j->el); free(j);
	}
}

static Target fromObj(Obj *o, void *connRef) {
	Target t = { o->read, o->readRef, o->readBlock ? Block_copy(o->readBlock) : NULL, o->recvBlock ? Block_copy(o->recvBlock) : NULL, o->protocol, connRef };
	return t;
}

// an alle Eingangsports, die mit der Quelle verbunden sind (in der Reihenfolge, in der die Ports angelegt wurden)
static OSStatus fromSource(MIDIEndpointRef src, const MIDIPacketList *pl, const MIDIEventList *el) {
	pthread_mutex_lock(&lk);
	Obj *s = find(src);
	if (!s || s->kind != O_SOURCE) { pthread_mutex_unlock(&lk); return s ? kMIDIWrongEndpointType : kMIDIUnknownEndpoint; }
	int n = 0;
	for (Obj *o = objs; o; o = o->next) if (o->kind == O_INPORT) for (Conn *c = o->conns; c; c = c->next) if (c->src == src) n++;
	Target *t = calloc(n + 1, sizeof *t); int k = n;
	for (Obj *o = objs; o; o = o->next) if (o->kind == O_INPORT) for (Conn *c = o->conns; c; c = c->next) if (c->src == src) t[--k] = fromObj(o, c->refcon);
	pthread_mutex_unlock(&lk);
	enqueue(t, n, pl, el);
	return noErr;
}

static OSStatus toDest(MIDIPortRef port, MIDIEndpointRef dest, const MIDIPacketList *pl, const MIDIEventList *el) {
	pthread_mutex_lock(&lk);
	Obj *p = find(port), *d = find(dest);
	// wie bei Apple: ein falscher Port-Typ oder eine Quelle als Ziel ist kein Fehler, es geschieht nur nichts
	if (!p || (p->kind != O_OUTPORT && p->kind != O_INPORT)) { pthread_mutex_unlock(&lk); return kMIDIInvalidPort; }
	if (!d) { pthread_mutex_unlock(&lk); return kMIDIUnknownEndpoint; }
	if (d->kind != O_DEST || p->kind != O_OUTPORT) { pthread_mutex_unlock(&lk); return noErr; }
	Target *t = malloc(sizeof *t); *t = fromObj(d, NULL);
	pthread_mutex_unlock(&lk);
	enqueue(t, 1, pl, el);
	return noErr;
}

// ---------------------------------------------------------------- Clients
static OSStatus clientCreate(CFStringRef name, MIDINotifyProc proc, void *ref, MIDINotifyBlock blk, MIDIClientRef *out) {
	if (!out) return kMIDIInvalidClient;
	pthread_mutex_lock(&lk);
	Obj *o = mk(O_CLIENT, 0, name);
	o->notify = proc; o->notifyRef = ref; o->notifyBlock = blk ? Block_copy(blk) : NULL;
	*out = o->ref;
	pthread_mutex_unlock(&lk);
	tr("MIDIClientCreate -> %#x", (unsigned)*out);
	return noErr;
}
EXPORT OSStatus MIDIClientCreate(CFStringRef name, MIDINotifyProc notifyProc, void *notifyRefCon, MIDIClientRef *outClient) {
	return clientCreate(name, notifyProc, notifyRefCon, NULL, outClient);
}
EXPORT OSStatus MIDIClientCreateWithBlock(CFStringRef name, MIDIClientRef *outClient, MIDINotifyBlock notifyBlock) {
	return clientCreate(name, NULL, NULL, notifyBlock, outClient);
}

static void disposeLocked(Obj *o, MIDIObjectRef *gone, MIDIObjectType *goneType, int *ng) {
	if (o->kind == O_SOURCE || o->kind == O_DEST) {
		gone[*ng] = o->ref; goneType[*ng] = o->kind == O_SOURCE ? kMIDIObjectType_Source : kMIDIObjectType_Destination; (*ng)++;
		// Verbindungen von Eingangsports zu dieser Quelle lösen
		for (Obj *p = objs; p; p = p->next) if (p->kind == O_INPORT)
			for (Conn **c = &p->conns; *c; ) { if ((*c)->src == o->ref) { Conn *x = *c; *c = x->next; free(x); } else c = &(*c)->next; }
	}
	unlink_obj(o); freeObj(o);
}

EXPORT OSStatus MIDIClientDispose(MIDIClientRef client) {
	MIDIObjectRef gone[256]; MIDIObjectType gt[256]; int ng = 0;
	pthread_mutex_lock(&lk);
	Obj *c = find(client);
	if (!c || c->kind != O_CLIENT) { pthread_mutex_unlock(&lk); return kMIDIInvalidClient; }
	for (Obj *o = objs, *n; o; o = n) { n = o->next; if (o->client == client && ng < 255) disposeLocked(o, gone, gt, &ng); }
	unlink_obj(c); freeObj(c);
	pthread_mutex_unlock(&lk);
	for (int i = 0; i < ng; i++) announce(gone[i], gt[i], 0);
	return noErr;
}

// ---------------------------------------------------------------- Ports
static OSStatus portCreate(MIDIClientRef client, CFStringRef name, int kind, MIDIProtocolID proto, MIDIReadProc read, void *ref,
                           MIDIReadBlock rb, MIDIReceiveBlock eb, MIDIPortRef *out) {
	if (!out) return kMIDIInvalidPort;
	pthread_mutex_lock(&lk);
	Obj *c = find(client);
	if (!c || c->kind != O_CLIENT) { pthread_mutex_unlock(&lk); return kMIDIInvalidClient; }
	Obj *o = mk(kind, client, name);
	o->protocol = proto; o->read = read; o->readRef = ref;
	o->readBlock = rb ? Block_copy(rb) : NULL; o->recvBlock = eb ? Block_copy(eb) : NULL;
	*out = o->ref;
	pthread_mutex_unlock(&lk);
	tr("port %s -> %#x", kind == O_INPORT ? "in" : "out", (unsigned)*out);
	return noErr;
}
EXPORT OSStatus MIDIInputPortCreate(MIDIClientRef client, CFStringRef portName, MIDIReadProc readProc, void *refCon, MIDIPortRef *outPort) {
	return portCreate(client, portName, O_INPORT, 0, readProc, refCon, NULL, NULL, outPort);
}
EXPORT OSStatus MIDIInputPortCreateWithBlock(MIDIClientRef client, CFStringRef portName, MIDIPortRef *outPort, MIDIReadBlock readBlock) {
	return portCreate(client, portName, O_INPORT, 0, NULL, NULL, readBlock, NULL, outPort);
}
EXPORT OSStatus MIDIInputPortCreateWithProtocol(MIDIClientRef client, CFStringRef portName, MIDIProtocolID protocol, MIDIPortRef *outPort, MIDIReceiveBlock receiveBlock) {
	return portCreate(client, portName, O_INPORT, protocol, NULL, NULL, NULL, receiveBlock, outPort);
}
EXPORT OSStatus MIDIOutputPortCreate(MIDIClientRef client, CFStringRef portName, MIDIPortRef *outPort) {
	return portCreate(client, portName, O_OUTPORT, 0, NULL, NULL, NULL, NULL, outPort);
}
EXPORT OSStatus MIDIPortDispose(MIDIPortRef port) {
	pthread_mutex_lock(&lk);
	Obj *o = find(port);
	if (!o || (o->kind != O_INPORT && o->kind != O_OUTPORT)) { pthread_mutex_unlock(&lk); return kMIDIInvalidPort; }
	unlink_obj(o); freeObj(o);
	pthread_mutex_unlock(&lk);
	return noErr;
}
EXPORT OSStatus MIDIPortConnectSource(MIDIPortRef port, MIDIEndpointRef source, void *connRefCon) {
	pthread_mutex_lock(&lk);
	Obj *p = find(port), *s = find(source);
	OSStatus r = noErr;
	if (!p || p->kind != O_INPORT) r = kMIDIInvalidPort;
	else if (!s) r = kMIDIUnknownEndpoint;
	else if (s->kind != O_SOURCE) r = kMIDIWrongEndpointType;
	else {
		Conn *c = calloc(1, sizeof *c); c->src = source; c->refcon = connRefCon; c->next = p->conns; p->conns = c;
	}
	pthread_mutex_unlock(&lk);
	tr("MIDIPortConnectSource %#x %#x -> %d", (unsigned)port, (unsigned)source, (int)r);
	return r;
}
EXPORT OSStatus MIDIPortDisconnectSource(MIDIPortRef port, MIDIEndpointRef source) {
	pthread_mutex_lock(&lk);
	Obj *p = find(port);
	OSStatus r = kMIDINoConnection;
	if (!p || p->kind != O_INPORT) r = kMIDIInvalidPort;
	else for (Conn **c = &p->conns; *c; c = &(*c)->next) if ((*c)->src == source) { Conn *x = *c; *c = x->next; free(x); r = noErr; break; }
	pthread_mutex_unlock(&lk);
	return r;
}

// ---------------------------------------------------------------- Endpunkte
static OSStatus endpointCreate(MIDIClientRef client, CFStringRef name, int kind, MIDIProtocolID proto, MIDIReadProc read, void *ref,
                               MIDIReadBlock rb, MIDIReceiveBlock eb, MIDIEndpointRef *out) {
	if (!out) return kMIDIUnknownEndpoint;
	pthread_mutex_lock(&lk);
	Obj *c = find(client);
	if (!c || c->kind != O_CLIENT) { pthread_mutex_unlock(&lk); return kMIDIInvalidClient; }
	Obj *o = mk(kind, client, name);
	o->protocol = proto; o->read = read; o->readRef = ref;
	o->readBlock = rb ? Block_copy(rb) : NULL; o->recvBlock = eb ? Block_copy(eb) : NULL;
	if (proto) setInt(o->props, kMIDIPropertyProtocolID, proto);
	*out = o->ref;
	pthread_mutex_unlock(&lk);
	tr("virtual %s -> %#x", kind == O_SOURCE ? "source" : "destination", (unsigned)*out);
	announce(*out, kind == O_SOURCE ? kMIDIObjectType_Source : kMIDIObjectType_Destination, 1);
	return noErr;
}
EXPORT OSStatus MIDISourceCreate(MIDIClientRef client, CFStringRef name, MIDIEndpointRef *outSrc) {
	return endpointCreate(client, name, O_SOURCE, 0, NULL, NULL, NULL, NULL, outSrc);
}
EXPORT OSStatus MIDISourceCreateWithProtocol(MIDIClientRef client, CFStringRef name, MIDIProtocolID protocol, MIDIEndpointRef *outSrc) {
	return endpointCreate(client, name, O_SOURCE, protocol, NULL, NULL, NULL, NULL, outSrc);
}
EXPORT OSStatus MIDIDestinationCreate(MIDIClientRef client, CFStringRef name, MIDIReadProc readProc, void *refCon, MIDIEndpointRef *outDest) {
	return endpointCreate(client, name, O_DEST, 0, readProc, refCon, NULL, NULL, outDest);
}
EXPORT OSStatus MIDIDestinationCreateWithBlock(MIDIClientRef client, CFStringRef name, MIDIEndpointRef *outDest, MIDIReadBlock readBlock) {
	return endpointCreate(client, name, O_DEST, 0, NULL, NULL, readBlock, NULL, outDest);
}
EXPORT OSStatus MIDIDestinationCreateWithProtocol(MIDIClientRef client, CFStringRef name, MIDIProtocolID protocol, MIDIEndpointRef *outDest, MIDIReceiveBlock readBlock) {
	return endpointCreate(client, name, O_DEST, protocol, NULL, NULL, NULL, readBlock, outDest);
}
EXPORT OSStatus MIDIEndpointDispose(MIDIEndpointRef endpt) {
	MIDIObjectRef gone[1]; MIDIObjectType gt[1]; int ng = 0;
	pthread_mutex_lock(&lk);
	Obj *o = find(endpt);
	if (!o || (o->kind != O_SOURCE && o->kind != O_DEST)) { pthread_mutex_unlock(&lk); return kMIDIUnknownEndpoint; }
	disposeLocked(o, gone, gt, &ng);
	pthread_mutex_unlock(&lk);
	if (ng) announce(gone[0], gt[0], 0);
	return noErr;
}

static ItemCount countKind(int kind) {
	ItemCount n = 0;
	pthread_mutex_lock(&lk);
	for (Obj *o = objs; o; o = o->next) if (o->kind == kind) n++;
	pthread_mutex_unlock(&lk);
	return n;
}
// Reihenfolge wie beim Anlegen (die Liste ist neueste zuerst)
static MIDIEndpointRef nthKind(int kind, ItemCount idx) {
	MIDIEndpointRef r = 0; ItemCount n = 0;
	pthread_mutex_lock(&lk);
	for (Obj *o = objs; o; o = o->next) if (o->kind == kind) n++;
	if (idx < n) { ItemCount want = n - 1 - idx, k = 0; for (Obj *o = objs; o; o = o->next) if (o->kind == kind) { if (k == want) { r = o->ref; break; } k++; } }
	pthread_mutex_unlock(&lk);
	return r;
}
EXPORT ItemCount MIDIGetNumberOfSources(void) { return countKind(O_SOURCE); }
EXPORT ItemCount MIDIGetNumberOfDestinations(void) { return countKind(O_DEST); }
EXPORT MIDIEndpointRef MIDIGetSource(ItemCount i) { return nthKind(O_SOURCE, i); }
EXPORT MIDIEndpointRef MIDIGetDestination(ItemCount i) { return nthKind(O_DEST, i); }

// Geräte und Entities: keine Treiber, also keine Geräte; virtuelle Endpunkte haben keine Entity
EXPORT ItemCount MIDIGetNumberOfDevices(void) { return 0; }
EXPORT MIDIDeviceRef MIDIGetDevice(ItemCount i) { (void)i; return 0; }
EXPORT ItemCount MIDIGetNumberOfExternalDevices(void) { return 0; }
EXPORT MIDIDeviceRef MIDIGetExternalDevice(ItemCount i) { (void)i; return 0; }
EXPORT ItemCount MIDIDeviceGetNumberOfEntities(MIDIDeviceRef d) { (void)d; return 0; }
EXPORT MIDIEntityRef MIDIDeviceGetEntity(MIDIDeviceRef d, ItemCount i) { (void)d; (void)i; return 0; }
EXPORT ItemCount MIDIEntityGetNumberOfSources(MIDIEntityRef e) { (void)e; return 0; }
EXPORT MIDIEndpointRef MIDIEntityGetSource(MIDIEntityRef e, ItemCount i) { (void)e; (void)i; return 0; }
EXPORT ItemCount MIDIEntityGetNumberOfDestinations(MIDIEntityRef e) { (void)e; return 0; }
EXPORT MIDIEndpointRef MIDIEntityGetDestination(MIDIEntityRef e, ItemCount i) { (void)e; (void)i; return 0; }
EXPORT OSStatus MIDIEntityGetDevice(MIDIEntityRef e, MIDIDeviceRef *out) { (void)e; if (out) *out = 0; return kMIDIObjectNotFound; }
EXPORT OSStatus MIDIEndpointGetEntity(MIDIEndpointRef ep, MIDIEntityRef *out) {
	if (out) *out = 0;
	pthread_mutex_lock(&lk); Obj *o = find(ep); pthread_mutex_unlock(&lk);
	return o && (o->kind == O_SOURCE || o->kind == O_DEST) ? kMIDIObjectNotFound : kMIDIUnknownEndpoint;
}

// ---------------------------------------------------------------- Senden und Empfangen
EXPORT OSStatus MIDISend(MIDIPortRef port, MIDIEndpointRef dest, const MIDIPacketList *pktlist) {
	if (!pktlist) return paramErr;
	return toDest(port, dest, pktlist, NULL);
}
EXPORT OSStatus MIDISendEventList(MIDIPortRef port, MIDIEndpointRef dest, const MIDIEventList *evtlist) {
	if (!evtlist) return paramErr;
	return toDest(port, dest, NULL, evtlist);
}
EXPORT OSStatus MIDIReceived(MIDIEndpointRef src, const MIDIPacketList *pktlist) {
	if (!pktlist) return paramErr;
	return fromSource(src, pktlist, NULL);
}
EXPORT OSStatus MIDIReceivedEventList(MIDIEndpointRef src, const MIDIEventList *evtlist) {
	if (!evtlist) return paramErr;
	return fromSource(src, NULL, evtlist);
}
EXPORT OSStatus MIDISendSysex(MIDISysexSendRequest *req) {
	if (!req) return paramErr;
	Byte buf[1024 + 64]; MIDIPacketList *pl = (MIDIPacketList *)buf;
	while (req->bytesToSend > 0) {
		UInt32 chunk = req->bytesToSend > 1024 ? 1024 : req->bytesToSend;
		MIDIPacket *p = MIDIPacketListInit(pl);
		MIDIPacketListAdd(pl, sizeof buf, p, 0, chunk, req->data);
		pthread_mutex_lock(&lk); Obj *d = find(req->destination);
		Target *t = NULL; if (d && d->kind == O_DEST) { t = malloc(sizeof *t); *t = fromObj(d, NULL); }
		pthread_mutex_unlock(&lk);
		if (t) enqueue(t, 1, pl, NULL);
		req->data += chunk; req->bytesToSend -= chunk;
	}
	req->complete = true;
	if (req->completionProc) req->completionProc(req);
	return noErr;
}
EXPORT OSStatus MIDIFlushOutput(MIDIEndpointRef dest) { (void)dest; return noErr; }
EXPORT OSStatus MIDIRestart(void) { return noErr; }

// ---------------------------------------------------------------- Paket- und Eventlisten (wie Apples Funktionen)
EXPORT MIDIPacket *MIDIPacketListInit(MIDIPacketList *pktlist) {
	pktlist->numPackets = 0;
	return &pktlist->packet[0];
}

EXPORT MIDIPacket *MIDIPacketListAdd(MIDIPacketList *pktlist, ByteCount listSize, MIDIPacket *cur, MIDITimeStamp time, ByteCount nData, const Byte *data) {
	if (!pktlist || !cur || (nData && !data)) return NULL;
	Byte *end = (Byte *)pktlist + listSize;
	int sysex = nData && (data[0] == 0xF0 || !(data[0] & 0x80));
	if (pktlist->numPackets) {
		// gleicher Zeitstempel, keine SysEx in beiden: an das laufende Paket anhängen
		Byte last0 = cur->length ? cur->data[0] : 0;
		if (cur->timeStamp == time && !sysex && cur->length && last0 != 0xF0 && (last0 & 0x80) &&
		    cur->length + nData <= 256 && &cur->data[cur->length + nData] <= end) {
			memcpy(&cur->data[cur->length], data, nData); cur->length += (UInt16)nData;
			return cur;
		}
		cur = MIDIPacketNext(cur);
	}
	if ((Byte *)&cur->data[nData] > end || nData > 65535) return NULL;
	cur->timeStamp = time; cur->length = (UInt16)nData;
	if (nData) memcpy(cur->data, data, nData);
	pktlist->numPackets++;
	return cur;
}

EXPORT MIDIEventPacket *MIDIEventListInit(MIDIEventList *evtlist, MIDIProtocolID protocol) {
	evtlist->protocol = protocol; evtlist->numPackets = 0;
	return &evtlist->packet[0];
}

EXPORT MIDIEventPacket *MIDIEventListAdd(MIDIEventList *evtlist, ByteCount listSize, MIDIEventPacket *cur, MIDITimeStamp time, ByteCount wordCount, const UInt32 *words) {
	if (!evtlist || !cur || (wordCount && !words)) return NULL;
	Byte *end = (Byte *)evtlist + listSize;
	if (evtlist->numPackets) {
		if (cur->timeStamp == time && cur->wordCount + wordCount <= 64 && (Byte *)&cur->words[cur->wordCount + wordCount] <= end) {
			memcpy(&cur->words[cur->wordCount], words, wordCount * 4); cur->wordCount += (UInt32)wordCount;
			return cur;
		}
		cur = MIDIEventPacketNext(cur);
	}
	if (wordCount > 64 || (Byte *)&cur->words[wordCount] > end) return NULL;
	cur->timeStamp = time; cur->wordCount = (UInt32)wordCount;
	memcpy(cur->words, words, wordCount * 4);
	evtlist->numPackets++;
	return cur;
}

// ---------------------------------------------------------------- Eigenschaften
static OSStatus getProp(MIDIObjectRef obj, CFStringRef key, CFTypeRef *out) {
	*out = NULL;
	pthread_mutex_lock(&lk);
	Obj *o = find(obj);
	if (!o) { pthread_mutex_unlock(&lk); return kMIDIObjectNotFound; }
	CFTypeRef v = key ? CFDictionaryGetValue(o->props, key) : NULL;
	// displayName fällt auf name zurück (virtuelle Endpunkte haben keine Geräte, deren Namen man voranstellen müsste)
	if (!v && key && CFEqual(key, kMIDIPropertyDisplayName)) v = CFDictionaryGetValue(o->props, kMIDIPropertyName);
	if (v) CFRetain(v);
	pthread_mutex_unlock(&lk);
	if (!v) return kMIDIUnknownProperty;
	*out = v;
	return noErr;
}

EXPORT OSStatus MIDIObjectGetIntegerProperty(MIDIObjectRef obj, CFStringRef propertyID, SInt32 *outValue) {
	CFTypeRef v; OSStatus r = getProp(obj, propertyID, &v);
	if (r) return r;
	if (CFGetTypeID(v) != CFNumberGetTypeID()) { CFRelease(v); return kMIDIWrongPropertyType; }
	CFNumberGetValue(v, kCFNumberSInt32Type, outValue); CFRelease(v);
	return noErr;
}
EXPORT OSStatus MIDIObjectSetIntegerProperty(MIDIObjectRef obj, CFStringRef propertyID, SInt32 value) {
	pthread_mutex_lock(&lk);
	Obj *o = find(obj);
	if (o && propertyID) setInt(o->props, propertyID, value);
	pthread_mutex_unlock(&lk);
	return o ? noErr : kMIDIObjectNotFound;
}
EXPORT OSStatus MIDIObjectGetStringProperty(MIDIObjectRef obj, CFStringRef propertyID, CFStringRef *str) {
	CFTypeRef v; OSStatus r = getProp(obj, propertyID, &v);
	if (r) { if (str) *str = NULL; return r; }
	if (CFGetTypeID(v) != CFStringGetTypeID()) { CFRelease(v); return kMIDIWrongPropertyType; }
	*str = v;                                   // der Aufrufer gibt frei (Copy-Semantik wie bei Apple)
	return noErr;
}
EXPORT OSStatus MIDIObjectSetStringProperty(MIDIObjectRef obj, CFStringRef propertyID, CFStringRef str) {
	pthread_mutex_lock(&lk);
	Obj *o = find(obj);
	if (o && propertyID && str) CFDictionarySetValue(o->props, propertyID, str);
	pthread_mutex_unlock(&lk);
	return o ? noErr : kMIDIObjectNotFound;
}
EXPORT OSStatus MIDIObjectGetDataProperty(MIDIObjectRef obj, CFStringRef propertyID, CFDataRef *outData) {
	CFTypeRef v; OSStatus r = getProp(obj, propertyID, &v);
	if (r) { if (outData) *outData = NULL; return r; }
	if (CFGetTypeID(v) != CFDataGetTypeID()) { CFRelease(v); return kMIDIWrongPropertyType; }
	*outData = v;
	return noErr;
}
EXPORT OSStatus MIDIObjectSetDataProperty(MIDIObjectRef obj, CFStringRef propertyID, CFDataRef data) {
	pthread_mutex_lock(&lk);
	Obj *o = find(obj);
	if (o && propertyID && data) CFDictionarySetValue(o->props, propertyID, data);
	pthread_mutex_unlock(&lk);
	return o ? noErr : kMIDIObjectNotFound;
}
EXPORT OSStatus MIDIObjectGetDictionaryProperty(MIDIObjectRef obj, CFStringRef propertyID, CFDictionaryRef *outDict) {
	CFTypeRef v; OSStatus r = getProp(obj, propertyID, &v);
	if (r) { if (outDict) *outDict = NULL; return r; }
	if (CFGetTypeID(v) != CFDictionaryGetTypeID()) { CFRelease(v); return kMIDIWrongPropertyType; }
	*outDict = v;
	return noErr;
}
EXPORT OSStatus MIDIObjectSetDictionaryProperty(MIDIObjectRef obj, CFStringRef propertyID, CFDictionaryRef dict) {
	pthread_mutex_lock(&lk);
	Obj *o = find(obj);
	if (o && propertyID && dict) CFDictionarySetValue(o->props, propertyID, dict);
	pthread_mutex_unlock(&lk);
	return o ? noErr : kMIDIObjectNotFound;
}
EXPORT OSStatus MIDIObjectRemoveProperty(MIDIObjectRef obj, CFStringRef propertyID) {
	pthread_mutex_lock(&lk);
	Obj *o = find(obj);
	if (o && propertyID) CFDictionaryRemoveValue(o->props, propertyID);
	pthread_mutex_unlock(&lk);
	return o ? noErr : kMIDIObjectNotFound;
}
EXPORT OSStatus MIDIObjectGetProperties(MIDIObjectRef obj, CFPropertyListRef *outProperties, Boolean deep) {
	(void)deep;
	pthread_mutex_lock(&lk);
	Obj *o = find(obj);
	if (o) *outProperties = CFDictionaryCreateCopy(NULL, o->props);
	pthread_mutex_unlock(&lk);
	return o ? noErr : kMIDIObjectNotFound;
}
EXPORT OSStatus MIDIObjectFindByUniqueID(MIDIUniqueID inUniqueID, MIDIObjectRef *outObject, MIDIObjectType *outObjectType) {
	if (outObject) *outObject = 0;
	if (outObjectType) *outObjectType = kMIDIObjectType_Other;
	pthread_mutex_lock(&lk);
	for (Obj *o = objs; o; o = o->next) {
		CFNumberRef n = CFDictionaryGetValue(o->props, kMIDIPropertyUniqueID); SInt32 v;
		if (n && CFNumberGetValue(n, kCFNumberSInt32Type, &v) && v == inUniqueID) {
			if (outObject) *outObject = o->ref;
			if (outObjectType) *outObjectType = o->kind == O_SOURCE ? kMIDIObjectType_Source : o->kind == O_DEST ? kMIDIObjectType_Destination : kMIDIObjectType_Other;
			pthread_mutex_unlock(&lk);
			return noErr;
		}
	}
	pthread_mutex_unlock(&lk);
	return kMIDIObjectNotFound;
}
