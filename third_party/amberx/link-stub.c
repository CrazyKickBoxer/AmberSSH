/* link-stub.c — the entry point for the LINK probe. It references nothing:
 * /WHOLEARCHIVE pulls every AmberXCore object in regardless, so the linker
 * reports every unresolved external across the whole core. That list is the
 * specification for the Windows os layer. Original AmberSSH file. */
int main(void) { return 0; }
