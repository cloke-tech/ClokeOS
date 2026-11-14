// Zwei-Prozess-Test für CoreMIDI über clokemidid: m2 recv | m2 send
#include <CoreMIDI/CoreMIDI.h>
#include <stdio.h>
#include <string.h>
static void rd(const MIDIPacketList *pl, void *r, void *c) {
	const MIDIPacket *p = &pl->packet[0];
	for (UInt32 i = 0; i < pl->numPackets; i++) { printf("%s got:", (char *)r); for (int k = 0; k < p->length; k++) printf(" %02x", p->data[k]); printf("\n"); p = MIDIPacketNext(p); }
	fflush(stdout);
}
static int added;
static void nt(const MIDINotification *m, void *r) { (void)r; if (m->messageID == kMIDIMsgObjectAdded) added++; }
int main(int argc, char **argv) {
	MIDIClientRef cl; MIDIClientCreate(CFSTR("m2"), nt, NULL, &cl);
	if (argc > 1 && !strcmp(argv[1], "recv")) {
		MIDIEndpointRef dst; MIDIDestinationCreate(cl, CFSTR("Recv Dst"), rd, "dest", &dst);
		MIDIPortRef in; MIDIInputPortCreate(cl, CFSTR("in"), rd, "port", &in);
		for (int t = 0; t < 100; t++) {   // warten, bis die Quelle des Senders auftaucht
			CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, false);
			for (ItemCount i = 0; i < MIDIGetNumberOfSources(); i++) {
				CFStringRef n; MIDIObjectGetStringProperty(MIDIGetSource(i), kMIDIPropertyName, &n); char b[64]; CFStringGetCString(n, b, 64, kCFStringEncodingUTF8); CFRelease(n);
				static int done; if (!done && (!strcmp(b, "Send Src") || !strcmp(b, "Raw Keyboard"))) { done = 1; printf("connect %s = %d\n", b, (int)MIDIPortConnectSource(in, MIDIGetSource(i), NULL)); fflush(stdout); }
			}
		}
		printf("notifications added>=1: %d\n", added > 0);
	} else {
		MIDIEndpointRef src; MIDISourceCreate(cl, CFSTR("Send Src"), &src);
		MIDIPortRef out; MIDIOutputPortCreate(cl, CFSTR("out"), &out);
		CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.5, false);
		Byte b[1024]; MIDIPacketList *pl = (MIDIPacketList *)b; MIDIPacket *p = MIDIPacketListInit(pl);
		Byte on[] = {0x90, 64, 90}; p = MIDIPacketListAdd(pl, sizeof b, p, 0, 3, on);
		printf("received=%d\n", (int)MIDIReceived(src, pl));
		MIDIEndpointRef d = 0;
		for (ItemCount i = 0; i < MIDIGetNumberOfDestinations(); i++) {
			CFStringRef n; MIDIObjectGetStringProperty(MIDIGetDestination(i), kMIDIPropertyName, &n); char s[64]; CFStringGetCString(n, s, 64, kCFStringEncodingUTF8); CFRelease(n);
			if (!strcmp(s, "Recv Dst")) d = MIDIGetDestination(i);
		}
		Byte cc[] = {0xB3, 10, 20}; p = MIDIPacketListInit(pl); p = MIDIPacketListAdd(pl, sizeof b, p, 0, 3, cc);
		printf("found dest=%d send=%d\n", d != 0, (int)MIDISend(out, d, pl));
		CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.0, false);
	}
	return 0;
}
