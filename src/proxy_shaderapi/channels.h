/* Lua -> proxy data channels (channels.c, host version 8) */
#ifndef PHOTON_CHANNELS_H
#define PHOTON_CHANNELS_H
void photon_chan_push(const char *name, const float *v, int n);   /* host->push: main thread */
/* the channel's latest floats (up to max) into out; returns the count, -1 if never pushed; *frame: the frame it was pushed in */
int photon_chan_get(const char *name, float *out, int max, unsigned long long *frame);
#endif
