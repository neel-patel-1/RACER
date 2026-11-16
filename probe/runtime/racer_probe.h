/* Symbols that probe points use. The RACER apps define both (see
 * src/baseline.cpp); racer_probe_runtime.c gives weak defaults for programs
 * that link probed libraries but never preempt. */
#ifndef RACER_PROBE_H
#define RACER_PROBE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Points at the status byte of the completion record of the offload in
 * flight. A probe point calls do_yield() when that byte is nonzero. */
extern __thread volatile uint8_t *preempt_signal;

/* Switches from the filler back to the scheduler. Returns when the scheduler
 * resumes the filler. */
void do_yield(void);

#ifdef __cplusplus
}
#endif

#endif /* RACER_PROBE_H */
