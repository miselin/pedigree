/*
 * Copyright (c) 2007 Kevin Wolf
 *
 * This program is free software. It comes without any warranty, to
 * the extent permitted by applicable law. You can redistribute it
 * and/or modify it under the terms of the Do What The Fuck You Want
 * To Public License, Version 2, as published by Sam Hocevar. See
 * http://sam.zoy.org/projects/COPYING.WTFPL for more details.
 */

#include <cdi-osdep.h>
#include <cdi.h>
#include <cdi/lists.h>
#include <cdi/pci.h>
#include <stdbool.h>
#include <stddef.h>
#include "modules/Module.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/machine/Device.h"
#include "pedigree/kernel/machine/Pci.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/StaticString.h"
#include "pedigree/kernel/utilities/String.h"
#include "pedigree/kernel/utilities/Vector.h"
#include "pedigree/kernel/utilities/new"

static cdi_list_t drivers = NULL;
static cdi_list_t devices = NULL;

static void cdi_destroy(void);

void cdi_run_drivers(void);

static Device *handleDevice(Device *p)
{
    struct cdi_pci_device *dev = new struct cdi_pci_device;
    dev->bus_data.bus_type = CDI_PCI;

    // Enable bus mastering and standard I/O as needed.
#if X86_COMMON
    uint32_t nPciCmdSts = PciBus::instance().readConfigSpace(p, 1);
    PciBus::instance().writeConfigSpace(p, 1, nPciCmdSts | 0x7);
#endif

    dev->bus = p->getPciBusPosition();
    dev->dev = p->getPciDevicePosition();
    dev->function = p->getPciFunctionNumber();

    dev->vendor_id = p->getPciVendorId();
    dev->device_id = p->getPciDeviceId();

    dev->class_id = p->getPciClassCode();
    dev->subclass_id = p->getPciSubclassCode();
    dev->interface_id = p->getPciProgInterface();

    dev->rev_id = 0; /// \todo Implement into Device
    
    dev->irq = p->getInterruptNumber();

    dev->resources = cdi_list_create();
    for(size_t j = 0; j < p->addresses().count(); j++)
    {
        Device::Address *addr = p->addresses()[j];
        struct cdi_pci_resource *res = new struct cdi_pci_resource;
        if(addr->m_IsIoSpace)
            res->type = CDI_PCI_IOPORTS;
        else
            res->type = CDI_PCI_MEMORY;
        res->start = addr->m_Address;
        res->length = addr->m_Size;
        res->index = j;
        res->address = reinterpret_cast<void*>(res->start);
        
        cdi_list_push(dev->resources, res);
    }

    dev->meta.backdev = reinterpret_cast<void *>(p);

    cdi_list_push(devices, dev);

    return p;
}

/**
 * Muss vor dem ersten Aufruf einer anderen CDI-Funktion aufgerufen werden.
 * Initialisiert interne Datenstruktur der Implementierung fuer das jeweilige
 * Betriebssystem.
 */
void cdi_init(void)
{
    drivers = cdi_list_create();
    devices = cdi_list_create();

    // Iterate the device tree and add cdi_bus_data structs to the device list
    // for each found device.
    Device::foreach(handleDevice);
}

static bool cdi_module_init()
{
    cdi_init();
    return true;
}

EXPORTED_PUBLIC bool cdi_pedigree_walk_dev_list_init(struct cdi_driver *dev)
{
    bool bFound = false;

    struct cdi_driver* driver = dev;
    struct cdi_bus_data* device;
    int i;
    for (i = 0; (device = reinterpret_cast<struct cdi_bus_data*>(cdi_list_get(devices, i))); i++) {
        if (driver->init_device) {
            struct cdi_device *p = driver->init_device(device);
            if(p)
            {
                p->driver = driver;
                bFound = true;
            }
        }
    }

    return bFound;
}

EXPORTED_PUBLIC void cdi_pedigree_walk_dev_list_destroy(struct cdi_driver *dev)
{
    struct cdi_driver* driver = dev;
    struct cdi_device* device;
    int i;
    for (i = 0; (device = reinterpret_cast<struct cdi_device*>(cdi_list_get(devices, i))); i++) {
        if (driver->remove_device) {
            driver->remove_device(device);
        }
    }
}

/**
 * Wird bei der Deinitialisierung aufgerufen
 */
static void cdi_destroy(void)
{
    // Drivers are already destroyed by the module exit function
}

/**
 * Fuehrt alle registrierten Treiber aus. Nach dem Aufruf dieser Funktion
 * duerfen keine weiteren Befehle ausgefuehrt werden, da nicht definiert ist,
 * ob und wann die Funktion zurueckkehrt.
 */
void cdi_run_drivers(void)
{
    // Geraete initialisieren
    struct cdi_driver* driver;
    int i;
    for (i = 0; (driver = reinterpret_cast<struct cdi_driver*>(cdi_list_get(drivers, i))); i++) {
        cdi_pedigree_walk_dev_list_init(driver);
    }
}

/**
 * Initialisiert die Datenstrukturen fuer einen Treiber
 */
void cdi_driver_init(struct cdi_driver* driver)
{
    driver->devices = cdi_list_create();
}

/**
 * Deinitialisiert die Datenstrukturen fuer einen Treiber
 */
void cdi_driver_destroy(struct cdi_driver* driver)
{
    cdi_list_destroy(driver->devices);
}

/**
 * Registriert den Treiber fuer ein neues Geraet
 *
 * @param driver Zu registierender Treiber
 */
void cdi_driver_register(struct cdi_driver* driver)
{
    cdi_list_push(drivers, driver);
}

int cdi_provide_device(struct cdi_bus_data *device)
{
    // What type is the device?
    switch(device->bus_type)
    {
        case CDI_PCI:
            {
                // Grab the cdi_pci_device for this device
                struct cdi_pci_device *pci = reinterpret_cast<struct cdi_pci_device *>(device);
                
                // Don't re-create existing devices
                if(pci->meta.backdev)
                    return -1;
                
                // Create a new device object to add to the tree
                Device *pDevice = new Device();
                
                // PCI data
                pDevice->setPciPosition(pci->bus, pci->dev, pci->function);
                pDevice->setPciIdentifiers(pci->class_id, pci->subclass_id, pci->vendor_id, pci->device_id, pci->interface_id);
                pDevice->setInterruptNumber(pci->irq);
                
                // PCI BARs
                struct cdi_pci_resource *pResource = 0;
                for(int i = 0; (pResource = reinterpret_cast<struct cdi_pci_resource *>(cdi_list_get(pci->resources, i))); i++)
                {
                    TinyStaticString barName("BAR");
                    barName.append(i, 10);
                    
                    Device::Address *pAddress = new Device::Address(String(barName), pResource->start, pResource->length, pResource->type == CDI_PCI_IOPORTS);
                    
                    pDevice->addresses().pushBack(pAddress);
                }
                
                // Link into the tree
                Device::addToRoot(pDevice);
                
                return 0;
            }
        
        default:
            WARNING("CDI: Unimplemented device type for cdi_provide_device(): " << static_cast<int>(device->bus_type));
            break;
    };
    
    return -1;
}

MODULE_INFO("cdi", &cdi_module_init, &cdi_destroy, "dma", "network-stack", "vfs");
