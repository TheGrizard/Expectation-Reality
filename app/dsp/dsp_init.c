/* SPDX-License-Identifier: MPL-2.0 */
/* Expectation Reality: the sine table dsp.h's sine_c reads (After Images' own, as there). */
#include "dsp.h"

float g_sine[SINE_SIZE + 1];
void dsp_init(void)
{
    for (int i = 0; i <= SINE_SIZE; i++) g_sine[i] = fm_sin_turns((float)i / SINE_SIZE);
}
