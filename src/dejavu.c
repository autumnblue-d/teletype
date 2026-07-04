#include "dejavu.h"

#define DV_MAX_UINT32 4294967296.0f

static float dv_rand(dejavu_t *d) {
    return (float)random_next(d->rng) / DV_MAX_UINT32;  // [0,1)
}

void dejavu_init(dejavu_t *d, random_state_t *rng) {
    d->rng = rng;
    for (int i = 0; i < DEJAVU_BUFFER_SIZE; i++) { d->loop[i] = dv_rand(d); }
    d->loop_write_head = 0;
    d->length = 8;
    d->step = 0;
    d->deja_vu = 0.0f;
}

// Port of RandomSequence::NextValue (non-deterministic path only). The loop
// always holds values in [0,1), so the original's ">= 1.0f" shift-register
// tagging is unnecessary here.
float dejavu_next(dejavu_t *d) {
    const float p_sqrt = 2.0f * d->deja_vu - 1.0f;
    const float p = p_sqrt * p_sqrt;
    const bool mutate = dv_rand(d) < p;

    if (mutate && d->deja_vu <= 0.5f) {
        // Generate a new value at the end of the loop.
        d->loop[d->loop_write_head] = dv_rand(d);
        d->loop_write_head = (d->loop_write_head + 1) % DEJAVU_BUFFER_SIZE;
        d->step = d->length - 1;
    }
    else if (mutate) {
        // deja_vu > 0.5: jump randomly through the loop.
        d->step = (int)(dv_rand(d) * (float)d->length);
    }
    else {
        // Replay the loop in order.
        d->step = d->step + 1;
        if (d->step >= d->length) { d->step = 0; }
    }

    uint32_t i = d->loop_write_head + DEJAVU_BUFFER_SIZE - d->length + d->step;
    return d->loop[i % DEJAVU_BUFFER_SIZE];
}

void dejavu_record(dejavu_t *d) {
    for (int i = 0; i < DEJAVU_BUFFER_SIZE; i++) { d->loop[i] = dv_rand(d); }
    d->loop_write_head = 0;
    d->step = 0;
}

void dejavu_set_deja_vu(dejavu_t *d, float amount) {
    if (amount < 0.0f) { amount = 0.0f; }
    if (amount > 1.0f) { amount = 1.0f; }
    d->deja_vu = amount;
}

void dejavu_set_length(dejavu_t *d, int length) {
    if (length < 1 || length > DEJAVU_BUFFER_SIZE) { return; }
    d->length = length;
    d->step = d->step % length;
}

float dejavu_get_deja_vu(const dejavu_t *d) {
    return d->deja_vu;
}

int dejavu_get_length(const dejavu_t *d) {
    return d->length;
}

// Option A: a single global instance seeded once at first use. Not per-scene;
// resets on power cycle. Unit tests use their own dejavu_t with a pinned seed.
static dejavu_t g_dejavu;
static random_state_t g_dejavu_rng;
static bool g_dejavu_ready = false;

dejavu_t *dejavu_global(void) {
    if (!g_dejavu_ready) {
        random_seed(&g_dejavu_rng, 0x600df00d);
        dejavu_init(&g_dejavu, &g_dejavu_rng);
        g_dejavu_ready = true;
    }
    return &g_dejavu;
}
