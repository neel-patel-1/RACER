/* Weak defaults for the probe symbols. A program that defines its own
 * preempt_signal and do_yield (the RACER runtime, preempt_bench) overrides
 * them; any other program that links a probed library, such as protoc, gets a
 * signal byte that stays zero, so its probes never yield. */
#include "racer_probe.h"

static uint8_t signal_byte_never_set;

__attribute__((weak)) __thread volatile uint8_t *preempt_signal = &signal_byte_never_set;

__attribute__((weak)) void do_yield(void) {}
