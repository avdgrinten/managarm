#include <assert.h>
#include <format>
#include <net/if.h>
#include <netinet/in.h>
#include <optional>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <async/algorithm.hpp>
#include <async/result.hpp>
#include <bragi/helpers-std.hpp>
#include <core/clock.hpp>
#include <core/cmdline.hpp>
#include <core/dispatch.hpp>
#include <frg/cmdline.hpp>
#include <hel.h>
#include <hel-syscalls.h>
#include <helix/ipc.hpp>
#include <protocols/mbus/client.hpp>
#include <protocols/hw/client.hpp>
#include <protocols/svrctl/server.hpp>
#include <protocols/fs/server.hpp>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include "fs.bragi.hpp"

#include "ip/ip4.hpp"
#include "netlink/netlink.hpp"
#include "raw.hpp"

#include <netserver/nic.hpp>
#include <nic/virtio/virtio.hpp>
#include <nic/rtl8168/rtl8168.hpp>
#ifdef __riscv
#include <nic/k1x-emac/k1x-emac.hpp>
#endif
#include <nic/bcmgenet/bcmgenet.hpp>
#ifdef __x86_64__
# include <nic/freebsd-e1000/common.hpp>
# include <nic/igc/igc.hpp>
#endif
#include <nic/usb_net/usb_net.hpp>
#include <protocols/ostrace/ostrace.hpp>
#include <protocols/usb/client.hpp>

// Maps mbus IDs to device objects
std::unordered_map<int64_t, std::shared_ptr<nic::Link>> baseDeviceMap;

std::optional<helix::UniqueDescriptor> posixLane;

const std::string VENDOR_REALTEK = "10ec";
const std::string VENDOR_DLINK = "1186";
const std::string VENDOR_TPLINK = "10ff";
const std::string VENDOR_COREGA = "1259";
const std::string VENDOR_LINKSYS = "1737";
const std::string VENDOR_US_ROBOTICS = "16ec";
const std::string VENDOR_REDHAT = "1af4";
const std::string VENDOR_INTEL = "8086";


std::unordered_set<std::string_view> nic_vendor_ids = {
	VENDOR_REDHAT, /* virtio */
	VENDOR_REALTEK, /* rtl8168 */
	VENDOR_DLINK, /* rtl8168 */
	VENDOR_TPLINK, /* rtl8168 */
	VENDOR_COREGA, /* rtl8168 */
	VENDOR_LINKSYS, /* rtl8168 */
	VENDOR_US_ROBOTICS, /* rtl8168 */
	VENDOR_INTEL, /* e1000, igc */
};

std::unordered_set<std::string_view> virtio_device_ids = {
	"1000",
	"1041",
};

std::unordered_set<std::string_view> rtl8168_device_ids = {
	"8125", /* RTL8125 */
	"8129", /* RTL8129 */
	"8136", /* RTL8136 */
	"8161", /* RTL8161 */
	"8162", /* RTL8162 */
	"8167", /* RTL8167 */
	"8168", /* RTL8168 */
	"8169", /* RTL8169 */
};

std::unordered_set<std::string_view> rtl8168_dlink_device_ids = {
	"4300",
	"4302",
};

std::unordered_set<std::string_view> intel_device_ids = {
	"100e", /* QEMU's e1000 device */
	"10d3", /* QEMU's e1000e device */
	"15d8", /* i219-V (4) */
	"57b4", /* i219-V (25) */
};

std::unordered_set<std::string_view> igc_device_ids = {
	"125c", /* i226-V */
	"125b", /* i226-LM */
	"125d", /* i226-IT */
	"15f2", /* i225-LM */
	"15f3", /* i225-V */
	"0d9f", /* i225-IT */
};

std::unordered_map<int64_t, std::shared_ptr<nic::Link>> &nic::Link::getLinks() {
	return baseDeviceMap;
}

std::shared_ptr<nic::Link> nic::Link::byIndex(int index) {
	for(auto it = baseDeviceMap.begin(); it != baseDeviceMap.end(); it++)
		if(it->second->index() == index)
			return it->second;

	return {};
}

std::shared_ptr<nic::Link> nic::Link::byName(std::string name) {
	for(auto it = baseDeviceMap.begin(); it != baseDeviceMap.end(); it++)
		if(it->second->name() == name)
			return it->second;

	return {};
}

namespace {

async::result<std::shared_ptr<nic::Link>> setupVirtioDevice(mbus_ng::Entity &base_entity, protocols::hw::Device hwDevice) {
	virtio_core::DiscoverMode discover_mode = virtio_core::DiscoverMode::null;
	auto properties = (co_await base_entity.getProperties()).unwrap();

	if(auto device_str = std::get_if<mbus_ng::StringItem>(&properties["pci-device"]);
		device_str) {
		if(device_str->value == "1000")
			discover_mode = virtio_core::DiscoverMode::transitional;
		else if(device_str->value == "1041")
			discover_mode = virtio_core::DiscoverMode::modernOnly;
		else
			assert(!"Unhandled virtio device");
	}

	auto transport = co_await virtio_core::discover(std::move(hwDevice), discover_mode);

	auto nic = co_await nic::virtio::makeShared(base_entity.id(), std::move(transport));
	co_return std::move(nic);
}

bool determineRTL8168Support(const std::string& vendor_str, const std::string& device_str) {
	if(vendor_str == VENDOR_REALTEK) {
		if(rtl8168_device_ids.contains(device_str)) {
			return true;
		}
	} else if(vendor_str == VENDOR_DLINK) {
		if(rtl8168_dlink_device_ids.contains(device_str)) {
			return true;
		}
	} else if(vendor_str == VENDOR_TPLINK) {
		if(device_str == std::string("8168")) {
			return true;
		}
	} else if(vendor_str == VENDOR_COREGA) {
		if(device_str == std::string("c107")) {
			return true;
		}
	} else if(vendor_str == VENDOR_LINKSYS) {
		if(device_str == std::string("1032")) {
			return true;
		}
	} else if(vendor_str == VENDOR_US_ROBOTICS) {
		if(device_str == std::string("0116")) {
			return true;
		}
	}
	return false;
}

} // namespace

async::result<protocols::svrctl::Error> doBindPci(mbus_ng::Entity baseEntity) {
	protocols::hw::Device hwDevice((co_await baseEntity.getRemoteLane()).unwrap());
	co_await hwDevice.enableBusmaster();

	auto properties = (co_await baseEntity.getProperties()).unwrap();
	auto vendor_str = std::get_if<mbus_ng::StringItem>(&properties["pci-vendor"]);
	auto device_str = std::get_if<mbus_ng::StringItem>(&properties["pci-device"]);

	if(!vendor_str || !nic_vendor_ids.contains(vendor_str->value) || !device_str)
		co_return protocols::svrctl::Error::deviceNotSupported;

	std::shared_ptr<nic::Link> device;

	if(vendor_str->value == VENDOR_REDHAT) {
		if(!virtio_device_ids.contains(device_str->value))
			co_return protocols::svrctl::Error::deviceNotSupported;

		protocols::hw::Device hwDevice((co_await baseEntity.getRemoteLane()).unwrap());
		co_await hwDevice.enableBusmaster();

		device = co_await setupVirtioDevice(baseEntity, std::move(hwDevice));
	} else if(determineRTL8168Support(vendor_str->value, device_str->value)) {
		device = co_await nic::rtl8168::makeShared(std::move(hwDevice));
#ifdef __x86_64__
	} else if(vendor_str->value == VENDOR_INTEL && igc_device_ids.contains(device_str->value)) {
		device = co_await nic::igc::makeShared(std::move(hwDevice));
	} else if(vendor_str->value == VENDOR_INTEL) {
		device = co_await nic::e1000::makeShared(std::move(hwDevice));
#endif
	} else {
		std::cout << std::format("netserver: skipping PCI device {}:{}\n", vendor_str->value, device_str->value);
		co_return protocols::svrctl::Error::deviceNotSupported;
	}

	// The driver declined the device.
	if(!device)
		co_return protocols::svrctl::Error::deviceNotSupported;

	baseDeviceMap.insert({baseEntity.id(), device});
	nic::runDevice(device);

	co_return protocols::svrctl::Error::success;
}

async::result<protocols::svrctl::Error> doBindUsb(mbus_ng::Entity baseEntity) {
	auto dev = protocols::usb::connect((co_await baseEntity.getRemoteLane()).unwrap());
	auto raw_desc = (co_await dev.deviceDescriptor()).value();
	auto dev_desc = reinterpret_cast<protocols::usb::DeviceDescriptor *>(raw_desc.data());

	std::optional<nic::usb_net::ConfigurationInfo> matched_usb_info;

	for(size_t configuration = 0; configuration < dev_desc->numConfigs; configuration++) {
		nic::usb_net::ConfigurationInfo usb_info{};

		auto raw_descs = (co_await dev.configurationDescriptor(configuration)).value();
		for(auto [intf, body] : protocols::usb::groupByInterface(protocols::usb::configurationRange(raw_descs))) {
			if(intf.interfaceClass == protocols::usb::usb_class::cdc) {
				switch(intf.interfaceSubClass) {
					case protocols::usb::cdc_subclass::ethernet:
						usb_info.subclass = protocols::usb::cdc_subclass::ethernet;
						usb_info.valid = true;
						break;
					case protocols::usb::cdc_subclass::ncm:
						usb_info.subclass = protocols::usb::cdc_subclass::ncm;
						usb_info.valid = true;
						break;
					case protocols::usb::cdc_subclass::mbim:
						usb_info.subclass = protocols::usb::cdc_subclass::mbim;
						usb_info.valid = true;
						break;
					default:
						std::cout << std::format("netserver: unknown CDC subclass 0x{:x}\n", intf.interfaceSubClass);
						break;
				}
			}

			// Bulk endpoints found within this single alternate setting.
			std::optional<int> alt_in;
			std::optional<int> alt_out;

			for(const auto &[head, bytes] : body) {
				if(head.descriptorType == protocols::usb::descriptor_type::cs_interface) {
					auto desc = protocols::usb::extractDescriptor<protocols::usb::CdcDescriptor>(bytes);
					if(!desc)
						continue;

					switch(desc->subtype) {
						using CdcSubType = protocols::usb::CdcDescriptor::CdcSubType;

						case CdcSubType::Header: {
							auto hdr = protocols::usb::extractDescriptor<protocols::usb::CdcHeader>(bytes);
							if(hdr)
								printf("netserver: CDC version 0x%x\n", hdr->bcdCDC);
							break;
						}
						case CdcSubType::AbstractControl: {
							auto hdr = protocols::usb::extractDescriptor<protocols::usb::CdcAbstractControl>(bytes);
							if(hdr)
								printf("netserver: ACM capabilities 0x%02x\n", hdr->bmCapabilities);
							break;
						}
						case CdcSubType::Union: {
							auto hdr = protocols::usb::extractDescriptor<protocols::usb::CdcUnion>(bytes);
							if(hdr)
								usb_info.data_if = hdr->bSubordinateInterface[0];
							break;
						}
						case CdcSubType::EthernetNetworking: {
							auto hdr = protocols::usb::extractDescriptor<protocols::usb::CdcEthernetNetworking>(bytes);
							if(hdr) {
								usb_info.iMACAddress = hdr->iMACAddress;
								if(!usb_info.control_if)
									usb_info.control_if = intf.interfaceNumber;
							}
							break;
						}
						case CdcSubType::Ncm: {
							auto hdr = protocols::usb::extractDescriptor<protocols::usb::CdcNcm>(bytes);
							if(hdr) {
								printf("netserver: NCM %x\n", hdr->bcdNcmVersion);
								usb_info.ncm = true;
							}
							break;
						}
						case CdcSubType::Mbim: {
							auto hdr = protocols::usb::extractDescriptor<protocols::usb::CdcMbim>(bytes);
							if(hdr) {
								printf("netserver: MBIM %x\n", hdr->bcdMBIMVersion);
								if(!usb_info.control_if)
									usb_info.control_if = intf.interfaceNumber;
							}
							break;
						}
						case CdcSubType::MbimExtended: {
							auto hdr = protocols::usb::extractDescriptor<protocols::usb::CdcMbimExtended>(bytes);
							if(hdr)
								printf("netserver: MBIM Extended MTU %u\n", hdr->wMTU);
							break;
						}
						default: {
							std::cout << std::format("netserver: unhandled Function Descriptor SubType {}", uint8_t(desc->subtype)) << std::endl;
							break;
						}
					}
				} else if(head.descriptorType == protocols::usb::descriptor_type::endpoint) {
					auto ep = protocols::usb::extractDescriptor<protocols::usb::EndpointDescriptor>(bytes);
					if(!ep)
						continue;
					auto epType = static_cast<protocols::usb::EndpointType>(ep->attributes & 0x03);

					if(usb_info.data_if && usb_info.data_if == intf.interfaceNumber
							&& epType == protocols::usb::EndpointType::bulk) {
						if(ep->endpointAddress & 0x80) {
							alt_in = ep->endpointAddress & 0x0F;
						} else {
							alt_out = ep->endpointAddress & 0x0F;
						}
					} else if(epType == protocols::usb::EndpointType::interrupt
							&& usb_info.control_if && usb_info.control_if == intf.interfaceNumber) {
						usb_info.int_endp_number = ep->endpointAddress & 0x0F;
					}
				}
			}

			// Mirror Linux: pick the first alternate setting that provides both bulk IN and bulk OUT.
			if(usb_info.data_if && usb_info.data_if == intf.interfaceNumber
					&& alt_in && alt_out && !usb_info.in_endp_number) {
				usb_info.data_if_alt = intf.alternateSetting;
				usb_info.in_endp_number = alt_in;
				usb_info.out_endp_number = alt_out;
			}
		}

		if(usb_info.valid && usb_info.control_if && usb_info.data_if) {
			usb_info.configuration_index = configuration;
			usb_info.chosen_configuration = reinterpret_cast<protocols::usb::ConfigDescriptor *>(raw_descs.data())->configValue;
			matched_usb_info = std::move(usb_info);
			break;
		}
	}

	if(!matched_usb_info
	|| matched_usb_info->subclass == protocols::usb::cdc_subclass::reserved
	|| !matched_usb_info->valid
	|| !matched_usb_info->chosen_configuration
	|| !matched_usb_info->control_if
	|| !matched_usb_info->data_if) {
		std::cout << std::format("netserver: skipping device {:04x}:{:04x} with mbus ID {}\n",
			dev_desc->idVendor, dev_desc->idProduct, baseEntity.id()
		);
		co_return protocols::svrctl::Error::deviceNotSupported;
	}

	std::cout << std::format("netserver: binding device {:04x}:{:04x} with mbus ID {}\n",
		dev_desc->idVendor, dev_desc->idProduct, baseEntity.id()
	);

	std::string str = "000000000000";

	if(matched_usb_info->iMACAddress)
		str = (co_await dev.getString(*matched_usb_info->iMACAddress)).value();

	auto decodeHexString = [](char c) -> uint8_t {
		if(c >= 'A' && c <= 'F')
			return c - 'A' + 0x0A;
		if(c >= 'a' && c <= 'f')
			return c - 'a' + 0x0A;
		if(c >= '0' && c <= '9')
			return c - '0';
		__builtin_unreachable();
	};

	nic::MacAddress mac{{
		static_cast<uint8_t>((decodeHexString(str[0]) << 4) | decodeHexString(str[1])),
		static_cast<uint8_t>((decodeHexString(str[2]) << 4) | decodeHexString(str[3])),
		static_cast<uint8_t>((decodeHexString(str[4]) << 4) | decodeHexString(str[5])),
		static_cast<uint8_t>((decodeHexString(str[6]) << 4) | decodeHexString(str[7])),
		static_cast<uint8_t>((decodeHexString(str[8]) << 4) | decodeHexString(str[9])),
		static_cast<uint8_t>((decodeHexString(str[10]) << 4) | decodeHexString(str[11])),
	}};

	auto device = co_await nic::usb_net::makeShared(baseEntity.id(), std::move(dev), mac, *matched_usb_info);

	baseDeviceMap.insert({baseEntity.id(), device});
	nic::runDevice(device);

	co_return protocols::svrctl::Error::success;
}

async::result<protocols::svrctl::Error> doBindDt(mbus_ng::Entity baseEntity) {
	auto properties = (co_await baseEntity.getProperties()).unwrap();

	std::shared_ptr<nic::Link> nic;
	if (properties.contains("dt.compatible=spacemit,k1x-emac")) {
#ifdef __riscv
		nic = co_await nic::k1x_emac::makeShared(baseEntity.id());
#endif
	} else if (properties.contains("dt.compatible=brcm,bcm2711-genet-v5")) {
		nic = co_await nic::bcmgenet::makeShared(baseEntity.id());
	}

	if (!nic) {
		co_return protocols::svrctl::Error::deviceNotSupported;
	}

	baseDeviceMap.insert({baseEntity.id(), nic});
	nic::runDevice(nic);

	co_return protocols::svrctl::Error::success;
}

async::result<protocols::svrctl::Error> bindDevice(int64_t base_id) {
	std::cout << "netserver: Binding to device " << base_id << std::endl;
	auto baseEntity = co_await mbus_ng::Instance::global().getEntity(base_id);

	// Do not bind to devices that are already bound to this driver.
	if(baseDeviceMap.find(baseEntity.id()) != baseDeviceMap.end())
		co_return protocols::svrctl::Error::success;

	// Make sure that we only bind to supported devices.
	auto properties = (co_await baseEntity.getProperties()).unwrap();
	auto type = std::get_if<mbus_ng::StringItem>(&properties["unix.subsystem"]);

	if(!type)
		co_return protocols::svrctl::Error::deviceNotSupported;

	if(type->value == "pci") {
		auto err = co_await doBindPci(std::move(baseEntity));

		if(err != protocols::svrctl::Error::success)
			co_return err;
	} else if(type->value == "usb") {
		auto err = co_await doBindUsb(std::move(baseEntity));

		if(err != protocols::svrctl::Error::success)
			co_return err;
	} else if(type->value == "dt") {
		auto err = co_await doBindDt(std::move(baseEntity));

		if(err != protocols::svrctl::Error::success)
			co_return err;
	}

	assert(baseDeviceMap.contains(base_id));

	auto device = baseDeviceMap.at(base_id);

	Cmdline cmdHelper;
	auto cmdline = co_await cmdHelper.get();
	frg::string_view station = "";
	frg::string_view subnet = "";
	frg::string_view gateway = "";

	frg::array args = {
		frg::option{"netserver.ip", frg::as_string_view(station)},
		frg::option{"netserver.subnet", frg::as_string_view(subnet)},
		frg::option{"netserver.gateway", frg::as_string_view(gateway)},
	};
	frg::parse_arguments(cmdline.c_str(), args);

	auto convert_ip = [](frg::string_view &str, in_addr *addr) -> bool {
		std::string strbuf{str.data(), str.size()};
		return inet_pton(AF_INET, strbuf.c_str(), addr) == 1;
	};

	in_addr station_ip{};
	in_addr subnet_mask{};
	in_addr gateway_ip{};

	bool station_valid = false;
	bool subnet_valid = false;
	bool gateway_valid = false;

	if(station.size() && subnet.size()) {
		station_valid = convert_ip(station, &station_ip);
		subnet_valid = convert_ip(subnet, &subnet_mask);
	}

	if(gateway.size())
		gateway_valid = convert_ip(gateway, &gateway_ip);

	if(~ntohl(subnet_mask.s_addr) && station_valid && subnet_valid) {
		auto prefix = static_cast<uint8_t>(__builtin_clz(~ntohl(subnet_mask.s_addr)));
		ip4().setLink({ntohl(station_ip.s_addr), prefix}, device);
		ip4Router().addRoute({ {ntohl(station_ip.s_addr & subnet_mask.s_addr), prefix}, device });
	}

	if(gateway_valid) {
		Ip4Router::Route default_route{ {0, 0}, device };
		default_route.gateway = ntohl(gateway_ip.s_addr);
		default_route.source = ntohl(station_ip.s_addr);
		ip4Router().addRoute(std::move(default_route));
	}

	nl::broadcastNewLink(device);

	co_return protocols::svrctl::Error::success;
}

namespace {

constinit protocols::ostrace::Event ostEvtRequest{"fs.request"};
constinit protocols::ostrace::UintAttribute ostAttrRequest{"request"};
constinit protocols::ostrace::UintAttribute ostAttrTime{"time"};
constinit protocols::ostrace::BragiAttribute ostBragi{managarm::fs::protocol_hash};

protocols::ostrace::Vocabulary ostVocabulary{
	ostEvtRequest,
	ostAttrRequest,
	ostAttrTime,
	ostBragi,
};

protocols::ostrace::Context ostContext{ostVocabulary};

}

static async::result<void> serveNetlinkLanes(
	helix::UniqueLane ctrlLane,
	helix::UniqueLane ptLane,
	smarter::shared_ptr<nl::NetlinkSocket> sock
) {
	co_await async::race_and_cancel(
		[&](async::cancellation_token) {
			return protocols::fs::serveFile(std::move(ctrlLane),
					sock.get(), &nl::NetlinkSocket::ops);
		},
		[&](async::cancellation_token ct) {
			return protocols::fs::servePassthrough(std::move(ptLane),
					sock, &nl::NetlinkSocket::ops, ct);
		}
	);
	sock->handleClose();
}

namespace {

struct HandleRequest {
	uint64_t id = 0;
	timespec requestTimestamp = {};

	template <typename T>
	void logBragiRequest(T &req) {
		if(!ostContext.isActive())
			return;

		requestTimestamp = clk::getTimeSinceBoot();
		std::string reqHead;
		std::string reqTail;
		reqHead.resize(req.size_of_head());
		reqTail.resize(req.size_of_tail());
		bragi::limited_writer headWriter{reqHead.data(), reqHead.size()};
		bragi::limited_writer tailWriter{reqTail.data(), reqTail.size()};
		auto headOk = req.encode_head(headWriter);
		auto tailOk = req.encode_tail(tailWriter);
		assert(headOk);
		assert(tailOk);
		ostContext.emitWithTimestamp(
			ostEvtRequest,
			(requestTimestamp.tv_sec * 1'000'000'000) + requestTimestamp.tv_nsec,
			ostAttrTime((requestTimestamp.tv_sec * 1'000'000'000) + requestTimestamp.tv_nsec),
			ostBragi({reinterpret_cast<uint8_t *>(reqHead.data()), reqHead.size()}, {reinterpret_cast<uint8_t *>(reqTail.data()), reqTail.size()})
		);
	}

	void logBragiReply(auto &resp) {
		if(!ostContext.isActive())
			return;

		auto ts = clk::getTimeSinceBoot();
		std::string replyHead;
		std::string replyTail;
		replyHead.resize(resp.size_of_head());
		replyTail.resize(resp.size_of_tail());
		bragi::limited_writer headWriter{replyHead.data(), replyHead.size()};
		bragi::limited_writer tailWriter{replyTail.data(), replyTail.size()};
		auto headOk = resp.encode_head(headWriter);
		auto tailOk = resp.encode_tail(tailWriter);
		assert(headOk);
		assert(tailOk);
		ostContext.emitWithTimestamp(
			ostEvtRequest,
			(ts.tv_sec * 1'000'000'000) + ts.tv_nsec,
			ostAttrRequest(id),
			ostAttrTime((requestTimestamp.tv_sec * 1'000'000'000) + requestTimestamp.tv_nsec),
			ostBragi({reinterpret_cast<uint8_t *>(replyHead.data()), replyHead.size()}, {reinterpret_cast<uint8_t *>(replyTail.data()), replyTail.size()})
		);
	}

	void logBragiSerializedReply(std::string &ser) {
		if(!ostContext.isActive())
			return;

		auto ts = clk::getTimeSinceBoot();
		ostContext.emitWithTimestamp(
			ostEvtRequest,
			(ts.tv_sec * 1'000'000'000) + ts.tv_nsec,
			ostAttrRequest(id),
			ostAttrTime((requestTimestamp.tv_sec * 1'000'000'000) + requestTimestamp.tv_nsec),
			ostBragi({reinterpret_cast<uint8_t *>(ser.data()), ser.size()}, {})
		);
	}

	async::result<std::expected<void, DispatchError>>
	operator()(managarm::fs::CntRequest &&req,
			helix::BorrowedDescriptor conversation, bragi::preamble preamble) {
		id = preamble.id();
		logBragiRequest(req);

		auto sendError = [&] (managarm::fs::Errors err)
				-> async::result<void> {
			managarm::fs::SvrResponse resp;
			resp.set_error(err);
			auto buff = resp.SerializeAsString();
			auto [send] =
				co_await helix_ng::exchangeMsgs(conversation,
					helix_ng::sendBuffer(
						buff.data(), buff.size())
				);
			HEL_CHECK(send.error());
			logBragiSerializedReply(buff);
		};

		if (req.req_type() == managarm::fs::CntReqType::CREATE_SOCKET) {
			auto [localCtrl, remoteCtrl] = helix::createStream();
			auto [localPt, remotePt] = helix::createStream();

			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::SUCCESS);

			if(req.domain() == AF_INET) {
				auto err = ip4().serveSocket(std::move(localCtrl),
						std::move(localPt), req.type(), req.protocol(), req.flags());
				if(err != managarm::fs::Errors::SUCCESS) {
					co_await sendError(err);
					co_return {};
				}
			} else if(req.domain() == AF_NETLINK) {
				auto nl_socket = smarter::make_shared<nl::NetlinkSocket>(req.flags(), req.protocol());
				async::detach(serveNetlinkLanes(std::move(localCtrl), std::move(localPt), nl_socket));
			} else if(req.domain() == AF_PACKET) {
				auto err = raw().serveSocket(std::move(localCtrl),
						std::move(localPt), req.type(), req.protocol(), req.flags());
				if(err != managarm::fs::Errors::SUCCESS) {
					co_await sendError(err);
					co_return {};
				}
			} else {
				std::cout << "mlibc: unexpected socket domain " << req.domain() << std::endl;
				co_await sendError(managarm::fs::Errors::ILLEGAL_ARGUMENT);
				co_return {};
			}

			auto ser = resp.SerializeAsString();
			auto [sendResp, pushCtrl, pushPt] =
				co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBuffer(
						ser.data(), ser.size()),
					helix_ng::pushDescriptor(remoteCtrl, kHelRightInvoke | kHelRightManage),
					helix_ng::pushDescriptor(remotePt, kHelRightInvoke | kHelRightManage)
				);
			HEL_CHECK(sendResp.error());
			HEL_CHECK(pushCtrl.error());
			HEL_CHECK(pushPt.error());
			logBragiSerializedReply(ser);
		} else {
			std::cout << "netserver: received unknown request type: "
				<< (int32_t)req.req_type() << std::endl;
			auto [dismiss] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::dismiss()
			);
			HEL_CHECK(dismiss.error());
		}
		co_return {};
	}

	async::result<std::expected<void, DispatchError>>
	operator()(managarm::fs::IfreqRequest &&req,
			helix::BorrowedDescriptor conversation, bragi::preamble preamble) {
		id = preamble.id();

		auto tailRes = co_await dispatchTail(req, conversation, preamble);
		if(!tailRes)
			co_return std::unexpected(tailRes.error());
		logBragiRequest(req);

		managarm::fs::IfreqReply resp;
		resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);

		if(req.command() == SIOCGIFCONF) {
			std::vector<managarm::fs::Ifconf> ifconf;

			for(auto [_, link] : nic::Link::getLinks()) {
				auto addr_check = ip4().getCidrByIndex(link->index());
				if(!addr_check)
					continue;

				managarm::fs::Ifconf conf;
				conf.set_name(link->name());
				conf.set_ip4(addr_check->ip);
				ifconf.push_back(conf);
			}

			managarm::fs::IfconfReply ifconf_resp;

			ifconf_resp.set_ifconf(std::move(ifconf));
			ifconf_resp.set_error(managarm::fs::Errors::SUCCESS);

			auto [send_head, send_tail] =
				co_await helix_ng::exchangeMsgs(conversation,
					helix_ng::sendBragiHeadTail(ifconf_resp, frg::stl_allocator{})
				);
			HEL_CHECK(send_head.error());
			HEL_CHECK(send_tail.error());
			logBragiReply(ifconf_resp);

			co_return {};
		}else if(req.command() == SIOCGIFNETMASK) {
			auto link = nic::Link::byName(req.name());

			if(link) {
				auto addr_check = ip4().getCidrByIndex(link->index());

				if(addr_check) {
					resp.set_ip4_netmask(addr_check.value().mask());
					resp.set_error(managarm::fs::Errors::SUCCESS);
				}else {
					resp.set_ip4_netmask(0);
				}
			} else {
				resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
			}
		}else if(req.command() == SIOCGIFINDEX) {
			auto link = nic::Link::byName(req.name());

			if(link) {
				resp.set_index(link->index());
				resp.set_error(managarm::fs::Errors::SUCCESS);
			} else {
				resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
			}
		}else if(req.command() == SIOCGIFNAME) {
			auto link = nic::Link::byIndex(req.index());

			if(link) {
				resp.set_name(link->name());
				resp.set_error(managarm::fs::Errors::SUCCESS);
			} else {
				resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
			}
		}else if(req.command() == SIOCGIFFLAGS) {
			auto link = nic::Link::byName(req.name());

			if(link) {
				resp.set_flags(IFF_UP | IFF_RUNNING | link->iff_flags());
				resp.set_error(managarm::fs::Errors::SUCCESS);
			} else {
				resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
			}
		}else if(req.command() == SIOCGIFADDR) {
			auto link = nic::Link::byName(req.name());

			if(link) {
				auto addr_check = ip4().getCidrByIndex(link->index());

				if(addr_check) {
					resp.set_ip4_addr(addr_check->ip);
					resp.set_error(managarm::fs::Errors::SUCCESS);
				} else {
					resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
				}
			} else {
				resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
			}
		}else if(req.command() == SIOCGIFMTU) {
			auto link = nic::Link::byName(req.name());

			if(link) {
				resp.set_mtu(link->mtu);
				resp.set_error(managarm::fs::Errors::SUCCESS);
			} else {
				resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
			}
		}else if(req.command() == SIOCGIFBRDADDR) {
			auto link = nic::Link::byName(req.name());

			if(link) {
				auto addr_check = ip4().getCidrByIndex(link->index());

				if(addr_check) {
					auto addr = addr_check.value().ip;
					auto mask = addr_check.value().mask();
					auto wildcard_bits = ~mask;
					addr &= mask;
					addr |= wildcard_bits;

					resp.set_ip4_broadcast_addr(addr);
					resp.set_error(managarm::fs::Errors::SUCCESS);
				} else {
					resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
				}
			} else {
				resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
			}
		}else if(req.command() == SIOCGIFHWADDR) {
			auto link = nic::Link::byName(req.name());

			if(link) {
				std::array<uint8_t, 6> mac{};
				memcpy(mac.data(), link->deviceMac().data(), 6);
				resp.set_mac(mac);
				resp.set_error(managarm::fs::Errors::SUCCESS);
			} else {
				resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
			}
		}

		auto [send, send_tail] =
			co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBragiHeadTail(resp, frg::stl_allocator{})
			);
		HEL_CHECK(send.error());
		HEL_CHECK(send_tail.error());
		logBragiReply(resp);
		co_return {};
	}

	async::result<std::expected<void, DispatchError>>
	operator()(managarm::fs::InitializePosixLane &&,
			helix::BorrowedDescriptor conversation, bragi::preamble) {
		co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::dismiss()
		);

		posixLane = helix::UniqueDescriptor{conversation.dup()};
		co_return {};
	}
};

} // namespace

async::detached serve(helix::UniqueLane lane) {
	co_await ostContext.create();

	while (true) {
		auto res = co_await dispatchRequest<
			managarm::fs::CntRequest,
			managarm::fs::IfreqRequest,
			managarm::fs::InitializePosixLane
		>(lane, HandleRequest{});
		if(!res) {
			if(res.error() == DispatchError::shutdown)
				co_return;
			std::cout << "netserver: dispatch error" << std::endl;
			continue;
		}
	}
}

async::detached advertise() {
	mbus_ng::Properties descriptor {
		{"class", mbus_ng::StringItem{"netserver"}}
	};

	auto entity = (co_await mbus_ng::Instance::global().createEntity(
		"netserver", descriptor)).unwrap();

	[] (mbus_ng::EntityManager entity) -> async::detached {
		while (true) {
			auto [localLane, remoteLane] = helix::createStream();

			// If this fails, too bad!
			(void)(co_await entity.serveRemoteLane(std::move(remoteLane)));

			serve(std::move(localLane));
		}
	}(std::move(entity));
}

static constexpr protocols::svrctl::ControlOperations controlOps = {
	.bind = bindDevice
};

// --------------------------------------------------------
// main() function
// --------------------------------------------------------

int main() {
	printf("netserver: Starting driver\n");

	async::run(clk::enumerateTracker(), helix::currentDispatcher);
	nl::initialize();

//	HEL_CHECK(helSetPriority(kHelThisThread, 3));

	auto loopbackLink = nic::getLoopback();
	baseDeviceMap.insert({-1, loopbackLink});
	ip4().setLink({INADDR_LOOPBACK, 8}, loopbackLink);
	Ip4Router::Route loopbackRoute{{INADDR_LOOPBACK, 8}, loopbackLink};
	loopbackRoute.type = RTN_LOCAL;
	loopbackRoute.protocol = RTPROT_KERNEL;
	loopbackRoute.scope = RT_SCOPE_HOST;
	loopbackRoute.source = INADDR_LOOPBACK;
	ip4Router().addRoute(loopbackRoute);
	nic::runDevice(loopbackLink);

	async::detach(protocols::svrctl::serveControl(&controlOps));
	advertise();
	async::run_forever(helix::currentDispatcher);
}
