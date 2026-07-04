#ifndef _BETA_H_
#define _BETA_H_

// Marbles fast beta-distribution shaper (Emilie Gillet, MIT), ported to
// Teletype. Warps a uniform [0,1) value through a fixed beta(3,3) inverse-CDF
// (a symmetric bell with fatter tails) - the character of the Marbles X output.
// Faithful port of FastBetaDistributionSample (random/distributions.h) using a
// single precomputed table; no bias/spread controls (those need 45 tables /
// ~68 KB and do not fit in program flash).

float beta_fast(float uniform);  // uniform in [0,1) -> shaped value in [0,1)

#endif
