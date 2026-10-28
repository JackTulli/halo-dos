/*
DOS_PCI.C

The PCI bus as DOS sees it: configuration space through the host bridge's
ports (configuration mechanism 1: an address at CF8h, the data at CFCh),
which every PCI chipset of a Pentium has and DOSBox-X emulates. For the
3dfx Voodoo's driver (dos_voodoo.c): find a card by its vendor and device,
read and write its registers.
*/

#include "dos_dpmi.h"
#include "dos_pci.h"

static __inline unsigned long port_in32(unsigned short port)
{
	unsigned long value;

	__asm__ __volatile__("inl %1, %0" : "=a"(value) : "Nd"(port));
	return value;
}

static __inline void port_out32(unsigned short port, unsigned long value)
{
	__asm__ __volatile__("outl %0, %1" : : "a"(value), "Nd"(port));
}

static unsigned long address_of(const struct dos_pci_device *device, int offset)
{
	return 0x80000000UL | ((unsigned long)device->bus << 16) | ((unsigned long)device->slot << 11) |
		((unsigned long)device->function << 8) | ((unsigned long)offset & 0xFC);
}

unsigned long dos_pci_read(const struct dos_pci_device *device, int offset)
{
	unsigned long flags = dos_interrupts_disable(), value;

	port_out32(0xCF8, address_of(device, offset));
	value = port_in32(0xCFC);
	dos_interrupts_restore(flags);
	return value;
}

void dos_pci_write(const struct dos_pci_device *device, int offset, unsigned long value)
{
	unsigned long flags = dos_interrupts_disable();

	port_out32(0xCF8, address_of(device, offset));
	port_out32(0xCFC, value);
	dos_interrupts_restore(flags);
}

int dos_pci_present(void)
{
	unsigned long flags = dos_interrupts_disable(), saved, answer;

	/* (mechanism 1: the address register reads back what was written) */
	saved = port_in32(0xCF8);
	port_out32(0xCF8, 0x80000000UL);
	answer = port_in32(0xCF8);
	port_out32(0xCF8, saved);
	dos_interrupts_restore(flags);
	return answer == 0x80000000UL;
}

int dos_pci_find(unsigned short vendor, unsigned short device_id, struct dos_pci_device *device)
{
	if (!dos_pci_present())
		return 0;
	for (int bus = 0; bus < 4; bus++)
	{
		for (int slot = 0; slot < 32; slot++)
		{
			struct dos_pci_device candidate = { (unsigned char)bus, (unsigned char)slot, 0 };
			unsigned long id = dos_pci_read(&candidate, 0);

			if ((id & 0xFFFF) == vendor && (id >> 16) == device_id)
			{
				*device = candidate;
				return 1;
			}
		}
	}
	return 0;
}
