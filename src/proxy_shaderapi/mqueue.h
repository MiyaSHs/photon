/* P6Q (analysis/27): a Photon function queued on the material system's call queue, run in order on the MatQueue thread when the frame is
 * played back. Main thread only (the queue's owner), like the engine's own queued calls. */
#ifndef PHOTON_MQUEUE_H
#define PHOTON_MQUEUE_H
typedef void (*PhotonMqFn)(void *obj, void *a, int b);
int photon_mq_queue(PhotonMqFn fn, void *obj, void *a, int b);   /* 1 queued; 0 not (immediate context, arena full, or off) */
void photon_mq_test_draw(int end);                                /* PHOTON_TEST_MQ=1: a counting call before (0) / after (1) a model draw */
void photon_mq_dump(const char *why);
#endif
