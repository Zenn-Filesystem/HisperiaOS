#ifndef HISPERIA_HEAP_H
#define HISPERIA_HEAP_H

#include <stddef.h>
#include <stdint.h>

int kheap_init(void);
void *kmalloc(size_t size);
void kfree(void *pointer);
size_t kheap_bytes_mapped(void);
int kheap_self_test(void);

#endif
