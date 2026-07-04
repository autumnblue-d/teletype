#ifndef _DEJAVU_H_
#define _DEJAVU_H_

#include <stdbool.h>
#include <stdint.h>

#include "random.h"  // random_state_t, random_next, random_seed

// Marbles "deja vu" random sequence (Emilie Gillet, MIT), ported to Teletype.
//
// A 16-slot loop buffer of random values in [0,1). The deja_vu amount (0..1)
// controls behavior: 0.0 locks a repeating loop of `length` steps, 0.5 plays
// fresh random, and in between it probabilistically regenerates loop slots;
// above 0.5 it jumps randomly through the loop. Stateful.
//
// Simplified from random_sequence.h: the replay / rewrite (ASR / quantizer)
// path and its history buffer are dropped - Teletype only uses the plain
// non-deterministic random output.

#define DEJAVU_BUFFER_SIZE 16

typedef struct {
    float loop[DEJAVU_BUFFER_SIZE];
    int loop_write_head;
    int length;
    int step;
    float deja_vu;
    random_state_t *rng;
} dejavu_t;

void dejavu_init(dejavu_t *d, random_state_t *rng);
float dejavu_next(dejavu_t *d);   // advance one step, return value in [0,1)
void dejavu_record(dejavu_t *d);  // re-lock: refill the loop from the RNG
void dejavu_set_deja_vu(dejavu_t *d, float amount);  // clamped 0..1
void dejavu_set_length(dejavu_t *d, int length);     // clamped 1..16
float dejavu_get_deja_vu(const dejavu_t *d);
int dejavu_get_length(const dejavu_t *d);

// Lazily-initialised global instance (Option A: not per-scene, resets at boot).
dejavu_t *dejavu_global(void);

#endif
