#pragma once

void mock_rcc_enter(void);
void mock_rcc_exit(void);

/* Model entry/exit and require both clock calls to be inside this scope.
 * This does not model locking, interrupts, or real RCC atomicity. */
#define PERIPH_RCC_ATOMIC() \
    for (int mock_rcc_once = (mock_rcc_enter(), 1); mock_rcc_once; \
         mock_rcc_exit(), mock_rcc_once = 0)
