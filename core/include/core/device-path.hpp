#pragma once

#include <charconv>
#include <expected>
#include <filesystem>
#include <format>
#include <print>
#include <ranges>
#include <stdint.h>
#include <string.h>
#include <string>
#include <vector>

struct DevicePathParser {
	enum class Error {
		invalidNodeArgument,
		// Some device along the path is not published (yet); retrying later may succeed.
		notYetAvailable,
		// The path can never resolve, e.g. because it contains unsupported nodes.
		broken,
	};

	static std::expected<DevicePathParser, Error> fromString(std::string str) {
		std::vector<Node> nodes;
		auto parts = str | std::ranges::views::split('/') | std::ranges::views::transform([](auto &&s) { return std::string_view(&*s.begin(), std::ranges::distance(s)); });

		for(auto p : parts) {
			if(p.starts_with("PciRoot(") || p.starts_with("PcieRoot(")) {
				// PciRoot is PNP0A03, PcieRoot is PNP0A08.
				bool pcie = p.starts_with("PcieRoot(");
				size_t prefix_len = pcie ? 9 : 8;
				auto uid = p.substr(prefix_len, p.size() - prefix_len - 1);
				uint32_t uid_num = 0;
				auto uid_val = integerFromString<uint32_t>(uid);
				if(uid_val)
					uid_num = uid_val.value();

				Node n{
					.type = DevicePathType::ACPI,
					.subtype = 1,
					.data = { 0x41, 0xD0, 0x0A, static_cast<uint8_t>(pcie ? 0x08 : 0x03), 0, 0, 0, 0 },
				};

				memcpy(&n.data[4], &uid_num, sizeof(uid_num));

				nodes.push_back(n);
			} else if(p.starts_with("Pci(")) {
				Node n{
					.type = DevicePathType::Hardware,
					.subtype = 1
				};

				auto slot = p.substr(4, p.find_first_of(',', 4) - 4);
				auto slot_num = integerFromString<uint8_t>(slot);
				if(!slot_num)
					return std::unexpected{slot_num.error()};

				auto func = p.substr(p.find_first_of(',', 4) + 1, p.find_first_of(')', p.find_first_of(',', 4) + 1) - (p.find_first_of(',', 4) + 1));
				auto func_num = integerFromString<uint8_t>(func);
				if(!func_num)
					return std::unexpected{func_num.error()};

				n.data.push_back(func_num.value());
				n.data.push_back(slot_num.value());

				nodes.push_back(n);
			} else {
				std::println("core/device-path: unhandled device node type '{}', skipping", p);
			}
		}

		return DevicePathParser{std::move(nodes)};
	}

	// Resolves the device path to its canonical sysfs path.
	std::expected<std::string, Error> sysfs() {
		std::filesystem::path current_path;
		std::error_code ec;

		for (const auto &n : nodes_) {
			switch (n.type) {
				case DevicePathType::ACPI: {
					switch (n.subtype) {
						case 1: { // ACPI _HID / _UID Device Path
							if (!current_path.empty()) {
								std::println("core/device-path: ACPI root is not the first node");
								return std::unexpected{Error::broken};
							}

							char letters[4];
							letters[0] = ((n.data[0] & 0b01111100) >> 2) + 0x40;
							letters[1] =
							    (((n.data[0] & 0b00000011) << 3) | ((n.data[1] & 0b11100000) >> 5))
							    + 0x40;
							letters[2] = (n.data[1] & 0b00011111) + 0x40;
							letters[3] = '\0';

							// UID is a 32-bit integer in UEFI ACPI Device Path (bytes 4..7)
							uint32_t uid = 0;
							memcpy(&uid, &n.data[4], sizeof(uid));

							std::string hid =
							    std::format("{}{:02X}{:02X}", letters, n.data[2], n.data[3]);

							// Firmware commonly uses PciRoot in device paths for host bridges
							// whose _HID is PNP0A08, so try the other host bridge _HID as well.
							std::string alt_hid;
							if (hid == "PNP0A03")
								alt_hid = "PNP0A08";
							else if (hid == "PNP0A08")
								alt_hid = "PNP0A03";

							for (const auto &candidate : {hid, alt_hid}) {
								if (candidate.empty())
									continue;

								std::filesystem::path acpi_target =
								    std::filesystem::path("/sys/bus/acpi/devices")
								    / std::format("{}:{:02x}", candidate, uid) / "physical_node";

								// Resolve the physical device (e.g. /sys/devices/pci0000:00)
								auto physical = std::filesystem::canonical(acpi_target, ec);
								if (!ec) {
									current_path = physical;
									break;
								}
							}

							if (current_path.empty()) {
								// Fallback for standard single-root-bridge systems if ACPI sysfs is
								// missing
								current_path = std::format("/sys/devices/pci0000:{:02x}", uid);
							}
							break;
						}
						default:
							std::println("core/device-path: unhandled ACPI subtype {}", n.subtype);
							return std::unexpected{Error::broken};
					}
					break;
				}

				case DevicePathType::Hardware: {
					switch (n.subtype) {
						case 1: { // PCI Device Path
							if (current_path.empty()) {
								std::println("core/device-path: PCI node without a root");
								return std::unexpected{Error::broken};
							}

							uint8_t function = n.data[0];
							uint8_t device = n.data[1];

							// Target pattern matching ":<slot>.<func>" at the end of the BDF
							std::string target_suffix =
							    std::format(":{:02x}.{:1x}", device, function);

							std::filesystem::path next_step;

							// Scan entries in current directory for matching BDF. The directory
							// does not exist until the bus is published.
							for (std::filesystem::directory_iterator it{current_path, ec}, end;
							     !ec && it != end;
							     it.increment(ec)) {
								std::string filename = it->path().filename().string();

								// Matches BDF directories like 0000:01:00.0
								if (filename.ends_with(target_suffix)) {
									next_step = it->path();
									break;
								}
							}

							if (ec)
								return ec | toError;
							// Devices on a bus are published one by one.
							if (next_step.empty())
								return std::unexpected{Error::notYetAvailable};

							// Devices behind a bridge live in the bridge's directory.
							current_path = next_step;
							break;
						}
						default:
							std::println(
							    "core/device-path: unhandled hardware subtype {}", n.subtype
							);
							return std::unexpected{Error::broken};
					}
					break;
				}

				default:
					std::println(
					    "core/device-path: unhandled devpath type {}", static_cast<int>(n.type)
					);
					return std::unexpected{Error::broken};
			}
		}

		if (current_path.empty())
			return std::unexpected{Error::broken};

		auto resolved = std::filesystem::canonical(current_path, ec);
		if (ec)
			return ec | toError;
		return resolved.string();
	}

private:
	enum class DevicePathType : uint8_t {
		Hardware = 1,
		ACPI = 2,
		Messaging = 3,
		MediaDevice = 4,
		BiosBootSpecification = 5,
	};

	struct Node {
		DevicePathType type;
		uint8_t subtype;
		std::vector<uint8_t> data;

		size_t size() const {
			return 4 + data.size();
		}
	};

	std::vector<Node> nodes_;

	DevicePathParser(std::vector<Node> nodes) : nodes_{std::move(nodes)} {}

	// Turns a filesystem error into an Error via `ec | toError`. The operator is a hidden friend,
	// so it is only found when one operand is a ToError, which is private to this class.
	struct ToError {
		friend std::unexpected<Error> operator|(std::error_code ec, ToError) {
			if (ec == std::errc::no_such_file_or_directory)
				return std::unexpected{Error::notYetAvailable};
			return std::unexpected{Error::broken};
		}
	};
	static constexpr ToError toError{};

	template<typename T>
	static std::expected<T, Error> integerFromString(std::string_view str) {
		size_t base = 10;

		if(str.starts_with("0x")) {
			base = 16;
			str = str.substr(2);
		}

		T value;
		auto res = std::from_chars(str.begin(), str.end(), value, base);
		if(res.ec == std::errc::invalid_argument || res.ec == std::errc::result_out_of_range)
			return std::unexpected{Error::invalidNodeArgument};

		return value;
	}
};
