#include <async/mutex.hpp>
#include <async/result.hpp>
#include <frg/mutex.hpp>
#include <blockfs.hpp>
#include <scsi.hpp>

#include <protocols/usb/server.hpp>

enum Signatures {
	kSignCbw = 0x43425355,
	kSignCsw = 0x53425355
};

struct [[ gnu::packed ]] CommandBlockWrapper {
	uint32_t signature;
	uint32_t tag;
	uint32_t transferLength;
	uint8_t flags;
	uint8_t lun;
	uint8_t cmdLength;
	uint8_t cmdData[16];
};
static_assert(sizeof(CommandBlockWrapper) == 31);

struct [[ gnu::packed ]] CommandStatusWrapper {
	uint32_t signature;
	uint32_t tag;
	uint32_t dataResidue;
	uint8_t status;
};
static_assert(sizeof(CommandStatusWrapper) == 13);

struct StorageDevice : scsi::StorageDevice {
	StorageDevice(protocols::usb::Device usb_device, int64_t parent_id)
	: scsi::StorageDevice(512, parent_id, &pool_),
	  usbDevice_(std::move(usb_device)),
	  endp_in_{nullptr},
	  endp_out_{nullptr} {
		// Same limit as Linux' usb-storage (for non-USB3 devices).
		maxSectorsPerCommand = 240;
	}

	async::result<void> initialize(int config_num, int intf_num);

	async::result<frg::expected<scsi::Error, size_t>> sendScsiCommand(const scsi::CommandInfo &info) override;

private:
	arch::dma_realm dmaRealm_;
	arch::contiguous_pool pool_{&dmaRealm_, {.addressBits = 64}};

	// Bulk-only transport allows only one command at a time.
	async::mutex transportMutex_;

	protocols::usb::Device usbDevice_;
	protocols::usb::Endpoint endp_in_;
	protocols::usb::Endpoint endp_out_;
};

