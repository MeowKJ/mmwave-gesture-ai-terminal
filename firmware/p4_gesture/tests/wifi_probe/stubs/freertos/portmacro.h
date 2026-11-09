#pragma once
/* A checked lock model: deterministic interleavings, not an SMP spinlock. */
typedef struct { unsigned held; } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED {0}
void wifi_test_enter_critical(portMUX_TYPE *mux);
void wifi_test_exit_critical(portMUX_TYPE *mux);
#define portENTER_CRITICAL(mux) wifi_test_enter_critical(mux)
#define portEXIT_CRITICAL(mux) wifi_test_exit_critical(mux)
