/*
 * Copyright (c) 2015 Thomas Roell.  All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal with the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 *  1. Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimers.
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimers in the
 *     documentation and/or other materials provided with the distribution.
 *  3. Neither the name of Thomas Roell, nor the names of its contributors
 *     may be used to endorse or promote products derived from this Software
 *     without specific prior written permission.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
 * CONTRIBUTORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * WITH THE SOFTWARE.
 */

#include <stdio.h>

#include "armv7m.h"

#include "stm32l4xx.h"
#include "stm32l4_system.h"
#include "stm32l4_rtc.h"

typedef struct _armv7m_systick_control_t {
    volatile uint64_t         micros;
    volatile uint64_t         millis;
    uint64_t                  count;
    uint32_t                  cycle;
    uint32_t                  frac;
    uint32_t                  accum;
    uint32_t                  scale;
    armv7m_systick_callback_t callback;
    void                      *context;
} armv7m_systick_control_t;

static armv7m_systick_control_t armv7m_systick_control;

uint64_t armv7m_systick_millis(void)
{
    return armv7m_systick_control.millis;
}

uint64_t armv7m_systick_micros(void)
{
    uint64_t micros;
    uint32_t count;
    
    do
    {
	micros = armv7m_systick_control.micros;
	count = SysTick->VAL;
    }
    while (micros != armv7m_systick_control.micros);

    micros += ((((armv7m_systick_control.cycle - 1) - count) * armv7m_systick_control.scale) >> 22);

    return micros;
}

void armv7m_systick_delay(uint32_t delay)
{
    uint32_t millis;

    millis = armv7m_systick_control.millis;
    
    do
    {
	armv7m_core_yield();
    }
    while ((armv7m_systick_control.millis - millis) < delay);
}

void armv7m_systick_notify(armv7m_systick_callback_t callback, void *context)
{
    armv7m_systick_control.callback = NULL;
    armv7m_systick_control.context = context;
    armv7m_systick_control.callback = callback;
}

void armv7m_systick_initialize(unsigned int priority)
{
    NVIC_SetPriority(SysTick_IRQn, priority);

    armv7m_systick_control.cycle = stm32l4_system_sysclk() / 1000;
    armv7m_systick_control.frac  = stm32l4_system_sysclk() - (armv7m_systick_control.cycle * 1000);
    armv7m_systick_control.accum = 0;

    SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk;
    SysTick->VAL  = (armv7m_systick_control.cycle - 1);
    SysTick->LOAD = (armv7m_systick_control.cycle - 1);
    SysTick->CTRL = (SysTick_CTRL_CLKSOURCE_Msk | SysTick_CTRL_TICKINT_Msk | SysTick_CTRL_ENABLE_Msk);

    /* To get from the current counter to the microsecond offset,
     * the ((cycle - 1) - Systick->VAL) value is scaled so that the resulting
     * microseconds fit into the upper 10 bits of a 32bit value. Then
     * this is post diveded by 2^22. That ensures proper scaling.
     */
    armv7m_systick_control.scale = (uint64_t)4194304000000ull / (uint64_t)stm32l4_system_sysclk();
    armv7m_systick_control.millis = 0;
    armv7m_systick_control.micros = 0;
}

void armv7m_systick_enable(void)
{
    uint32_t count, o_count, o_cycle;
    uint64_t delta;

    /* Compute a new adjusted count value based upon the new SystemCoreClock.
     */
    o_cycle = armv7m_systick_control.cycle;
    o_count = (armv7m_systick_control.cycle - 1) - SysTick->VAL;

    armv7m_systick_control.cycle = SystemCoreClock / 1000;

    count = (o_count * armv7m_systick_control.cycle) / o_cycle;

    /* Compute the delta between disable() -> enable() in terms of 32768 clocks.
     */
    delta = stm32l4_rtc_get_count() - armv7m_systick_control.count;

    /* Adjust the current count by the left over sub millis second delta. 
     * The calculation is a tad cryptic, just so that expensive
     * 64 bit division can be avoided.
     *
     * The remainder in 32768Hz units of 1ms is:
     *
     * ((uint32_t)(delta * 1000ull) & 32767) / 1000
     *
     * Rescaling this to CPU clock cycles is:
     *
     * ((((uint32_t)(delta * 1000ull) & 32767) / 1000) * SystemCoreClock) / 32768
     *
     * Or in other words:
     *
     * ((((uint32_t)(delta * 1000ull) & 32767) / 1000) * (armv7m_systick_control.cycle * 1000)) / 32768
     */

    count += ((((uint32_t)(delta * 1000ull) & 32767) * armv7m_systick_control.cycle) / 32768);

    while (count >= armv7m_systick_control.cycle)
    {
	armv7m_systick_control.millis += 1;
	armv7m_systick_control.micros += 1000;
	
	count -= armv7m_systick_control.cycle;
    }

    SysTick->VAL  = (armv7m_systick_control.cycle - 1) - count;
    SysTick->LOAD = (armv7m_systick_control.cycle - 1);
    SysTick->CTRL = (SysTick_CTRL_CLKSOURCE_Msk | SysTick_CTRL_TICKINT_Msk | SysTick_CTRL_ENABLE_Msk);

    armv7m_systick_control.millis += ((delta * 1000ull) / 32768);
    armv7m_systick_control.micros = armv7m_systick_control.millis * 1000;
}

void armv7m_systick_disable(void)
{
    SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk;

    armv7m_systick_control.count = stm32l4_rtc_get_count();
}

void SysTick_Handler(void)
{
    armv7m_systick_control.micros += 1000;
    armv7m_systick_control.millis += 1;

    /* If SYSCLK is driven throu MSI with LSE PLL then the frequency
     * is not a multiple of 1000. Hence use a fractional scheme to
     * distribute the fractional part.
     */
    if (armv7m_systick_control.frac)
    {
	armv7m_systick_control.accum += armv7m_systick_control.frac;

	if (armv7m_systick_control.accum >= 1000)
	{
	    armv7m_systick_control.accum -= 1000;

	    SysTick->LOAD = (armv7m_systick_control.cycle - 1) + 1;
	}
	else
	{
	    SysTick->LOAD = (armv7m_systick_control.cycle - 1);
	}
    }

    if (armv7m_systick_control.callback) 
    {
	armv7m_pendsv_enqueue((armv7m_pendsv_routine_t)armv7m_systick_control.callback, armv7m_systick_control.context, (uint32_t)armv7m_systick_control.millis);
    }
}
