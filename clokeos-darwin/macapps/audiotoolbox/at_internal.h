// Gemeinsame Hilfen der AudioToolbox-Teilmenge für ClokeOS (eigener Code, kein Apple-Code).
#ifndef AT_INTERNAL_H
#define AT_INTERNAL_H
#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>

#define AT_MAXCH 32

// PCM-Format in Float32 (nicht verschachtelt) zerlegen und zurückschreiben. Unterstützt 8..32 Bit Ganzzahl
// (gepackt oder in größerem Container, beide Byteordnungen, ohne Vorzeichen bei 8 Bit) und Float32/64.
int  at_pcm_ok(const AudioStreamBasicDescription *f);
UInt32 at_pcm_bytes_per_frame(const AudioStreamBasicDescription *f);   // Bytes eines Frames in EINEM Puffer
UInt32 at_pcm_nbuffers(const AudioStreamBasicDescription *f);
void at_decode(const AudioStreamBasicDescription *f, const AudioBufferList *abl, UInt32 n, float **planar);
void at_encode(const AudioStreamBasicDescription *f, AudioBufferList *abl, UInt32 n, float **planar);
// Kanalabbildung ohne Layout: gleiche Zahl = durchreichen, Mono = auf alle, auf Mono = Mittel, sonst min() und Rest 0
void at_chanmap(UInt32 sch, float **src, UInt32 dch, float **dst, UInt32 n);
// ALAC-Wandler: nach dem Suchen so viele dekodierte Frames verwerfen (Lage im Paket)
void at_conv_skip(AudioConverterRef c, UInt32 frames);
#endif
