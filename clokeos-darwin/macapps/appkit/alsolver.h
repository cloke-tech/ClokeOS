/* Lineare Bedingungen mit Prioritäten lösen (Cassowary nach Badros/Borning/Stuckey, Aufbau wie Kiwi), für Auto Layout (autolayout.m).
 *
 * Eine Bedingung ist  Summe(koeff_i * var_i) + konstante  OP  0  mit OP <=, == oder >=.  Priorität 1000 = Pflicht, darunter
 * werden Verletzungen minimiert, und zwar lexikografisch wie in AppKit: eine Bedingung mit 999 geht allen mit 998 vor, egal wie
 * viele es sind (je Priorität eine eigene Zielfunktion, nicht gewichtete Summen).
 * Nicht erfüllbare Pflichtbedingungen werden verworfen (al_add gibt -1 zurück), wie AppKit eine Bedingung "bricht". */
#ifndef CLK_ALSOLVER_H
#define CLK_ALSOLVER_H

typedef struct al_solver al_solver;

enum { AL_LE = -1, AL_EQ = 0, AL_GE = 1 };

al_solver *al_new(void);
void al_free(al_solver *s);
/* neue freie Variable (Wert 0, solange keine Bedingung sie festlegt) */
int al_var(al_solver *s);
/* Bedingung hinzufügen; 0 = aufgenommen, 1 = überflüssig (schon erfüllt), -1 = Pflicht nicht erfüllbar (verworfen) */
int al_add(al_solver *s, int n, const int *vars, const double *coefs, double constant, int op, double priority);
double al_value(al_solver *s, int var);

#endif
