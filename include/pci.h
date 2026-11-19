#ifndef HISPERIA_PCI_H
#define HISPERIA_PCI_H
#include <stdint.h>
struct pci_device {
    uint8_t bus, device, function;
    uint16_t vendor_id, device_id;
    uint8_t class_code, subclass, prog_if, revision;
    uint8_t header_type;
};
int pci_find_class(uint8_t class_code, uint8_t subclass, uint8_t prog_if,
                   struct pci_device *out);
uint32_t pci_read32(const struct pci_device *dev, uint8_t offset);
int pci_enable_memory_busmaster(const struct pci_device *dev);
#endif
