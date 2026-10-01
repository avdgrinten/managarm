#pragma once

#include <compare>
#include <stdint.h>
#include <type_traits>

namespace spec {

enum class DataTransfer {
	PRP,
	SGL,
};

enum CommandOpcode {
	kWrite = 0x01,
	kRead = 0x02,
};

enum class AdminOpcode {
	DeleteSQ = 0x0,
	CreateSQ = 0x1,
	DeleteCQ = 0x4,
	CreateCQ = 0x5,
	Identify = 0x6,
	SetFeatures = 0x9,
	KeepAlive = 0x18,
	Fabrics = 0x7F,
};

enum class FabricsCommand : uint16_t {
	PropertySet = 0x00,
	Connect = 0x01,
	PropertyGet = 0x04,
	AuthenticationSend = 0x05,
	AuthenticationReceive = 0x06,
	Disconnect = 0x08,
};

enum CommandFlags {
	kQueuePhysContig = 1 << 0,
	kCQIrqEnabled = 1 << 1,
};

enum IdentifyCNS {
	kIdentifyNamespace = 0x00,
	kIdentifyController = 0x01,
	kIdentifyActiveList = 0x02,
};

struct SglGeneric {
	uint8_t __reserved1[15];
	uint8_t sglSubType: 4;
	uint8_t sglDescriptorType: 4;
};

struct SglDataBlock {
	uint64_t address;
	uint32_t length;
	uint8_t __reserved1[3];
	uint8_t sglSubType: 4;
	uint8_t sglDescriptorType: 4;
};

union Sgl {
	SglGeneric generic;
	SglDataBlock dataBlock;
};
static_assert(sizeof(Sgl) == 16);

struct PowerState {
	uint16_t maxPower;
	uint8_t __reserved2;
	uint8_t flags;
	uint32_t entryLatency;
	uint32_t exitLatency;
	uint8_t readThroughput;
	uint8_t readLatency;
	uint8_t writeThroughput;
	uint8_t writeLatency;
	uint16_t idlePower;
	uint8_t idleScale;
	uint8_t __reserved19;
	uint16_t activePower;
	uint8_t activeWorkScale;
	uint8_t __reserved23[9];
};
static_assert(sizeof(PowerState) == 32);

struct IdentifyController {
	uint16_t vid;
	uint16_t ssvid;
	char sn[20];
	char mn[40];
	char fr[8];
	uint8_t rab;
	uint8_t ieee[3];
	uint8_t cmic;
	uint8_t mdts;
	uint16_t cntlid;
	uint32_t ver;
	uint32_t rtd3r;
	uint32_t rtd3e;
	uint32_t oaes;
	uint32_t ctratt;
	uint8_t __reserved100[11];
	uint8_t cntrltype;
	uint8_t __reserved112[16];
	uint16_t crdt1;
	uint16_t crdt2;
	uint16_t crdt3;
	uint8_t __reserved134[122];
	uint16_t oacs;
	uint8_t acl;
	uint8_t aerl;
	uint8_t frmw;
	uint8_t lpa;
	uint8_t elpe;
	uint8_t npss;
	uint8_t avscc;
	uint8_t apsta;
	uint16_t wctemp;
	uint16_t cctemp;
	uint16_t mtfa;
	uint32_t hmpre;
	uint32_t hmmin;
	uint8_t tnvmcap[16];
	uint8_t unvmcap[16];
	uint32_t rpmbs;
	uint16_t edstt;
	uint8_t dsto;
	uint8_t fwug;
	uint16_t kas;
	uint16_t hctma;
	uint16_t mntmt;
	uint16_t mxtmt;
	uint32_t sanicap;
	uint32_t hmminds;
	uint16_t hmmaxd;
	uint8_t __reserved338[4];
	uint8_t anatt;
	uint8_t anacap;
	uint32_t anagrpmax;
	uint32_t nanagrpid;
	uint8_t __reserved352[160];
	uint8_t sqes;
	uint8_t cqes;
	uint16_t maxcmd;
	uint32_t nn;
	uint16_t oncs;
	uint16_t fuses;
	uint8_t fna;
	uint8_t vwc;
	uint16_t awun;
	uint16_t awupf;
	uint8_t nvscc;
	uint8_t nwpc;
	uint16_t acwu;
	uint8_t __reserved534[2];
	uint32_t sgls;
	uint32_t mnan;
	uint8_t __reserved544[224];
	char subnqn[256];
	uint8_t __reserved1024[768];
	uint32_t ioccsz;
	uint32_t iorcsz;
	uint16_t icdoff;
	uint8_t ctrattr;
	uint8_t msdbd;
	uint8_t __reserved1804[244];
	PowerState psd[32];
	uint8_t vs[1024];
};
static_assert(sizeof(IdentifyController) == 0x1000);

struct LbaFormat {
	uint16_t ms;
	uint8_t ds;
	uint8_t rp;
};
static_assert(sizeof(LbaFormat) == 4);

struct IdentifyNamespace {
	uint64_t nsze;
	uint64_t ncap;
	uint64_t nuse;
	uint8_t nsfeat;
	uint8_t nlbaf;
	uint8_t flbas;
	uint8_t mc;
	uint8_t dpc;
	uint8_t dps;
	uint8_t nmic;
	uint8_t rescap;
	uint8_t fpi;
	uint8_t dlfeat;
	uint16_t nawun;
	uint16_t nawupf;
	uint16_t nacwu;
	uint16_t nabsn;
	uint16_t nabo;
	uint16_t nabspf;
	uint16_t noiob;
	uint8_t nvmcap[16];
	uint16_t npwg;
	uint16_t npwa;
	uint16_t npdg;
	uint16_t npda;
	uint16_t nows;
	uint8_t __reserved74[18];
	uint32_t anagrpid;
	uint8_t __reserved96[3];
	uint8_t nsattr;
	uint16_t nvmsetid;
	uint16_t endgid;
	uint8_t nguid[16];
	uint8_t eui64[8];
	LbaFormat lbaf[16];
	uint8_t __reserved192[192];
	uint8_t vs[3712];
};
static_assert(sizeof(IdentifyNamespace) == 0x1000);

union DataPointer {
	struct {
		uint64_t prp1;
		uint64_t prp2;
	} prp;
	Sgl sgl;
};
static_assert(sizeof(DataPointer) == 16);

struct CommonCommand {
	uint8_t opcode;
	uint8_t flags;
	uint16_t commandId;
	uint32_t namespaceId;
	uint32_t cdw2[2];
	uint64_t metadata;
	DataPointer dataPtr;
	uint32_t cdw10;
	uint32_t cdw11;
	uint32_t cdw12;
	uint32_t cdw13;
	uint32_t cdw14;
	uint32_t cdw15;
};

struct ReadWriteCommand {
	uint8_t opcode;
	uint8_t flags;
	uint16_t commandId;
	uint32_t nsid;
	uint64_t __reserved2;
	uint64_t metadata;
	DataPointer dataPtr;
	uint64_t startLba;
	uint16_t length;
	uint16_t control;
	uint32_t dsMgmt;
	uint32_t refTag;
	uint16_t appTag;
	uint16_t appMask;
};

struct CreateCQCommand {
	uint8_t opcode;
	uint8_t flags;
	uint16_t commandId;
	uint32_t __reserved1[5];
	uint64_t prp1;
	uint64_t __prp2;
	uint16_t cqid;
	uint16_t qSize;
	uint16_t cqFlags;
	uint16_t irqVector;

	uint32_t __reserved2[4];
};

struct CreateSQCommand {
	uint8_t opcode;
	uint8_t flags;
	uint16_t commandId;
	uint32_t __reserved1[5];
	uint64_t prp1;
	uint64_t __prp2;
	uint16_t sqid;
	uint16_t qSize;
	uint16_t sqFlags;
	uint16_t cqid;
	uint32_t __reserved2[4];
};

struct IdentifyCommand {
	uint8_t opcode;
	uint8_t flags;
	uint16_t commandId;
	uint32_t nsid;
	uint64_t __reserved2[2];
	DataPointer dataPtr;
	uint8_t cns;
	uint8_t __reserved3;
	uint16_t controllerId;
	uint32_t __reserved11[5];
};

struct SetFeaturesCommand {
	uint8_t opcode;
	uint8_t flags;
	uint16_t commandId;
	uint32_t nsid;
	uint64_t __reserved2[2];
	DataPointer dataPtr;
	uint32_t data[6];
};

namespace fabric {

struct ConnectCommand {
	uint8_t opcode;
	uint8_t flags;
	uint16_t commandId;
	uint8_t fabricsCommandType;
	uint8_t __reserved1[19];
	Sgl sgl1;
	uint16_t recordFormat;
	uint16_t queueId;
	uint16_t sqSize;
	uint8_t connectAttrs;
	uint32_t keepAliveTimeout;
	uint8_t __reserved2[9];
};
static_assert(sizeof(ConnectCommand) == 64);

struct ConnectCommandData {
	uint8_t hostIdentifier[16];
	uint16_t controllerId;
	uint8_t __reserved1[238];
	char subsystemNqn[256];
	char hostNqn[256];
	uint8_t __reserved2[256];
};
static_assert(sizeof(ConnectCommandData) == 1024);

struct PropertySetCommand {
	uint8_t opcode;
	uint8_t flags;
	uint16_t commandId;
	uint8_t fabricsCommandType;
	uint8_t __reserved1[35];
	uint8_t attributes;
	uint8_t __reserved2[3];
	uint32_t offset;
	uint64_t value;
	uint8_t __reserved3[8];
};
static_assert(sizeof(PropertySetCommand) == 64);

struct PropertyGetCommand {
	uint8_t opcode;
	uint8_t flags;
	uint16_t commandId;
	uint8_t fabricsCommandType;
	uint8_t __reserved1[35];
	uint8_t attributes;
	uint8_t __reserved2[3];
	uint32_t offset;
	uint8_t __reserved3[16];
};
static_assert(sizeof(PropertyGetCommand) == 64);

} // namespace fabric

union Command {
	CommonCommand common;
	ReadWriteCommand readWrite;
	CreateCQCommand createCQ;
	CreateSQCommand createSQ;
	IdentifyCommand identify;
	SetFeaturesCommand setFeatures;
	fabric::ConnectCommand fabricConnect;
	fabric::PropertySetCommand fabricPropertySet;
	fabric::PropertyGetCommand fabricPropertyGet;
};
static_assert(sizeof(Command) == 64);

struct CompletionStatus {
	uint16_t status;

	enum class CodeType {
		Generic = 0x00,
		CommandSpecific = 0x01,
		MediaAndDataIntegretyError = 0x02,
		PathRelated = 0x03,
		VendorSpecific = 0x07,
	};

	CodeType codeType() const {
		return CodeType{(status & 0x0E00) >> 9};
	}

	uint8_t code() const {
		return (status & 0x01FE) >> 1;
	}

	bool successful() const {
		return codeType() == CodeType::Generic && code() == 0;
	}

	auto operator<=>(const CompletionStatus &) const = default;
};

static_assert(std::is_standard_layout<CompletionStatus>() && std::is_trivial<CompletionStatus>());
static_assert(sizeof(CompletionStatus) == 2);

struct CompletionEntry {
	union Result {
		uint16_t u16;
		uint32_t u32;
		uint64_t u64;
	} result;
	uint16_t sqHead;
	uint16_t sqId;
	uint16_t commandId;
	CompletionStatus status;
};
static_assert(sizeof(CompletionEntry) == 16);

// NVMe TCP transport
namespace tcp {

enum class PduType : uint8_t {
	ICReq = 0x00,
	ICResp = 0x01,
	H2CTermReq = 0x02,
	C2HTermReq = 0x03,
	CapsuleCmd = 0x04,
	CapsuleResp = 0x05,
	H2CData = 0x06,
	C2HData = 0x07,
	R2T = 0x09,
	KDReq = 0x0A,
	KDResp = 0x0B,
};

struct PduCommonHeader {
	PduType pduType;
	uint8_t flags;
	uint8_t headerLength;
	uint8_t pduDataOffset;
	uint32_t pduLength;
};

struct ICReq {
	PduCommonHeader ch;
	uint16_t pduFormatVersion;
	uint8_t hostPduDataAlignment;
	uint8_t digest;
	uint32_t maxr2t;
	uint8_t reserved[112];
};

struct ICResp {
	PduCommonHeader ch;
	uint16_t pduFormatVersion;
	uint8_t controllerPduDataAlignment;
	uint8_t digest;
	uint32_t maxh2cdata;
	uint8_t reserved[112];
};

struct CapsuleCmd {
	PduCommonHeader ch;
};

struct CapsuleResp {
	PduCommonHeader ch;
	CompletionEntry responseCqe;
};

struct C2HData {
	PduCommonHeader ch;
	uint16_t commandCapsuleId;
	uint8_t __reserved1[2];
	uint32_t dataOffset;
	uint32_t dataLength;
	uint8_t __reserved2[4];
};

struct H2CData {
	PduCommonHeader ch;
	uint16_t commandCapsuleId;
	uint16_t transferTag;
	uint32_t dataOffset;
	uint32_t dataLength;
	uint8_t __reserved[4];
};

struct R2T {
	PduCommonHeader ch;
	uint16_t commandCapsuleId;
	uint16_t transferTag;
	uint32_t r2tOffset;
	uint32_t r2tLength;
	uint8_t __reserved[4];
};

constexpr uint8_t pduFlagDataLast = 1 << 2;

} // namespace tcp

} // namespace spec
