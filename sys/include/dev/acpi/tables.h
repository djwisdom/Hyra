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

#ifndef _ACPI_TABLES_H_
#define _ACPI_TABLES_H_

#include <sys/types.h>
#include <sys/cdefs.h>

#define OEMID_SIZE 6

struct __packed acpi_header {
    char signature[4];          /* ASCII signature string */
    uint32_t length;            /* Length of table in bytes */
    uint8_t revision;           /* Revision of the structure */
    uint8_t checksum;           /* Checksum of the header */
    char oemid[OEMID_SIZE];     /* OEM-supplied string that IDs the OEM */
    char oem_table_id[8];       /* OEM-supplied string (used by OEM) */
    uint32_t oem_revision;      /* OEM-supplied revision number */
    uint32_t creator_id;        /* Vendor ID of creator utility */
    uint32_t creator_revision;  /* Revision of creator utility */
};

struct __packed acpi_rsdp {
    uint64_t signature;         /* RSD PTR */
    uint8_t checksum;           /* Structure checksum */
    char oemid[OEMID_SIZE];     /* OEM-supplied string that IDs the OEM */
    uint8_t revision;           /* Revision of the structure */
    uint32_t rsdt_addr;         /* RSDT physical address */

    /* Reserved if revision < 2 */
    uint32_t length;            /* Length of table in bytes */
    uint64_t xsdt_addr;         /* XSDT physical address */
    uint8_t ext_checksum;       /* Extended checksum */
    uint8_t reserved[3];
};

/*
 * XSDT or RSDT depending
 * on what revision the header
 * says.
 */
struct __packed acpi_root_sdt {
    struct acpi_header hdr;
    uint32_t tables[];
};

struct __packed acpi_madt {
    struct acpi_header hdr;
    uint32_t lapic_addr;
    uint32_t flags;
};

struct __packed apic_header {
    uint8_t type;
    uint8_t length;
};

struct __packed local_apic {
    struct apic_header hdr;
    uint8_t processor_id;
    uint8_t apic_id;
    uint32_t flags;
};

struct __packed ioapic {
    struct apic_header hdr;
    uint8_t ioapic_id;
    uint8_t reserved;
    uint32_t ioapic_addr;
    uint32_t gsi_base;
};

struct __packed interrupt_override {
    struct apic_header hdr;
    uint8_t bus;
    uint8_t source;         /* IRQ */
    uint32_t interrupt;     /* GSI */
    uint16_t flags;
};

struct __packed acpi_gas {
    uint8_t address_space_id;
    uint8_t register_bit_width;
    uint8_t register_bit_offset;
    uint8_t reserved;
    uint64_t address;
};

struct __packed acpi_hpet {
    struct acpi_header hdr;
    uint8_t hardware_rev_id;
    uint8_t comparator_count : 5;
    uint8_t counter_size : 1;
    uint8_t reserved : 1;
    uint8_t legacy_replacement : 1;
    uint16_t pci_vendor_id;
    struct acpi_gas gas;
    uint8_t hpet_number;
    uint16_t minimum_tick;
    uint8_t page_protection;
};

#endif  /* _ACPI_TABLES_H_ */
