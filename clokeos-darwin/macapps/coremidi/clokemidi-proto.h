// Protokoll zwischen CoreMIDI (macapps/coremidi/CoreMIDI.c), dem MIDI-Dienst clokemidid und einfachen Programmen
// wie der MIDI-Tastatur (apps/clokeapps.c). Unix-Stream-Socket CLOKEMIDI_SOCKET, jede Nachricht = Kopf + Nutzdaten.
//
// Endpunkte werden über ihre MIDI-uniqueID angesprochen (eine zufällige 32-Bit-Zahl, die der Besitzer wählt).
//   ADD       Programm -> Dienst  kind (1 Quelle, 2 Ziel), uid, Nutzdaten = Name (UTF-8)
//   REMOVE    Programm -> Dienst  uid
//   SUB/UNSUB Programm -> Dienst  uid einer fremden Quelle abonnieren / abbestellen
//   DATA      Programm -> Dienst  MIDI aus der eigenen Quelle uid; Dienst -> Abonnenten dieselbe Nachricht
//   SEND      Programm -> Dienst  MIDI an das fremde Ziel uid; Dienst -> Besitzer des Ziels dieselbe Nachricht
//   EP_ADD    Dienst -> Programm  ein Endpunkt eines anderen Programms (beim Verbinden alle vorhandenen)
//   EP_REMOVE Dienst -> Programm  uid ist weg
// fmt der MIDI-Nutzdaten: 0 = rohe MIDI-1.0-Bytes (Zeitstempel 0), 1 = MIDIPacketList, 2 = MIDIEventList (beide im
// Speicherformat dieses Rechners, alle Beteiligten laufen auf derselben Maschine).
#ifndef CLOKEMIDI_PROTO_H
#define CLOKEMIDI_PROTO_H
#include <stdint.h>

#define CLOKEMIDI_SOCKET "/var/run/clokemidi.sock"
#define CLOKEMIDI_MAXPAYLOAD 65536

enum { CM_ADD = 1, CM_REMOVE, CM_SUB, CM_UNSUB, CM_DATA, CM_SEND, CM_EP_ADD, CM_EP_REMOVE };
enum { CM_SOURCE = 1, CM_DEST = 2 };
enum { CM_FMT_BYTES = 0, CM_FMT_PACKETS = 1, CM_FMT_EVENTS = 2 };

typedef struct {
	uint32_t len;    // Länge der Nutzdaten in Bytes
	uint16_t type;
	uint16_t kind;
	int32_t uid;
	uint32_t fmt;
} cm_hdr;

#endif
