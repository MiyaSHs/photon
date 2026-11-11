#ifndef PHOTON_SLOT_NAMES_H
#define PHOTON_SLOT_NAMES_H
#include <stddef.h>
typedef struct PhotonSlotNames { const char *iface; const char *const *names; size_t count; } PhotonSlotNames;
extern const PhotonSlotNames photon_slot_names[]; /* terminated by { NULL } */
#endif
