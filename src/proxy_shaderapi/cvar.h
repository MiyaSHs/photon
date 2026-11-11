/* The engine's console variables from the proxy (NANITE: r_drawstaticprops). See cvar.c. */
#ifndef PHOTON_CVAR_H
#define PHOTON_CVAR_H
void *photon_cvar_find(const char *name);         /* a ConVar defined in engine.dll or client.dll, checked (NULL if not found or the layout does not check out) */
void *photon_cvar_find_in(const char *module, const char *name);
int photon_cvar_get_int(void *cv);
int photon_cvar_set_int(void *cv, int v);          /* 1 on success; also bypasses FCVAR_CHEAT (Photon's own setting decides) */
float photon_cvar_get_float(void *cv);
int photon_cvar_set_float(void *cv, float f);      /* as set_int; the string too when the old one has room */
#endif
