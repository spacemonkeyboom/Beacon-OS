/*
 * Copyright 2022, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 *
 * Adapted from src/add-ons/kernel/busses/pci/designware/kernel_interface.cpp
 * for the StarFive JH7110 PCIe driver.
 * Modified by Beacon contributors.
 */


#include "StarFivePCIe.h"


device_manager_info* gDeviceManager;


pci_controller_module_info gPciControllerDriver = {
	.info = {
		.info = {
			.name = STARFIVE_PCI_DRIVER_MODULE_NAME,
		},
		.supports_device = StarFivePCIeController::SupportsDevice,
		.register_device = StarFivePCIeController::RegisterDevice,
		.init_driver = [](device_node* node, void** driverCookie) {
			return StarFivePCIeController::InitDriver(node,
				*(StarFivePCIeController**)driverCookie);
		},
		.uninit_driver = [](void* driverCookie) {
			return static_cast<StarFivePCIeController*>(driverCookie)
				->UninitDriver();
		},
	},
	.read_pci_config = [](void* cookie,
		uint8 bus, uint8 device, uint8 function,
		uint16 offset, uint8 size, uint32* value) {
		return static_cast<StarFivePCIeController*>(cookie)
			->ReadConfig(bus, device, function, offset, size, *value);
	},
	.write_pci_config = [](void* cookie,
		uint8 bus, uint8 device, uint8 function,
		uint16 offset, uint8 size, uint32 value) {
		return static_cast<StarFivePCIeController*>(cookie)
			->WriteConfig(bus, device, function, offset, size, value);
	},
	.get_max_bus_devices = [](void* cookie, int32* count) {
		return static_cast<StarFivePCIeController*>(cookie)
			->GetMaxBusDevices(*count);
	},
	.read_pci_irq = [](void* cookie,
		uint8 bus, uint8 device, uint8 function,
		uint8 pin, uint8 *irq) {
		return static_cast<StarFivePCIeController*>(cookie)
			->ReadIrq(bus, device, function, pin, *irq);
	},
	.write_pci_irq = [](void* cookie,
		uint8 bus, uint8 device, uint8 function,
		uint8 pin, uint8 irq) {
		return static_cast<StarFivePCIeController*>(cookie)
			->WriteIrq(bus, device, function, pin, irq);
	},
	.get_range = [](void *cookie, uint32 index, pci_resource_range* range) {
		return static_cast<StarFivePCIeController*>(cookie)
			->GetRange(index, range);
	},
	.finalize = [](void *cookie) {
		return static_cast<StarFivePCIeController*>(cookie)->Finalize();
	}
};


_EXPORT module_dependency module_dependencies[] = {
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&gDeviceManager },
	{}
};

_EXPORT module_info *modules[] = {
	(module_info *)&gPciControllerDriver,
	NULL
};
