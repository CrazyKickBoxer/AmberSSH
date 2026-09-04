/* FontProbe.c — opens the built-in "fixed" and "cursor" fonts through
 * libXfont2 exactly the way dix does, with every return code printed.
 * Original AmberSSH file. Exists because the server said "could not open
 * default font" and nothing else; this says why. Kept as a gate test:
 * if the built-ins stop opening, AmberX cannot start. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <X11/Xdefs.h>
#include <X11/fonts/fontstruct.h>
#include <X11/fonts/fontproto.h>
#include <X11/fonts/fsmasks.h>
#include <X11/fonts/libxfont2.h>
#include "../../third_party/amberx/config-msvc/compat/dirent.h"

/* POSIX names libXfont2 expects; the host supplies these on its own side */
int strcasecmp(const char *a, const char *b) { return _stricmp(a, b); }
DIR *opendir(const char *n) { (void) n; return NULL; }
struct dirent *readdir(DIR *d) { (void) d; return NULL; }
int closedir(DIR *d) { (void) d; return 0; }

static const xfont2_fpe_funcs_rec *fpes[8];
static int nfpes;

static int  c_auth_gen(ClientPtr c) { (void) c; return 0; }
static Bool c_signal(ClientPtr c) { (void) c; return 1; }
static void c_delete_id(Font id) { (void) id; }
static void c_verrorf(const char *f, va_list ap) { vfprintf(stderr, f, ap); }
static FontPtr c_find_old(FSID id) { (void) id; return NULL; }
static FontResolutionPtr c_res(int *n) { *n = 0; return NULL; }
static int  c_ptsize(void) { return 120; }
static Font c_new_id(void) { static Font f = 0x10000; return f++; }
static uint32_t c_millis(void) { return 0; }
static int  c_init_fs(FontPathElementPtr f, FontBlockHandlerProcPtr b) { (void) f; (void) b; return 0; }
static int  c_register(const xfont2_fpe_funcs_rec *funcs) { fpes[nfpes] = funcs; return nfpes++; }
static void c_remove_fs(FontPathElementPtr f, FontBlockHandlerProcPtr b, Bool a) { (void) f; (void) b; (void) a; }
static void *c_server_client(void) { return NULL; }
static int  c_font_auth(char **a, int *l, void *c) { *a = NULL; *l = 0; (void) c; return 0; }
static int  c_store(FontPtr p, Font id) { (void) p; (void) id; return 1; }
static Atom c_make_atom(const char *s, unsigned len, int makeit) { static Atom a = 1; (void) s; (void) len; (void) makeit; return a++; }
static int  c_valid_atom(Atom a) { (void) a; return 1; }
static const char *c_atom_name(Atom a) { (void) a; return "atom"; }
static unsigned long c_generation(void) { return 1; }
static int  c_add_fd(int fd, FontFdHandlerProcPtr h, void *d) { (void) fd; (void) h; (void) d; return 0; }
static void c_remove_fd(int fd) { (void) fd; }
static void c_adjust(void *w, unsigned long d) { (void) w; (void) d; }

static const xfont2_client_funcs_rec funcs = {
    XFONT2_CLIENT_FUNCS_VERSION, c_auth_gen, c_signal, c_delete_id, c_verrorf, c_find_old,
    c_res, c_ptsize, c_new_id, c_millis, c_init_fs, c_register, c_remove_fs, c_server_client,
    c_font_auth, c_store, c_make_atom, c_valid_atom, c_atom_name, c_generation, c_add_fd,
    c_remove_fd, c_adjust
};

static int
try_open(FontPathElementPtr fpe, const char *name)
{
    const xfont2_fpe_funcs_rec *f = fpes[fpe->type];
    FontPtr pfont = NULL;
    char *alias = NULL;
    int hops, ret;
    fsBitmapFormat format = BitmapFormatByteOrderLSB | BitmapFormatBitOrderLSB |
                            BitmapFormatImageRectMin | BitmapFormatScanlinePad32 |
                            BitmapFormatScanlineUnit32;
    fsBitmapFormatMask fmask = BitmapFormatMaskByte | BitmapFormatMaskBit |
                               BitmapFormatMaskImageRectangle | BitmapFormatMaskScanLinePad |
                               BitmapFormatMaskScanLineUnit;
    for (hops = 0; hops < 5; hops++) {
        ret = (*f->open_font) (NULL, fpe, FontLoadAll, name, (int) strlen(name), format, fmask,
                               0x10001, &pfont, &alias, NULL);
        printf("  open_font(\"%s\") -> %d%s\n", name, ret,
               ret == Successful ? " Successful" : ret == BadFontName ? " BadFontName" :
               ret == FontNameAlias ? " FontNameAlias" : ret == AllocError ? " AllocError" : "");
        if (ret == FontNameAlias && alias) {
            name = alias;
            continue;
        }
        break;
    }
    if (ret == Successful && pfont)
        printf("  font: ascent %d descent %d, chars %d..%d\n", pfont->info.fontAscent,
               pfont->info.fontDescent, pfont->info.firstCol, pfont->info.lastCol);
    return ret == Successful ? 0 : 1;
}

int
main(void)
{
    FontPathElementRec fpe;
    int i, type = -1, failures = 0;

    printf("xfont2_init -> %d\n", xfont2_init(&funcs));
    printf("fpe types registered: %d\n", nfpes);
    for (i = 0; i < nfpes; i++)
        if ((*fpes[i]->name_check) ("built-ins")) {
            type = i;
            break;
        }
    printf("built-ins fpe type: %d\n", type);
    if (type < 0)
        return 2;
    memset(&fpe, 0, sizeof fpe);
    fpe.name = "built-ins";
    fpe.name_length = 9;
    fpe.type = type;
    printf("init_fpe -> %d\n", (*fpes[type]->init_fpe) (&fpe));
    failures += try_open(&fpe, "fixed");
    failures += try_open(&fpe, "cursor");
    printf("exact names (no alias, no scaling):\n");
    try_open(&fpe, "-misc-fixed-medium-r-semicondensed--13-120-75-75-c-60-iso8859-1");
    try_open(&fpe, "6x13");
    {
        FontNamesPtr names = xfont2_make_font_names_record(64);
        int r = (*fpes[type]->list_fonts) (NULL, &fpe, "*", 1, 64, names);
        int i;
        printf("list_fonts(\"*\") -> %d, %d names\n", r, names ? names->nnames : -1);
        for (i = 0; names && i < names->nnames && i < 8; i++)
            printf("   %.*s\n", names->length[i], names->names[i]);
    }
    printf(failures ? "FONT PROBE FAILED\n" : "FONT PROBE PASSED\n");
    return failures;
}
