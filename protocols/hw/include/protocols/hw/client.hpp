#pragma once

#include <async/result.hpp>
#include <dtb.hpp>
#include <helix/ipc.hpp>
#include <memory>
#include <optional>
#include <vector>

namespace pci {

enum {
	Vendor = 0x00,
	Device = 0x02,
	Revision = 0x08,
	SubsystemVendor = 0x2C,
	SubsystemDevice = 0x2E,
};

} // namespace pci

namespace protocols {
namespace hw {

enum IoType {
	kIoTypeNone = 0,
	kIoTypePort = 1,
	kIoTypeMemory = 2
};

struct BarInfo {
	IoType ioType;
	IoType hostType;
	uintptr_t address;
	size_t length;
	ptrdiff_t offset;
};

struct ExpansionRomInfo {
	uintptr_t address;
	size_t length;
};

struct Capability {
	unsigned int type;
};

struct PciInfo {
	BarInfo barInfo[6];
	ExpansionRomInfo expansionRomInfo;
	std::vector<Capability> caps;
	unsigned int numMsis;
	bool msiX = false;
};

struct DmaSpaceInfo {
	bool iommuActive;
	helix::UniqueDescriptor space;
	// Whether DMA by the device snoops the CPU caches.
	bool dmaCoherent;
};

struct FbInfo {
	uint64_t pitch;
	uint64_t width;
	uint64_t height;
	uint64_t bpp;
	uint64_t type;
};

struct BatteryState {
	bool charging;
	std::optional<uint64_t> current_now = std::nullopt;
	std::optional<uint64_t> power_now = std::nullopt;
	std::optional<uint64_t> energy_now = std::nullopt;
	std::optional<uint64_t> energy_full = std::nullopt;
	std::optional<uint64_t> energy_full_design = std::nullopt;
	std::optional<uint64_t> voltage_now = std::nullopt;
	std::optional<uint64_t> voltage_min_design = std::nullopt;
};

struct AcpiMemoryRange {
	uintptr_t address;
	size_t length;
	ptrdiff_t offset;
};

struct AcpiResources {
	std::vector<uint16_t> io_ports;
	std::vector<uint32_t> irqs;
	std::vector<AcpiMemoryRange> memoryRanges;
};

struct DtRegister {
	uintptr_t address;
	size_t length;
	ptrdiff_t offset;
};

struct DtInfo {
	std::vector<DtRegister> regs;
	uint32_t numIrqs;
};

struct DtProperty {
	DtProperty(std::vector<uint8_t> data) : data_{std::move(data)} {}

	size_t size() const { return data_.size(); }

	const std::vector<uint8_t> &data() const {
		return data_;
	}

	dtb::Accessor access() {
		return dtb::Accessor{frg::span{(const std::byte *)data_.data(), data_.size()}, 0};
	}

	uint32_t asU32(size_t offset = 0) {
		assert(offset + 4 <= data_.size());

		arch::scalar_storage<uint32_t, arch::big_endian> v;
		memcpy(&v, data_.data() + offset, 4);

		return v.load();
	}

	uint64_t asU64(size_t offset = 0) {
		assert(offset + 8 <= data_.size());

		arch::scalar_storage<uint64_t, arch::big_endian> v;
		memcpy(&v, data_.data() + offset, 8);

		return v.load();
	}

	frg::optional<frg::string_view> asString(size_t index = 0) {
		size_t total = 0;
		const char *off = reinterpret_cast<const char *>(data_.data());
		for (size_t i = 0; i < index; i++) {
			total += strnlen(off + total, data_.size() - total) + 1;
			if (total >= data_.size())
				return frg::null_opt;
		}
		return frg::string_view{off + total, strnlen(off + total, data_.size() - total)};
	}

	uint64_t asPropArrayEntry(size_t nCells, size_t offset = 0) {
		if (nCells == 0)
			return 0;
		else if (nCells == 1)
			return asU32(offset);
		else if (nCells == 2)
			return asU64(offset);

		assert(!"Invalid amount of cells");
		return -1;
	}

private:
	std::vector<uint8_t> data_;
};

struct Device {
	Device(helix::UniqueLane lane)
	:_lane(std::move(lane)) { };

	async::result<PciInfo> getPciInfo();
	async::result<helix::UniqueDescriptor> accessBar(int index);
	async::result<helix::UniqueDescriptor> accessExpansionRom();
	async::result<helix::UniqueDescriptor> accessIrq(size_t index = 0);
	async::result<helix::UniqueDescriptor> installMsi(int index);

	async::result<DtInfo> getDtInfo();
	async::result<std::string> getDtPath();
	async::result<std::optional<DtProperty>> getDtProperty(std::string_view name);
	async::result<std::vector<std::pair<std::string, DtProperty>>> getDtProperties();
	async::result<helix::UniqueDescriptor> accessDtRegister(uint32_t index);
	async::result<helix::UniqueDescriptor> installDtIrq(uint32_t index);

	async::result<void> claimDevice();
	async::result<void> enableBusIrq();
	async::result<void> enableMsi();
	async::result<void> enableBusmaster();
	async::result<void> enableDma(bool passthrough = true);

	async::result<uint32_t> loadPciSpace(size_t offset, unsigned int size);
	async::result<void> storePciSpace(size_t offset, unsigned int size, uint32_t word);
	async::result<uint32_t> loadPciCapability(unsigned int index, size_t offset, unsigned int size);

	async::result<FbInfo> getFbInfo();
	async::result<helix::UniqueDescriptor> accessFbMemory();
	async::result<std::pair<helix::UniqueDescriptor, uint32_t>> getVbt();

	async::result<DmaSpaceInfo> getDmaSpace();

	async::result<void> getBatteryState(BatteryState &state, bool block = false);

	async::result<std::shared_ptr<AcpiResources>> getResources();
	async::result<helix::UniqueDescriptor> accessAcpiMemory(uint32_t index);

	async::result<std::vector<uint8_t>> getSmbiosHeader();
	async::result<std::vector<uint8_t>> getSmbiosTable();

private:
	helix::UniqueLane _lane;
};

} } // namespace protocols::hw
