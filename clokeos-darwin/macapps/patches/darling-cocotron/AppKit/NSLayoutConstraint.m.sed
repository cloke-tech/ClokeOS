# NSViewNoIntrinsicMetric ist bei Apple -1.0; Darling schrieb das Bitmuster von -1.0 als Ganzzahl (ergibt 1,38e19)
s/= 0xbff0000000000000;/= -1.0;/
