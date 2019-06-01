
#include <string.h>
#include <iostream>
#include <sstream>

#include <cofiber.hpp>
#include <protocols/mbus/client.hpp>

#include "../common.hpp"
#include "../drvcore.hpp"
#include "../util.hpp"
#include "../vfs.hpp"
#include "fs.pb.h"

namespace pci_subsystem {

drvcore::BusSubsystem *sysfsSubsystem;

std::unordered_map<int, std::shared_ptr<drvcore::Device>> mbusMap;

struct VendorAttribute : sysfs::Attribute {
	VendorAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<std::string> show(sysfs::Object *object) override;
};

struct DeviceAttribute : sysfs::Attribute {
	DeviceAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<std::string> show(sysfs::Object *object) override;
};

struct PlainfbAttribute : sysfs::Attribute {
	PlainfbAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<std::string> show(sysfs::Object *object) override;
};

VendorAttribute vendorAttr{"vendor"};
DeviceAttribute deviceAttr{"device"};
PlainfbAttribute plainfbAttr{"owns_plainfb"};

struct Device : drvcore::BusDevice {
	Device(std::string sysfs_name)
	: drvcore::BusDevice{sysfsSubsystem, std::move(sysfs_name), nullptr} { }

	void composeUevent(std::stringstream &ss) override {
		ss << "SUBSYSTEM=pci" << '\0';
	}

	uint32_t vendorId;
	uint32_t deviceId;
	bool ownsPlainfb = false;
};

COFIBER_ROUTINE(async::result<std::string>, VendorAttribute::show(sysfs::Object *object),
		([=] {
	char buffer[7]; // The format is 0x1234\0.
	auto device = static_cast<Device *>(object);
	sprintf(buffer, "0x%.4x", device->vendorId);
	COFIBER_RETURN(std::string{buffer});
}))

COFIBER_ROUTINE(async::result<std::string>, DeviceAttribute::show(sysfs::Object *object),
		([=] {
	char buffer[7]; // The format is 0x1234\0.
	auto device = static_cast<Device *>(object);
	sprintf(buffer, "0x%.4x", device->deviceId);
	COFIBER_RETURN(std::string{buffer});
}))

COFIBER_ROUTINE(async::result<std::string>, PlainfbAttribute::show(sysfs::Object *object),
		([=] {
	auto device = static_cast<Device *>(object);
	COFIBER_RETURN(device->ownsPlainfb ? "1" : "0");
}))

COFIBER_ROUTINE(cofiber::no_future, run(), ([] {
	sysfsSubsystem = new drvcore::BusSubsystem{"pci"};

	auto root = COFIBER_AWAIT mbus::Instance::global().getRoot();

	auto filter = mbus::Conjunction({
		mbus::EqualsFilter("unix.subsystem", "pci")
	});

	auto handler = mbus::ObserverHandler{}
	.withAttach([] (mbus::Entity entity, mbus::Properties properties) {
		std::string sysfs_name = "0000:" + std::get<mbus::StringItem>(properties["pci-bus"]).value
				+ ":" + std::get<mbus::StringItem>(properties["pci-slot"]).value
				+ "." + std::get<mbus::StringItem>(properties["pci-function"]).value;

		// TODO: Add bus/slot/function to this message.
		std::cout << "POSIX: Installing PCI device " << sysfs_name
				<< " (mbus ID: " << entity.getId() << ")" << std::endl;

		auto device = std::make_shared<Device>(sysfs_name);
		device->vendorId = std::stoi(std::get<mbus::StringItem>(
				properties["pci-vendor"]).value, 0, 16);
		device->deviceId = std::stoi(std::get<mbus::StringItem>(
				properties["pci-device"]).value, 0, 16);
		if(properties.find("class") != properties.end()
				&& std::get<mbus::StringItem>(properties["class"]).value == "framebuffer")
			device->ownsPlainfb = true;

		drvcore::installDevice(device);
		// TODO: Call realizeAttribute *before* installing the device.
		device->realizeAttribute(&vendorAttr);
		device->realizeAttribute(&deviceAttr);
		device->realizeAttribute(&plainfbAttr);

		mbusMap.insert(std::make_pair(entity.getId(), device));
	});

	COFIBER_AWAIT root.linkObserver(std::move(filter), std::move(handler));
}))

std::shared_ptr<drvcore::Device> getDeviceByMbus(int id) {
	auto it = mbusMap.find(id);
	assert(it != mbusMap.end());
	return it->second;
}

} // namespace pci_subsystem

