/* Coordination between several clil processes on one machine, without a daemon.
 *
 * Every instance holds an flock on its own "slot.N" file for as long as it lives, so the kernel
 * tells us who is alive (no pid reuse, no stale entries). Slot N's data sits at a fixed offset in
 * one small shared mmap'd file: which server and model it uses and whether it is generating.
 * Queues and the model-load lock are flock'ed files in the same directory.
 *
 * Everything degrades to "no coordination" when the directory is unusable. */
#ifndef CLIL_STATE_H
#define CLIL_STATE_H

#ifdef __cplusplus
extern "C" {
#endif

enum { CLIL_IDLE = 0, CLIL_QUEUED = 1, CLIL_BUSY = 2, CLIL_LOADING = 3 };

/* Called while waiting, ~20x/s. `ahead` = instances in front of us (0 for plain locks).
 * Return non-zero to give up. */
typedef int (*clil_wait_cb)(void* ctx, int ahead);

typedef struct {
    char host[48];
    char model[64];
    int state;
    int self;
} clil_peer;

/* dir: NULL for $CLIL_RUN, else /tmp/clil-<uid> (must be a private dir of ours). 0 if tracking works. */
int clil_open(const char* dir);
void clil_close(void);
const char* clil_dir(void); /* run directory, "" when unusable */

/* Announce what this instance uses (host:port, model id) and its state. */
void clil_set(const char* host, const char* model, int state);

/* Other live instances: using `model` on `host` (model NULL: any model); and busy on `host`. */
int clil_users(const char* host, const char* model);
int clil_busy(const char* host);
int clil_peers(clil_peer* out, int max); /* live instances, self included */

/* FIFO queue for `slots` concurrent requests on `host`. 0 = acquired, -1 = gave up.
 * Hold it for one request only; clil_release() is safe to call without a hold. */
int clil_acquire(const char* host, int slots, clil_wait_cb cb, void* ctx);
void clil_release(void);

/* Exclusive named lock (e.g. "load.<host>"). Returns an id >= 0, -1 gave up, -2 no lock dir:
 * -2 means "carry on without". clil_unlock() accepts any of them. */
int clil_lock(const char* name, clil_wait_cb cb, void* ctx);
void clil_unlock(int id);

#ifdef __cplusplus
}
#endif
#endif
