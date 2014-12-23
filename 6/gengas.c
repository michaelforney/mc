#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include <ctype.h>
#include <string.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "parse.h"
#include "mi.h"
#include "asm.h"
#include "../config.h"

static char *insnfmt[] = {
#define Insn(val, gasfmt, p9fmt, use, def) gasfmt,
#include "insns.def"
#undef Insn
};

static char *regnames[] = {
#define Reg(r, gasname, p9name, mode) gasname,
#include "regs.def"
#undef Reg
};

static char* modenames[] = {
  [ModeB] = "b",
  [ModeW] = "w",
  [ModeL] = "l",
  [ModeQ] = "q",
  [ModeF] = "s",
  [ModeD] = "d"
};

static size_t writeblob(FILE *fd, Htab *globls, Htab *strtab, Node *blob);
static void locprint(FILE *fd, Loc *l, char spec);

static void fillglobls(Stab *st, Htab *globls)
{
    void **k;
    size_t i, nk;
    Stab *stab;
    Node *s;

    k = htkeys(st->dcl, &nk);
    for (i = 0; i < nk; i++) {
        s = htget(st->dcl, k[i]);
        htput(globls, s, asmname(s->decl.name));
    }
    free(k);

    k = htkeys(st->ns, &nk);
    for (i = 0; i < nk; i++) {
        stab = htget(st->ns, k[i]);
        fillglobls(stab, globls);
    }
    free(k);
}

static void initconsts(Htab *globls)
{
    Type *ty;
    Node *name;
    Node *dcl;

    tyintptr = mktype(Zloc, Tyuint64);
    tyword = mktype(Zloc, Tyuint);
    tyvoid = mktype(Zloc, Tyvoid);

    ty = mktyfunc(Zloc, NULL, 0, mktype(Zloc, Tyvoid));
    name = mknsname(Zloc, "_rt", "abort_oob");
    dcl = mkdecl(Zloc, name, ty);
    dcl->decl.isconst = 1;
    dcl->decl.isextern = 1;
    htput(globls, dcl, asmname(dcl->decl.name));

    abortoob = mkexpr(Zloc, Ovar, name, NULL);
    abortoob->expr.type = ty;
    abortoob->expr.did = dcl->decl.did;
    abortoob->expr.isconst = 1;
}

void printmem(FILE *fd, Loc *l, char spec)
{
    if (l->type == Locmem) {
        if (l->mem.constdisp)
            fprintf(fd, "%ld", l->mem.constdisp);
    } else {
        if (l->mem.lbldisp)
            fprintf(fd, "%s", l->mem.lbldisp);
    }
    if (l->mem.base) {
        fprintf(fd, "(");
        locprint(fd, l->mem.base, 'r');
        if (l->mem.idx) {
            fprintf(fd, ",");
            locprint(fd, l->mem.idx, 'r');
        }
        if (l->mem.scale > 1)
            fprintf(fd, ",%d", l->mem.scale);
        if (l->mem.base)
            fprintf(fd, ")");
    } else if (l->type != Locmeml) {
        die("Only locmeml can have unspecified base reg");
    }
}

static void locprint(FILE *fd, Loc *l, char spec)
{
    assert(l->mode);
    switch (l->type) {
        case Loclitl:
            assert(spec == 'i' || spec == 'x' || spec == 'u');
            fprintf(fd, "$%s", l->lbl);
            break;
        case Loclbl:
            assert(spec == 'm' || spec == 'v' || spec == 'x');
            fprintf(fd, "%s", l->lbl);
            break;
        case Locreg:
            assert((spec == 'r' && isintmode(l->mode)) || 
                   (spec == 'f' && isfloatmode(l->mode)) ||
                   spec == 'v' ||
                   spec == 'x' ||
                   spec == 'u');
            if (l->reg.colour == Rnone)
                fprintf(fd, "%%P.%zd%s", l->reg.id, modenames[l->mode]);
            else
                fprintf(fd, "%s", regnames[l->reg.colour]);
            break;
        case Locmem:
        case Locmeml:
            assert(spec == 'm' || spec == 'v' || spec == 'x');
            printmem(fd, l, spec);
            break;
        case Loclit:
            assert(spec == 'i' || spec == 'x' || spec == 'u');
            fprintf(fd, "$%ld", l->lit);
            break;
        case Locnone:
            die("Bad location in locprint()");
            break;
    }
}

static int issubreg(Loc *a, Loc *b)
{
    return rclass(a) == rclass(b) && a->mode != b->mode;
}

void iprintf(FILE *fd, Insn *insn)
{
    char *p;
    int i;
    int modeidx;

    /* x64 has a quirk; it has no movzlq because mov zero extends. This
     * means that we need to do a movl when we really want a movzlq. Since
     * we don't know the name of the reg to use, we need to sub it in when
     * writing... */
    switch (insn->op) {
        case Imovzx:
            if (insn->args[0]->mode == ModeL && insn->args[1]->mode == ModeQ) {
                if (insn->args[1]->reg.colour) {
                    insn->op = Imov;
                    insn->args[1] = coreg(insn->args[1]->reg.colour, ModeL);
                }
            }
            break;
        case Imovs:
            if (insn->args[0]->reg.colour == Rnone || insn->args[1]->reg.colour == Rnone)
                break;
            /* moving a reg to itself is dumb. */
            if (insn->args[0]->reg.colour == insn->args[1]->reg.colour)
                return;
            break;
        case Imov:
            assert(!isfloatmode(insn->args[0]->mode));
            if (insn->args[0]->type != Locreg || insn->args[1]->type != Locreg)
                break;
            if (insn->args[0]->reg.colour == Rnone || insn->args[1]->reg.colour == Rnone)
                break;
            /* if one reg is a subreg of another, we can just use the right
             * mode to move between them. */
            if (issubreg(insn->args[0], insn->args[1]))
                insn->args[0] = coreg(insn->args[0]->reg.colour, insn->args[1]->mode);
            /* moving a reg to itself is dumb. */
            if (insn->args[0]->reg.colour == insn->args[1]->reg.colour)
                return;
            break;
        default:
            break;
    }
    p = insnfmt[insn->op];
    i = 0;
    modeidx = 0;
    for (; *p; p++) {
        if (*p !=  '%') {
            fputc(*p, fd);
            continue;
        }

        /* %-formating */
        p++;
        switch (*p) {
            case '\0':
                goto done; /* skip the final p++ */
            case 'r': /* int register */
            case 'f': /* float register */
            case 'm': /* memory */
            case 'i': /* imm */
            case 'v': /* reg/mem */
            case 'u': /* reg/imm */
            case 'x': /* reg/mem/imm */
                locprint(fd, insn->args[i], *p);
                i++;
                break;
            case 'R': /* int register */
            case 'F': /* float register */
            case 'M': /* memory */
            case 'I': /* imm */
            case 'V': /* reg/mem */
            case 'U': /* reg/imm */
            case 'X': /* reg/mem/imm */
                locprint(fd, insn->args[i], *p);
                i++;
                break;
            case 't':
            case 'T':
            default:
                /* the  asm description uses 1-based indexing, so that 0
                 * can be used as a sentinel. */
                if (isdigit(*p))
                    modeidx = strtol(p, &p, 10) - 1;

                if (*p == 't')
                    fputs(modenames[insn->args[modeidx]->mode], fd);
                else
                    die("Invalid %%-specifier '%c'", *p);
                break;
        }
    }
done:
    return;
}


static void writebytes(FILE *fd, char *p, size_t sz)
{
    size_t i;

    for (i = 0; i < sz; i++) {
        if (i % 60 == 0)
            fprintf(fd, "\t.ascii \"");
        if (p[i] == '"' || p[i] == '\\')
            fprintf(fd, "\\");
        if (isprint(p[i]))
            fprintf(fd, "%c", p[i]);
        else
            fprintf(fd, "\\%03o", (uint8_t)p[i] & 0xff);
        /* line wrapping for readability */
        if (i % 60 == 59 || i == sz - 1)
            fprintf(fd, "\"\n");
    }
}

static size_t writelit(FILE *fd, Htab *strtab, Node *v, Type *ty)
{
    char buf[128];
    char *lbl;
    size_t sz;
    char *intsz[] = {
        [1] = ".byte",
        [2] = ".short",
        [4] = ".long",
        [8] = ".quad"
    };
    union {
        float fv;
        double dv;
        uint64_t qv;
        uint32_t lv;
    } u;

    assert(v->type == Nlit);
    sz = tysize(ty);
    switch (v->lit.littype) {
        case Lint:      fprintf(fd, "\t%s %lld\n", intsz[sz], v->lit.intval);    break;
        case Lbool:     fprintf(fd, "\t.byte %d\n", v->lit.boolval);     break;
        case Lchr:      fprintf(fd, "\t.long %d\n",  v->lit.chrval);     break;
        case Lflt:
                if (tybase(v->lit.type)->type == Tyflt32) {
                    u.fv = v->lit.fltval;
                    fprintf(fd, "\t.long 0x%llx\n", (vlong)u.lv);
                } else if (tybase(v->lit.type)->type == Tyflt64) {
                    u.dv = v->lit.fltval;
                    fprintf(fd, "\t.quad 0x%llx\n", (vlong)u.qv);
                }
                break;
        case Lstr:
           if (hthas(strtab, &v->lit.strval)) {
               lbl = htget(strtab, &v->lit.strval);
           } else {
               lbl = genlblstr(buf, sizeof buf);
               htput(strtab, &v->lit.strval, strdup(lbl));
           }
           fprintf(fd, "\t.quad %s\n", lbl);
           fprintf(fd, "\t.quad %zd\n", v->lit.strval.len);
           break;
        case Lfunc:
            die("Generating this shit ain't ready yet ");
            break;
        case Llbl:
            die("Can't generate literal labels, ffs. They're not data.");
            break;
    }
    return sz;
}

static size_t writepad(FILE *fd, size_t sz)
{
    assert((ssize_t)sz >= 0);
    if (sz > 0)
        fprintf(fd, "\t.fill %zd,1,0\n", sz);
    return sz;
}

static size_t getintlit(Node *n, char *failmsg)
{
    if (exprop(n) != Olit)
        fatal(n, "%s", failmsg);
    n = n->expr.args[0];
    if (n->lit.littype != Lint)
        fatal(n, "%s", failmsg);
    return n->lit.intval;
}

static size_t writeslice(FILE *fd, Htab *globls, Htab *strtab, Node *n)
{
    Node *base, *lo, *hi;
    ssize_t loval, hival, sz;
    char *lbl;

    base = n->expr.args[0];
    lo = n->expr.args[1];
    hi = n->expr.args[2];

    /* by this point, all slicing operations should have had their bases
     * pulled out, and we should have vars with their pseudo-decls in their
     * place */
    if (exprop(base) != Ovar || !base->expr.isconst)
        fatal(base, "slice base is not a constant value");
    loval = getintlit(lo, "lower bound in slice is not constant literal");
    hival = getintlit(hi, "upper bound in slice is not constant literal");
    sz = tysize(tybase(exprtype(base))->sub[0]);

    lbl = htget(globls, base);
    fprintf(fd, "\t.quad %s + (%zd*%zd)\n", lbl, loval, sz);
    fprintf(fd, "\t.quad %zd\n", (hival - loval));
    return size(n);
}

static size_t writestruct(FILE *fd, Htab *globls, Htab *strtab, Node *n)
{
    Type *t;
    Node **dcl;
    int found;
    size_t i, j;
    size_t sz, pad, end;
    size_t ndcl;

    sz = 0;
    t = tybase(exprtype(n));
    assert(t->type == Tystruct);
    dcl = t->sdecls;
    ndcl = t->nmemb;
    for (i = 0; i < ndcl; i++) {
        pad = alignto(sz, decltype(dcl[i]));
        sz += writepad(fd, pad - sz);
        found = 0;
        for (j = 0; j < n->expr.nargs; j++)
            if (!strcmp(namestr(n->expr.args[j]->expr.idx), declname(dcl[i]))) {
                found = 1;
                sz += writeblob(fd, globls, strtab, n->expr.args[j]);
            }
        if (!found)
            sz += writepad(fd, size(dcl[i]));
    }
    end = alignto(sz, t);
    sz += writepad(fd, end - sz);
    return sz;
}
static size_t writeblob(FILE *fd, Htab *globls, Htab *strtab, Node *n)
{
    size_t i, sz;

    switch(exprop(n)) {
        case Otup:
        case Oarr:
            sz = 0;
            for (i = 0; i < n->expr.nargs; i++)
                sz += writeblob(fd, globls, strtab, n->expr.args[i]);
            break;
        case Ostruct:
            sz = writestruct(fd, globls, strtab, n);
            break;
        case Olit:
            sz = writelit(fd, strtab, n->expr.args[0], exprtype(n));
            break;
        case Oslice:
            sz = writeslice(fd, globls, strtab, n);
            break;
        default:
            dump(n, stdout);
            die("Nonliteral initializer for global");
            break;
    }
    return sz;
}

void genstrings(FILE *fd, Htab *strtab)
{
    void **k;
    Str *s;
    size_t i, nk;

    k = htkeys(strtab, &nk);
    for (i = 0; i < nk; i++) {
        s = k[i];
        fprintf(fd, "%s:\n", (char*)htget(strtab, k[i]));
        writebytes(fd, s->buf, s->len);
    }
}


static void writeasm(FILE *fd, Isel *s, Func *fn)
{
    size_t i, j;

    if (fn->isexport || !strcmp(fn->name, Symprefix "main"))
        fprintf(fd, ".globl %s\n", fn->name);
    fprintf(fd, "%s:\n", fn->name);
    for (j = 0; j < s->cfg->nbb; j++) {
        if (!s->bb[j])
            continue;
        for (i = 0; i < s->bb[j]->nlbls; i++)
            fprintf(fd, "%s:\n", s->bb[j]->lbls[i]);
        for (i = 0; i < s->bb[j]->ni; i++)
            iprintf(fd, s->bb[j]->il[i]);
    }
}

void genblob(FILE *fd, Node *blob, Htab *globls, Htab *strtab)
{
    char *lbl;

    /* lits and such also get wrapped in decls */
    assert(blob->type == Ndecl);

    lbl = htget(globls, blob);
    if (blob->decl.vis != Visintern)
        fprintf(fd, ".globl %s\n", lbl);
    fprintf(fd, "%s:\n", lbl);
    if (blob->decl.init)
        writeblob(fd, globls, strtab, blob->decl.init);
    else
        writepad(fd, size(blob));
}

/* genfunc requires all nodes in 'nl' to map cleanly to operations that are
 * natively supported, as promised in the output of reduce().  No 64-bit
 * operations on x32, no structures, and so on. */
void genfunc(FILE *fd, Func *fn, Htab *globls, Htab *strtab)
{
    Isel is = {0,};

    is.reglocs = mkht(varhash, vareq);
    is.stkoff = fn->stkoff;
    is.globls = globls;
    is.ret = fn->ret;
    is.cfg = fn->cfg;

    selfunc(&is, fn, globls, strtab);
    if (debugopt['i'])
        writeasm(stdout, &is, fn);
    writeasm(fd, &is, fn);
}

void gengas(Node *file, char *out)
{
    Htab *globls, *strtab;
    Node *n, **blob;
    Func **fn;
    size_t nfn, nblob;
    size_t i;
    FILE *fd;

    /* ensure that all physical registers have a loc created before any
     * other locs, so that locmap[Physreg] maps to the Loc for the physreg
     * in question */
    for (i = 0; i < Nreg; i++)
        locphysreg(i);

    fn = NULL;
    nfn = 0;
    blob = NULL;
    nblob = 0;
    globls = mkht(varhash, vareq);
    initconsts(globls);

    /* We need to define all global variables before use */
    fillglobls(file->file.globls, globls);

    pushstab(file->file.globls);
    for (i = 0; i < file->file.nstmts; i++) {
        n = file->file.stmts[i];
        switch (n->type) {
            case Nuse: /* nothing to do */ 
            case Nimpl:
                break;
            case Ndecl:
                simpglobl(n, globls, &fn, &nfn, &blob, &nblob);
                break;
            default:
                die("Bad node %s in toplevel", nodestr(n->type));
                break;
        }
    }
    popstab();

    fd = fopen(out, "w");
    if (!fd)
        die("Couldn't open fd %s", out);

    strtab = mkht(strlithash, strliteq);
    fprintf(fd, ".data\n");
    for (i = 0; i < nblob; i++)
        genblob(fd, blob[i], globls, strtab);
    fprintf(fd, ".text\n");
    for (i = 0; i < nfn; i++)
        genfunc(fd, fn[i], globls, strtab);
    genstrings(fd, strtab);
    fclose(fd);
}

void dbglocprint(FILE *fd, Loc *l, char spec)
{
    locprint(fd, l, spec);
}

void dbgiprintf(FILE *fd, Insn *i)
{
    iprintf(fd, i);
}