/*
 * Copyright (c) 2023-2024 Ian Marco Moffett and the Osmora Team.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of Hyra nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <dev/pci/pci.h>
#include <dev/pci/pcivar.h>
#include <sys/cdefs.h>
#include <sys/panic.h>
#include <sys/queue.h>
#include <sys/syslog.h>
#include <sys/errno.h>
#include <vm/dynalloc.h>
#include <machine/bus.h>
#if defined(__x86_64__)
#include <machine/io.h>
#endif
#include <assert.h>

__MODULE_NAME("pci");
__KERNEL_META("$Hyra$: pci.c, Ian Marco Moffett, "
              "PCI driver core");

#define pr_trace(fmt, ...) kprintf("pci: " fmt, ##__VA_ARGS__)

static TAILQ_HEAD(, pci_device) device_list;
static int access_method = PCI_ACCESS_CAM;

/*
 * Read device's legacy PCI CAM space
 *
 * @dev: Device to read.
 * @offset: Offset to read at.
 *
 * XXX: Do not use directly!
 */
static uint32_t
pci_cam_read(const struct pci_device *dev, uint32_t offset)
{
#if defined(__x86_64__)
    uint32_t address, data;

    address = __BIT(31)         |
              (offset & ~3)     |
              (dev->func << 8)  |
              (dev->slot << 11) |
              (dev->bus << 16);

    outl(0xCF8, address);
    data = inl(0xCFC) >> ((offset & 3) * 8);
    return data;
#else
    panic("Invalid arch (%s())\n", __func__);
#endif
}

/*
 * Write to device's legacy PCI CAM space
 *
 * @dev: Device to write to.
 * @offset: Offset to write at.
 *
 * XXX: Do not use directly!
 */
static void
pci_cam_write(const struct pci_device *dev, uint32_t offset, uint32_t value)
{
#if defined(__x86_64__)
    uint32_t address;

    address = __BIT(31)         |
              (offset & ~3)     |
              (dev->func << 8)  |
              (dev->slot << 11) |
              (dev->bus << 16);

    outl(0xCF8, address);
    outb(0xCFC, value);
#else
    panic("Invalid arch (%s())\n", __func__);
#endif
}

static bool
pci_device_exists(uint8_t bus, uint8_t slot, uint8_t func)
{
    uint16_t vendor_id;
    struct pci_device dev_tmp = {
        .bus = bus,
        .slot = slot,
        .func = func
    };

    vendor_id = pci_cam_read(&dev_tmp, 0x0) & 0xFFFF;

    if (vendor_id == 0xFFFF) {
        return false;
    }

    return true;
}

/*
 * Sets other device information e.g., device id, vendor id, etc
 *
 * @dev: Device descriptor to set up.
 *
 * XXX: Expects device bus, slot and func to be set.
 */
static void
pci_set_device_info(struct pci_device *dev)
{
    uint32_t classrev;

    dev->vendor_id = pci_readl(dev, PCIREG_VENDOR_ID) & __MASK(16);
    dev->device_id = pci_readl(dev, PCIREG_DEVICE_ID) & __MASK(16);
    classrev = pci_readl(dev, PCIREG_CLASSREV);

    dev->pci_class = PCIREG_CLASS(classrev);
    dev->pci_subclass = PCIREG_SUBCLASS(classrev);
    dev->prog_if = PCIREG_PROGIF(classrev);

    dev->bar[0] = pci_readl(dev, PCIREG_BAR0);
    dev->bar[1] = pci_readl(dev, PCIREG_BAR1);
    dev->bar[2] = pci_readl(dev, PCIREG_BAR2);
    dev->bar[3] = pci_readl(dev, PCIREG_BAR3);
    dev->bar[4] = pci_readl(dev, PCIREG_BAR4);
    dev->bar[5] = pci_readl(dev, PCIREG_BAR5);

    dev->irq_line = pci_readl(dev, PCIREG_IRQLINE) & __MASK(8);
}

static void
pci_register_device(uint8_t bus, uint8_t slot, uint8_t func)
{
    struct pci_device *dev = NULL;

    if (!pci_device_exists(bus, slot, func)) {
        return;
    }

    dev = dynalloc(sizeof(struct pci_device));
    __assert(dev != NULL);

    dev->bus = bus;
    dev->slot = slot;
    dev->func = func;

    pci_set_device_info(dev);
    TAILQ_INSERT_TAIL(&device_list, dev, link);
}

static void
pci_scan_bus(uint8_t bus)
{
    for (int slot = 0; slot < 32; ++slot) {
        for (int func = 0; func < 8; ++func) {
            pci_register_device(bus, slot, func);
        }
    }
}

/*
 * Convert a BAR number to BAR register offset.
 *
 * @dev: Device of BAR to check.
 * @bar: Bar number.
 */
static uint8_t
pci_get_barreg(struct pci_device *dev, uint8_t bar)
{
    switch (bar) {
    case 0: return PCIREG_BAR0;
    case 1: return PCIREG_BAR1;
    case 2: return PCIREG_BAR2;
    case 3: return PCIREG_BAR3;
    case 4: return PCIREG_BAR4;
    case 5: return PCIREG_BAR5;
    default: return 0;
    }
}

/*
 * Map a PCI(e) BAR into kernel memory.
 *
 * @dev: Device of BAR to map.
 * @bar: BAR number to map.
 * @vap: Resulting virtual address.
 */
int
pci_map_bar(struct pci_device *dev, uint8_t bar, void **vap)
{
    uint8_t bar_reg = pci_get_barreg(dev, bar);
    uintptr_t tmp;
    uint32_t size;

    if (bar_reg == 0) {
        return -EINVAL;
    }

    /*
     * Get the length of the region this BAR covers by writing a
     * mask of 32 bits into the BAR register and seeing how many
     * bits are unset. We can use this to compute the size of the
     * region. We know that log2(len) bits must be unset.
     */
    tmp = pci_readl(dev, bar_reg);
    pci_writel(dev, bar_reg, __MASK(32));
    size = pci_readl(dev, bar_reg);
    size = ~size + 1;

    /* Now we need to restore the previous value */
    pci_writel(dev, bar_reg, tmp);

    /* Now do the actual mapping work */
    return bus_map(dev->bar[bar], size, 0, vap);
}

/*
 * Read PCI(e) configuration space.
 *
 * @dev: Device to read from.
 * @offset: Offset to read at.
 */
uint32_t
pci_readl(struct pci_device *dev, uint32_t offset)
{
    if (access_method == PCI_ACCESS_CAM) {
        return pci_cam_read(dev, offset);
    }

    panic("Invalid access method (%s())\n", __func__);
    __builtin_unreachable();
}

/*
 * Write to PCI(e) configuration space.
 *
 * @dev: Device to write to.
 * @offset: Offset to write at.
 */
void
pci_writel(struct pci_device *dev, uint32_t offset, uint32_t val)
{
    if (access_method == PCI_ACCESS_CAM) {
        pci_cam_write(dev, offset, val);
        return;
    }

    panic("Invalid access method (%s())\n", __func__);
    __builtin_unreachable();
}

/*
 * Set command register bits.
 *
 * @dev: Device whose command register to modify.
 * @bits: Bits to set.
 */
void
pci_set_cmdreg(struct pci_device *dev, uint16_t bits)
{
    uint32_t tmp;

    tmp = pci_readl(dev, 0x4) | bits;
    pci_writel(dev, 0x4, tmp);
}

struct pci_device *
pci_get_device(struct pci_lookup lookup, uint16_t lookup_type)
{
    struct pci_device *dev;
    uint16_t lookup_matches = 0;

    TAILQ_FOREACH(dev, &device_list, link) {
        if (__TEST(lookup_type, PCI_DEVICE_ID)) {
            /* Check device ID */
            if (lookup.device_id == dev->device_id)
                lookup_matches |= PCI_DEVICE_ID;
        }

        if (__TEST(lookup_type, PCI_VENDOR_ID)) {
            /* Check vendor ID */
            if (lookup.vendor_id == dev->vendor_id)
                lookup_matches |= PCI_VENDOR_ID;
        }

        if (__TEST(lookup_type, PCI_CLASS)) {
            /* Check PCI class */
            if (lookup.pci_class == dev->pci_class)
                lookup_matches |= PCI_CLASS;
        }

        if (__TEST(lookup_type, PCI_SUBCLASS)) {
            /* Check PCI subclass */
            if (lookup.pci_subclass == dev->pci_subclass)
                lookup_matches |= PCI_SUBCLASS;
        }

        if (lookup_type == lookup_matches) {
            /* We found the device! */
            return dev;
        }
        lookup_matches = 0;
    }

    return NULL;
}

int
pci_init(void)
{
    TAILQ_INIT(&device_list);

    pr_trace("Scanning each bus...\n");

    for (uint16_t i = 0; i < 256; ++i) {
        pci_scan_bus(i);
    }

    return 0;
}
