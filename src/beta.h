#ifndef _BETA_H_
#define _BETA_H_

// Marbles fast beta-distribution shaper (Emilie Gillet, MIT), ported to
// Teletype. Warps a uniform [0,1) value through a fixed beta(3,3) inverse-CDF
// (a symmetric bell with fatter tails) - the character of the Marbles X output.
// Faithful port of FastBetaDistributionSample (random/distributions.h) using a
// single precomputed table; no bias/spread controls (those need 45 tables /
// ~68 KB and do not fit in program flash).

float beta_fast(float uniform);  // uniform in [0,1) -> shaped value in [0,1)

// Variable-shape beta-distribution surrogate (inverse-CDF), needing NO lookup
// table - one pow() call - where the full Marbles bias/spread beta shaper would
// need 45 tables (~68 KB). Built from two power half-warps meeting at the
// midpoint, so a==b is exactly symmetric and centred (unlike a raw Kumaraswamy
// K(a,a), whose median is offset). a=b>1 -> bell (mass mid), a=b<1 -> bimodal
// (mass at rails), a==b symmetric, a!=b skewed. a,b clamped > 0 internally.
float beta_shape_icdf(float uniform, float a, float b);

#endif
