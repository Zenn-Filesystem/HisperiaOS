#ifndef HISPERIA_STACK_H
#define HISPERIA_STACK_H

#include <stdint.h>

/* Allocate a four-page supervisor IST stack with one unmapped guard page. */
int kernel_guarded_ist_stack_init(uint64_t *guard_base_out,
                                  uint64_t *stack_bottom_out,
                                  uint64_t *stack_top_out);

#endif
