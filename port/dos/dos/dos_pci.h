/*
DOS_PCI.H

The PCI bus's configuration space (dos_pci.c).
*/

#ifndef __DOS_PCI_H
#define __DOS_PCI_H

struct dos_pci_device
{
	unsigned char bus, slot, function;
};

/* whether the host bridge answers configuration mechanism 1 */
int dos_pci_present(void);
/* the first card of that vendor and device on buses 0-3: FALSE if none */
int dos_pci_find(unsigned short vendor, unsigned short device_id, struct dos_pci_device *device);
/* a configuration register (offset a multiple of 4) */
unsigned long dos_pci_read(const struct dos_pci_device *device, int offset);
void dos_pci_write(const struct dos_pci_device *device, int offset, unsigned long value);

#endif /* __DOS_PCI_H */
