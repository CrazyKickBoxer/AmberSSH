/* server_main.c — the X side's entry point. Original AmberSSH file.
 *
 * dix_main (xserver/dix/main.c, compiled unmodified) is the generation loop
 * every X.Org server runs: it initialises the core, calls InitOutput and
 * InitInput, dispatches until the last client resets or a shutdown is
 * requested, and tears down. AmberX supplies the DDX hooks it calls and
 * nothing else; the host's own argument parsing happens on the Windows side
 * before this is reached. */
#include <dix-config.h>
#include "amberwin.h"

int dix_main(int argc, char *argv[], char *envp[]);

int
amberx_server_main(int argc, char **argv)
{
    char *envp[1] = { 0 };
    return dix_main(argc, argv, envp);
}
