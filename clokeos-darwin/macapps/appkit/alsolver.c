/* Cassowary-Löser für Auto Layout, siehe alsolver.h.
 *
 * Tableau wie in Kiwi: jede Zeile hält "Basissymbol = Konstante + Summe(koeff * Nichtbasissymbol)"; Symbole sind extern (Variablen
 * des Aufrufers, unbeschränkt), Schlupf (>= 0, für <= und >=), Fehler (>= 0, Verletzung einer Wunschbedingung) und Dummy (Marker
 * für Pflicht-Gleichungen, nie Pivot).  Die Zielfunktion ist ein Vektor: eine Zeile je Priorität, absteigend sortiert; ein Symbol
 * verbessert das Ziel, wenn der erste von 0 verschiedene Koeffizient (höchste Priorität zuerst) negativ ist.  Pivotwahl nach Bland
 * (kleinste Symbolnummer), damit entartete Schritte nicht kreisen.  Zeilen sind dünn besetzt (nach Symbol sortierte Felder). */
#include "alsolver.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define EPS 1e-8
#define REQUIRED 1000.0

enum { S_EXT = 1, S_SLACK, S_ERROR, S_DUMMY };

typedef struct { int n, cap; int *sym; double *c; double k; } row;

struct al_solver {
    int nsym, capsym; unsigned char *type; int *basic;   /* basic[sym] = Zeilennummer oder -1 */
    int nvar, capvar; int *var2sym;
    int nrows, caprows; row *rows; int *rowsym;          /* rowsym[i] = Basissymbol, -1 = freier Platz */
    int nlev, caplev; double *levprio; row *lev;         /* Zielfunktion, Priorität absteigend */
};

static int nz(double x) { return fabs(x) < EPS; }

/* ---- Zeilen ---- */
static void row_free(row *r) { free(r->sym); free(r->c); memset(r, 0, sizeof *r); }
static void row_grow(row *r, int need) {
    if (need <= r->cap) return;
    int cap = r->cap ? r->cap * 2 : 8;
    while (cap < need) cap *= 2;
    r->sym = realloc(r->sym, cap * sizeof(int));
    r->c = realloc(r->c, cap * sizeof(double));
    r->cap = cap;
}
static int row_find(const row *r, int sym) {
    int lo = 0, hi = r->n - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (r->sym[m] == sym) return m;
        if (r->sym[m] < sym) lo = m + 1; else hi = m - 1;
    }
    return -(lo + 1);
}
static double row_coef(const row *r, int sym) { int i = row_find(r, sym); return i >= 0 ? r->c[i] : 0; }
static void row_remove(row *r, int sym) {
    int i = row_find(r, sym);
    if (i < 0) return;
    memmove(r->sym + i, r->sym + i + 1, (r->n - i - 1) * sizeof(int));
    memmove(r->c + i, r->c + i + 1, (r->n - i - 1) * sizeof(double));
    r->n--;
}
static void row_add(row *r, int sym, double c) {
    int i = row_find(r, sym);
    if (i >= 0) {
        r->c[i] += c;
        if (nz(r->c[i])) row_remove(r, sym);
        return;
    }
    if (nz(c)) return;
    i = -i - 1;
    row_grow(r, r->n + 1);
    memmove(r->sym + i + 1, r->sym + i, (r->n - i) * sizeof(int));
    memmove(r->c + i + 1, r->c + i, (r->n - i) * sizeof(double));
    r->sym[i] = sym; r->c[i] = c; r->n++;
}
/* r += m * o (Mischen zweier sortierter Felder) */
static void row_add_row(row *r, const row *o, double m) {
    r->k += m * o->k;
    if (!o->n) return;
    int n = r->n + o->n, i = 0, j = 0, w = 0;
    int *sym = malloc(n * sizeof(int)); double *c = malloc(n * sizeof(double));
    while (i < r->n || j < o->n) {
        int s; double v;
        if (j >= o->n || (i < r->n && r->sym[i] < o->sym[j])) { s = r->sym[i]; v = r->c[i]; i++; }
        else if (i >= r->n || o->sym[j] < r->sym[i]) { s = o->sym[j]; v = m * o->c[j]; j++; }
        else { s = r->sym[i]; v = r->c[i] + m * o->c[j]; i++; j++; }
        if (!nz(v)) { sym[w] = s; c[w] = v; w++; }
    }
    free(r->sym); free(r->c);
    r->sym = sym; r->c = c; r->n = w; r->cap = n;
}
static void row_mul(row *r, double m) { r->k *= m; for (int i = 0; i < r->n; i++) r->c[i] *= m; }
/* 0 = k + ... + c*sym + ...  ->  sym = ... */
static void row_solve_for(row *r, int sym) {
    double c = row_coef(r, sym);
    row_remove(r, sym);
    row_mul(r, -1.0 / c);
}
/* lhs = k + ... + c*rhs  ->  rhs = ... */
static void row_solve_for2(row *r, int lhs, int rhs) { row_add(r, lhs, -1.0); row_solve_for(r, rhs); }
static void row_subst(row *r, int sym, const row *s) {
    double c = row_coef(r, sym);
    if (c == 0) return;
    row_remove(r, sym);
    row_add_row(r, s, c);
}
static void row_copy(row *d, const row *s) {
    memset(d, 0, sizeof *d);
    row_grow(d, s->n ? s->n : 1);
    memcpy(d->sym, s->sym, s->n * sizeof(int));
    memcpy(d->c, s->c, s->n * sizeof(double));
    d->n = s->n; d->k = s->k;
}

/* ---- Löser ---- */
al_solver *al_new(void) { return calloc(1, sizeof(al_solver)); }

void al_free(al_solver *s) {
    if (!s) return;
    for (int i = 0; i < s->nrows; i++) if (s->rowsym[i] >= 0) row_free(&s->rows[i]);
    for (int i = 0; i < s->nlev; i++) row_free(&s->lev[i]);
    free(s->rows); free(s->rowsym); free(s->lev); free(s->levprio);
    free(s->type); free(s->basic); free(s->var2sym);
    free(s);
}

static int new_sym(al_solver *s, int type) {
    if (s->nsym == s->capsym) {
        s->capsym = s->capsym ? s->capsym * 2 : 64;
        s->type = realloc(s->type, s->capsym);
        s->basic = realloc(s->basic, s->capsym * sizeof(int));
    }
    s->type[s->nsym] = type;
    s->basic[s->nsym] = -1;
    return s->nsym++;
}

int al_var(al_solver *s) {
    if (s->nvar == s->capvar) {
        s->capvar = s->capvar ? s->capvar * 2 : 64;
        s->var2sym = realloc(s->var2sym, s->capvar * sizeof(int));
    }
    s->var2sym[s->nvar] = new_sym(s, S_EXT);
    return s->nvar++;
}

/* Zeile r (wird übernommen) als Basiszeile für sym eintragen */
static void put_row(al_solver *s, int sym, row *r) {
    int i;
    for (i = 0; i < s->nrows; i++) if (s->rowsym[i] < 0) break;
    if (i == s->nrows) {
        if (s->nrows == s->caprows) {
            s->caprows = s->caprows ? s->caprows * 2 : 64;
            s->rows = realloc(s->rows, s->caprows * sizeof(row));
            s->rowsym = realloc(s->rowsym, s->caprows * sizeof(int));
        }
        s->nrows++;
    }
    s->rows[i] = *r;
    s->rowsym[i] = sym;
    s->basic[sym] = i;
}
/* Basiszeile von sym herausnehmen (Inhalt nach *out) */
static void take_row(al_solver *s, int sym, row *out) {
    int i = s->basic[sym];
    *out = s->rows[i];
    s->rowsym[i] = -1;
    s->basic[sym] = -1;
}

static row *level_for(al_solver *s, double prio) {
    int i;
    for (i = 0; i < s->nlev; i++) {
        if (s->levprio[i] == prio) return &s->lev[i];
        if (s->levprio[i] < prio) break;
    }
    if (s->nlev == s->caplev) {
        s->caplev = s->caplev ? s->caplev * 2 : 8;
        s->lev = realloc(s->lev, s->caplev * sizeof(row));
        s->levprio = realloc(s->levprio, s->caplev * sizeof(double));
    }
    memmove(s->lev + i + 1, s->lev + i, (s->nlev - i) * sizeof(row));
    memmove(s->levprio + i + 1, s->levprio + i, (s->nlev - i) * sizeof(double));
    memset(&s->lev[i], 0, sizeof(row));
    s->levprio[i] = prio;
    s->nlev++;
    return &s->lev[i];
}

static void substitute(al_solver *s, int sym, const row *r, row *extra) {
    for (int i = 0; i < s->nrows; i++) if (s->rowsym[i] >= 0) row_subst(&s->rows[i], sym, r);
    for (int i = 0; i < s->nlev; i++) row_subst(&s->lev[i], sym, r);
    if (extra) row_subst(extra, sym, r);
}

/* Eintretendes Symbol: kleinste Nummer mit lexikografisch negativem Koeffizientenvektor */
static int entering(al_solver *s, row *obj, int nobj) {
    int best = -1;
    for (int l = 0; l < nobj; l++) {
        const row *r = &obj[l];
        for (int i = 0; i < r->n; i++) {
            int sym = r->sym[i];
            if (r->c[i] >= -EPS || s->type[sym] == S_DUMMY || (best >= 0 && sym >= best)) continue;
            int ok = 1;
            for (int h = 0; h < l && ok; h++) if (!nz(row_coef(&obj[h], sym))) ok = 0;
            if (ok) best = sym;
        }
    }
    return best;
}

static int leaving(al_solver *s, int ent) {
    double ratio = INFINITY; int best = -1;
    for (int i = 0; i < s->nrows; i++) {
        int sym = s->rowsym[i];
        if (sym < 0 || s->type[sym] == S_EXT) continue;
        double c = row_coef(&s->rows[i], ent);
        if (c >= -EPS) continue;
        double q = -s->rows[i].k / c;
        if (q < ratio - EPS || (fabs(q - ratio) <= EPS && sym < best)) { ratio = q; best = sym; }
    }
    return best;
}

/* obj: nobj Zeilen (Prioritäten absteigend); extra = zusätzlich mitzuführende Zeile (künstliches Ziel) */
static int optimize(al_solver *s, row *obj, int nobj, row *extra) {
    for (int iter = 0; iter < 100000; iter++) {
        int ent = entering(s, obj, nobj);
        if (ent < 0) return 0;
        int lv = leaving(s, ent);
        if (lv < 0) return -1;   /* unbeschränkt: kommt bei Fehlersymbolen >= 0 nicht vor */
        row r;
        take_row(s, lv, &r);
        row_solve_for2(&r, lv, ent);
        substitute(s, ent, &r, extra);
        put_row(s, ent, &r);
    }
    return -1;
}

static int any_pivotable(const al_solver *s, const row *r) {
    for (int i = 0; i < r->n; i++) {
        int t = s->type[r->sym[i]];
        if (t == S_SLACK || t == S_ERROR) return r->sym[i];
    }
    return -1;
}

/* Zeile ohne passendes Subjekt: über eine künstliche Variable einführen (Phase 1) */
static int add_artificial(al_solver *s, row *r) {
    int art = new_sym(s, S_SLACK);
    row cp; row_copy(&cp, r);
    put_row(s, art, &cp);
    row aobj; row_copy(&aobj, r);
    optimize(s, &aobj, 1, &aobj);
    int ok = nz(aobj.k);
    row_free(&aobj);
    if (s->basic[art] >= 0) {
        row ar;
        take_row(s, art, &ar);
        if (ar.n == 0) { row_free(&ar); goto clean; }
        int ent = any_pivotable(s, &ar);
        if (ent < 0) { row_free(&ar); ok = 0; goto clean; }
        row_solve_for2(&ar, art, ent);
        substitute(s, ent, &ar, NULL);
        put_row(s, ent, &ar);
    }
clean:
    for (int i = 0; i < s->nrows; i++) if (s->rowsym[i] >= 0) row_remove(&s->rows[i], art);
    for (int i = 0; i < s->nlev; i++) row_remove(&s->lev[i], art);
    return ok;
}

int al_add(al_solver *s, int n, const int *vars, const double *coefs, double constant, int op, double priority) {
    row r; memset(&r, 0, sizeof r);
    r.k = constant;
    for (int i = 0; i < n; i++) {
        int sym = s->var2sym[vars[i]];
        if (s->basic[sym] >= 0) row_add_row(&r, &s->rows[s->basic[sym]], coefs[i]);
        else row_add(&r, sym, coefs[i]);
    }
    int required = priority >= REQUIRED;
    int marker = -1, other = -1;
    if (op != AL_EQ) {
        double c = op == AL_LE ? 1.0 : -1.0;
        marker = new_sym(s, S_SLACK);
        row_add(&r, marker, c);
        if (!required) {
            other = new_sym(s, S_ERROR);
            row_add(&r, other, -c);
            row_add(level_for(s, priority), other, 1.0);
        }
    } else if (!required) {
        marker = new_sym(s, S_ERROR); other = new_sym(s, S_ERROR);
        row_add(&r, marker, -1.0);
        row_add(&r, other, 1.0);
        row *lv = level_for(s, priority);
        row_add(lv, marker, 1.0);
        row_add(lv, other, 1.0);
    } else {
        marker = new_sym(s, S_DUMMY);
        row_add(&r, marker, 1.0);
    }
    if (r.k < 0) row_mul(&r, -1.0);

    /* Subjekt wählen: externes Symbol, sonst Schlupf/Fehler mit negativem Koeffizienten */
    int subj = -1;
    for (int i = 0; i < r.n && subj < 0; i++) if (s->type[r.sym[i]] == S_EXT) subj = r.sym[i];
    if (subj < 0 && (s->type[marker] == S_SLACK || s->type[marker] == S_ERROR) && row_coef(&r, marker) < 0) subj = marker;
    if (subj < 0 && other >= 0 && row_coef(&r, other) < 0) subj = other;

    int res = 0;
    if (subj < 0) {
        int dummies = 1;
        for (int i = 0; i < r.n; i++) if (s->type[r.sym[i]] != S_DUMMY) dummies = 0;
        if (dummies) {
            res = nz(r.k) ? 1 : -1;
            row_free(&r);
            return res;
        }
        if (!add_artificial(s, &r)) res = -1;
        row_free(&r);
    } else {
        row_solve_for(&r, subj);
        substitute(s, subj, &r, NULL);
        put_row(s, subj, &r);
    }
    if (optimize(s, s->lev, s->nlev, NULL) < 0) res = -1;
    return res;
}

double al_value(al_solver *s, int var) {
    int sym = s->var2sym[var];
    return s->basic[sym] >= 0 ? s->rows[s->basic[sym]].k : 0;
}
