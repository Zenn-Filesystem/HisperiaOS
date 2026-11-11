#ifndef HISPERIA_IDT_H
#define HISPERIA_IDT_H

#include <stdint.h>

void idt_init(void);

int idt_verify(
    uint16_t *limit_out,
    uint64_t *base_out
);

int idt_df_ist_verify(uint8_t *ist_out);

int idt_vector_uses_ist(uint8_t vector);

#endif