/* SPDX-License-Identifier: MPL-2.0 */
/* the balance: each part's loudness measured (weighted as the ear hears it: BS.1770) across 12 grooves at MOOD 0..100,
 * then held to its MOOD 50 balance at every MOOD; the mix's top-to-bottom and loudness measured likewise */
static const float BAL[NPARTS][5] = {   /* KICK, SNARE, PERC, HATS at MOOD 0, 25, 50, 75, 100: dB */
    { -0.8f, -0.4f, 0.0f, 0.3f, 0.9f },
    { -2.5f, -1.0f, 0.0f, 0.5f, 1.0f },
    { -2.2f, -1.0f, 0.0f, 2.3f, 3.9f },
    { -2.5f, -1.6f, 0.0f, 1.6f, 3.2f } };
#define BAL_TILT -10.4f                  /* the grooves' usual top over bottom, dB (measured: 36 grooves, MOOD 25-75) */
#define BAL_RMS 0.1f                     /* the leveller's loudness (the grooves' median at MOOD 50) */
