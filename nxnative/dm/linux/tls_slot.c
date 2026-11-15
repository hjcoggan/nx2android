// The guest thread slot block (host/loader.c, non-Apple path) lives at tpidr_el0[2], where bionic keeps TLS_SLOT_APP and the rewritten modules
// (DM_TARGET=android) read it. glibc's aarch64 TCB is two words (dtv, private), so tpidr_el0[2] is the first word of the executable's own TLS block.
// This object goes first on the link line and its variable first in .tdata, so that word is this variable and nothing else of the host's lands there.
// front_linux.c checks the address at start-up.
__attribute__((section(".tdata"), used, visibility("hidden"))) _Thread_local void *dm_tls_slot_app = 0;
