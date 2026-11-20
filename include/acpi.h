#ifndef HISPERIA_ACPI_H
#define HISPERIA_ACPI_H

#include <stdint.h>

/* Initial x86 ACPI fixed-event support (legacy PM1 I/O blocks). */
int acpi_power_init(uint64_t rsdp_phys, uint64_t hhdm_offset);
int acpi_power_button_poll(void);
void acpi_poweroff(void);
int acpi_power_available(void);

#endif
