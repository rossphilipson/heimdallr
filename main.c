/*
 * main.c
 *
 * Copyright (c) 2012 Aurelien Chartier <chartier.aurelien@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <pci/pci.h>
#include <assert.h>

#include "json.h"
#include "pci.h"

#ifdef DEBUG
# define PCIBACK_SLOTS_FILE  "/tmp/slots"
# define PCIBACK_QUIRKS_FILE "/tmp/quirks"
#else
# define PCIBACK_SLOTS_FILE  "/sys/bus/pci/drivers/pciback/slots"
# define PCIBACK_QUIRKS_FILE "/sys/bus/pci/drivers/pciback/quirks"
#endif

static pci_dev_infos*
get_pciback_devices(void)
{
    FILE *file = fopen(PCIBACK_SLOTS_FILE, "r");
    if (NULL == file)
    {
        perror (PCIBACK_SLOTS_FILE);
        return NULL;
    }

    char line[128];
    pci_dev_infos *res = NULL;

    while (fgets (line, sizeof (line), file) != NULL)
    {
        unsigned int domain, bus, dev;
        int func, scanned;

        scanned = sscanf(line, "%04x:%02x:%02x.%d\n",
                     &domain, &bus,
                     &dev, &func);
        if (4 != scanned) // the line was not well-formatted
            continue;

        res = pci_dev_infos_add(res, domain, bus, dev, func);
    }

    fclose (file);

    return res;
}

static unsigned
fill_device_with_quirks(struct pci_dev *dev,
                        pci_device_quirk *quirks)
{
    unsigned nb_quirks = 0;

    for (; quirks; quirks = quirks->next)
    {
        uint16_t vendor = strtoul(quirks->vendor, NULL, 16);
        if (vendor != 0xffff && vendor != dev->vendor_id)
            continue;

        uint16_t device = strtoul(quirks->device, NULL, 16);
        if (device != 0xffff && device != dev->device_id)
            continue;

        /* Check subvendor/subdevice ids as well */
        uint16_t subvendor = strtoul(quirks->subvendor, NULL, 16);
        uint16_t dev_subvendor = pci_read_word(dev, PCI_SUBSYSTEM_VENDOR_ID);
        if (subvendor != 0xffff && subvendor != dev_subvendor)
            continue;

        uint16_t subdevice = strtoul(quirks->subdevice, NULL, 16);
        uint16_t dev_subdevice = pci_read_word(dev, PCI_SUBSYSTEM_ID);
        if (subdevice != 0xffff && subdevice != dev_subdevice)
            continue;

        struct pci_device_field *field = quirks->fields;
        for (; field; field = field->next)
        {
            FILE *f = fopen(PCIBACK_QUIRKS_FILE, "w");
            if (!f)
            {
                perror(PCIBACK_QUIRKS_FILE);
                return nb_quirks;
            }

            uint32_t reg = strtoul(field->reg, NULL, 16);
            uint32_t size = field->size[0] - '0'; // we already tested that this field is correct
            uint32_t mask = strtoul(field->mask, NULL, 16);
            fprintf(f, "%04x:%02x:%02x.%1x-%08x:%d:%08x",
                    dev->domain, dev->bus, dev->dev, dev->func,
                    reg, size, mask);
            ++nb_quirks;

            fclose(f);
        }
    }

    return nb_quirks;
}

static unsigned
scan_all_pci_devices(pci_device_quirk *quirks,
                     pci_dev_infos    *pciback_devices)
{

    struct pci_access *pacc = pci_alloc();
    pci_init(pacc);

    unsigned nb_quirks = 0;

    while (pciback_devices)
    {
        struct pci_dev *dev = pci_get_dev(pacc, pciback_devices->domain,
                                          pciback_devices->bus,
                                          pciback_devices->dev,
                                          pciback_devices->func);
        pci_fill_info(dev, PCI_FILL_IDENT);

        nb_quirks += fill_device_with_quirks(dev, quirks);
        pci_free_dev(dev);
        pciback_devices = pciback_devices->next;
    }

    pci_cleanup(pacc);

    return nb_quirks;
}


static void
usage(void)
{
    printf("heidallr <config-file>\n");
}

int
main(int argc, char **argv)
{
    if (argc != 2)
    {
        usage();
        return 1;
    }

    pci_device_quirk *quirks = parse_json_file(argv[1]);

    if (NULL == quirks)
    {
        fprintf(stderr, "No quirks found.\n");
        return 2;
    }

    pci_dev_infos *pciback_devices = get_pciback_devices();

    if (NULL == pciback_devices)
    {
        fprintf(stderr, "No pciback device found.\n");
        return 3;
    }

    unsigned nb_quirks = scan_all_pci_devices(quirks, pciback_devices);
    printf("Added a total of %u quirks to %s\n", nb_quirks, PCIBACK_QUIRKS_FILE);

    pci_device_quirk_free(quirks);
    pci_dev_infos_free(pciback_devices);
    return 0;
}
