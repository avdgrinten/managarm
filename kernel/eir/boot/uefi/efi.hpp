#pragma once

// based on UEFI 2.10 Errata A

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

// 2.3.1 Data Types
using efi_status = size_t;
using efi_handle = void *;
using efi_tpl = size_t;
using efi_event = void *;

struct alignas(8) efi_guid {
	uint32_t data1;
	uint16_t data2;
	uint16_t data3;
	uint8_t data4[8];
};

// 4.2.1 EFI_TABLE_HEADER

struct efi_table_header {
	uint64_t signature;
	uint32_t revision;
	uint32_t header_size;
	uint32_t crc32;
	uint32_t reserved;
};

// 4.3.1 EFI_SYSTEM_TABLE

struct efi_boot_services;
struct efi_configuration_table;
struct efi_simple_text_output_protocol;

struct efi_system_table {
	efi_table_header hdr;
	char16_t *firmware_vendor;
	uint32_t firmware_revision;
	efi_handle console_in_handle;
	void *con_in;
	efi_handle console_out_handle;
	struct efi_simple_text_output_protocol *con_out;
	efi_handle standard_error_handle;
	void *std_err;
	void *runtime_services;
	efi_boot_services *boot_services;
	size_t number_of_table_entries;
	efi_configuration_table *configuration_table;
};

// 4.6.1 EFI_CONFIGURATION_TABLE
struct efi_configuration_table {
	efi_guid vendor_guid;
	void *vendor_table;
};

constexpr efi_guid ACPI_20_TABLE_GUID = {
    0x8868e871, 0xe4f1, 0x11d3, {0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81}
};
constexpr efi_guid SMBIOS3_TABLE_GUID = {
    0xf2fd1544, 0x9794, 0x4a2c, {0x99, 0x2e, 0xe5, 0xbb, 0xcf, 0x20, 0xe3, 0x94}
};
constexpr efi_guid EFI_DTB_TABLE_GUID = {
    0xb1b621d5, 0xf19c, 0x41a5, {0x83, 0x0b, 0xd9, 0x15, 0x2c, 0x69, 0xaa, 0xe0}
};

// 7.2.1 EFI_BOOT_SERVICES.AllocatePages()
enum efi_allocate_type { AllocateAnyPages, AllocateMaxAddress, AllocateAddress, MaxAllocateType };

enum efi_memory_type {
	EfiReservedMemoryType,
	EfiLoaderCode,
	EfiLoaderData,
	EfiBootServicesCode,
	EfiBootServicesData,
	EfiRuntimeServicesCode,
	EfiRuntimeServicesData,
	EfiConventionalMemory,
	EfiUnusableMemory,
	EfiACPIReclaimMemory,
	EfiACPIMemoryNVS,
	EfiMemoryMappedIO,
	EfiMemoryMappedIOPortSpace,
	EfiPalCode,
	EfiPersistentMemory,
	EfiUnacceptedMemoryType,
	EfiMaxMemoryType
};

using efi_physical_addr = uint64_t;

// 7.2.3 EFI_BOOT_SERVICES.GetMemoryMap()

using efi_virtual_addr = uint64_t;

struct efi_memory_descriptor {
	uint32_t type;
	efi_physical_addr physical_start;
	efi_virtual_addr virtual_start;
	uint64_t number_of_pages;
	uint64_t attribute;
};

// 8.3.1 GetTime()

struct efi_time {
	uint16_t year;
	uint8_t month;
	uint8_t day;
	uint8_t hour;
	uint8_t minute;
	uint8_t second;
	uint8_t pad1;
	uint32_t nanosecond;
	int16_t time_zone;
	uint8_t daylight;
	uint8_t pad2;
};

// 9.1.1 EFI_LOADED_IMAGE_PROTOCOL

constexpr efi_guid EFI_LOADED_IMAGE_PROTOCOL_GUID = {
    0x5B1B31A1, 0x9562, 0x11d2, {0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}
};

struct efi_loaded_image_protocol {
	uint32_t revision;
	efi_handle parent_handle;
	efi_system_table *system_table;
	efi_handle device_handle;
	void *file_path;
	void *reserved;
	uint32_t load_options_size;
	void *load_options;
	void *image_base;
	uint64_t image_size;
	efi_memory_type image_code_type;
	efi_memory_type image_data_type;
	void *unload;
};

// 10.2 EFI Device Path Protocol

enum class DevicePathType : uint8_t {
	Hardware = 1,
	ACPI = 2,
	Messaging = 3,
	MediaDevice = 4,
	BiosBootSpecification = 5,
};

constexpr efi_guid EFI_DEVICE_PATH_PROTOCOL_GUID = {
    0x09576E91, 0x6D3F, 0x11D2, {0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}
};

struct efi_device_path_protocol {
	DevicePathType type;
	uint8_t subtype;
	uint8_t length[2];
};

// 10.6.2 Device Path to Text Protocol

constexpr efi_guid EFI_DEVICE_PATH_TO_TEXT_PROTOCOL_GUID = {
    0x8B843E20, 0x8132, 0x4852, {0x90, 0xCC, 0x55, 0x1A, 0x4E, 0x4A, 0x7F, 0x1C}
};

struct efi_device_path_to_text_protocol {
	char16_t *(*convert_device_node_to_text)(
	    const efi_device_path_protocol *device_node, bool display_only, bool allow_shortcuts
	);
	char16_t *(*convert_device_path_to_text)(
	    const efi_device_path_protocol *device_path, bool display_only, bool allow_shortcuts
	);
};

// 12.4.1 EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL

struct efi_simple_text_output_protocol {
	void *reset;
	efi_status (*output_string)(struct efi_simple_text_output_protocol *self, char16_t *string);
	void *test_string;
	void *query_mode;
	void *set_mode;
	void *set_attribute;
	efi_status (*clear_screen)(struct efi_simple_text_output_protocol *self);
	void *set_cursor_position;
	void *enable_cursor;
	void *mode;
};

// 12.9.2 EFI_GRAPHICS_OUTPUT_PROTOCOL

constexpr efi_guid EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID = {
    0x9042a9de, 0x23dc, 0x4a38, {0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a}
};

struct efi_graphics_output_protocol_mode;

struct efi_graphics_output_protocol {
	void *query_mode;
	void *set_mode;
	void *blt;
	struct efi_graphics_output_protocol_mode *mode;
};

struct efi_pixel_bitmask {
	uint32_t red_mask;
	uint32_t green_mask;
	uint32_t blue_mask;
	uint32_t reserved_mask;
};

enum efi_graphics_pixel_format {
	PixelRedGreenBlueReserved8BitPerColor,
	PixelBlueGreenRedReserved8BitPerColor,
	PixelBitMask,
	PixelBltOnly,
	PixelFormatMax
};

struct efi_graphics_output_mode_information {
	uint32_t version;
	uint32_t horizontal_resolution;
	uint32_t vertical_resolution;
	efi_graphics_pixel_format pixel_format;
	efi_pixel_bitmask pixel_information;
	uint32_t pixels_per_scan_line;
};

struct efi_graphics_output_protocol_mode {
	uint32_t max_mode;
	uint32_t mode;
	efi_graphics_output_mode_information *info;
	size_t size_of_info;
	efi_physical_addr framebuffer_base;
	size_t framebuffer_size;
};

// 13.4.1 EFI_SIMPLE_FILE_SYSTEM_PROTOCOL

constexpr efi_guid EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID = {
    0x0964e5b22, 0x6459, 0x11d2, {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}
};

struct efi_file_protocol;

struct efi_simple_file_system_protocol {
	uint64_t revision;
	efi_status (*open_volume)(
	    struct efi_simple_file_system_protocol *self, struct efi_file_protocol **root
	);
};

// 13.5.1 EFI_FILE_PROTOCOL

struct efi_file_protocol {
	uint64_t revision;
	efi_status (*open)(
	    struct efi_file_protocol *self,
	    struct efi_file_protocol **new_handle,
	    char16_t *file_name,
	    uint64_t open_mode,
	    uint64_t attributes
	);
	void *close;
	void *del;
	efi_status (*read)(struct efi_file_protocol *self, size_t *buffer_size, void *buffer);
	void *write;
	efi_status (*get_position)(struct efi_file_protocol *self, uint64_t *position);
	efi_status (*set_position)(struct efi_file_protocol *self, uint64_t position);
	efi_status (*get_info)(
	    struct efi_file_protocol *self,
	    efi_guid *information_type,
	    size_t *buffer_size,
	    void *buffer
	);
	void *set_info;
	void *flush;
	void *open_ex;
	void *read_ex;
	void *write_ex;
	void *flush_ex;
};

// 13.5.2 EFI_FILE_PROTOCOL.Open()

constexpr uint64_t EFI_FILE_MODE_READ = 0x0000000000000001;
constexpr uint64_t EFI_FILE_MODE_WRITE = 0x0000000000000002;
constexpr uint64_t EFI_FILE_MODE_CREATE = 0x8000000000000000;

constexpr uint64_t EFI_FILE_READ_ONLY = 0x0000000000000001;
constexpr uint64_t EFI_FILE_HIDDEN = 0x0000000000000002;
constexpr uint64_t EFI_FILE_SYSTEM = 0x0000000000000004;
constexpr uint64_t EFI_FILE_RESERVED = 0x0000000000000008;
constexpr uint64_t EFI_FILE_DIRECTORY = 0x0000000000000010;
constexpr uint64_t EFI_FILE_ARCHIVE = 0x0000000000000020;
constexpr uint64_t EFI_FILE_VALID_ATTR = 0x0000000000000037;

// 13.5.16 EFI_FILE_INFO

constexpr efi_guid EFI_FILE_INFO_GUID = {
    0x09576e92, 0x6d3f, 0x11d2, {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}
};

struct efi_file_info {
	uint64_t size;
	uint64_t file_size;
	uint64_t physical_size;
	efi_time create_time;
	efi_time last_access_time;
	efi_time modification_time;
	uint64_t attribute;
	char16_t file_name[];
};

// 24.3 PXE Base Code Protocol

constexpr uint64_t EFI_PXE_BASE_CODE_PROTOCOL_REVISION = 0x00010000;

constexpr efi_guid EFI_PXE_BASE_CODE_PROTOCOL_GUID = {
    0x03C4E603, 0xAC28, 0x11d3, {0x9A, 0x2D, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D}
};

struct efi_pxe_base_code_mode;

struct efi_ipv4_address {
	uint8_t addr[4];
};

struct efi_ipv6_address {
	uint8_t addr[16];
};

union efi_ip_address {
	uint32_t addr[4];
	efi_ipv4_address v4;
	efi_ipv6_address v6;
};

struct efi_mac_address {
	uint8_t addr[32];
};

enum efi_pxe_base_code_tftp_opcode {
	EfiPxeBaseCodeTftpFirst,
	EfiPxeBaseCodeTftpGetFileSize,
	EfiPxeBaseCodeTftpReadFile,
	EfiPxeBaseCodeTftpWriteFile,
	EfiPxeBaseCodeTftpReadDirectory,
	EfiPxeBaseCodeMtftpGetFileSize,
	EfiPxeBaseCodeMtftpReadFile,
	EfiPxeBaseCodeMtftpReadDirectory,
	EfiPxeBaseCodeMtftpLast,
};

struct efi_pxe_base_code_mtftp_info {
	efi_ip_address mcast_ip;
	uint16_t c_port;
	uint16_t s_port;
	uint16_t listen_timeout;
	uint16_t transmit_timeout;
};

struct efi_pxe_base_code_protocol {
	uint64_t revision;
	void *start;
	void *stop;
	void *dhcp;
	void *discover;
	efi_status (*mtftp)(
	    efi_pxe_base_code_protocol *self,
	    efi_pxe_base_code_tftp_opcode operation,
	    void *buffer_ptr,
	    bool overwrite,
	    uint64_t *buffer_size,
	    size_t *block_size,
	    efi_ip_address *server_ip,
	    char *filename,
	    efi_pxe_base_code_mtftp_info *info,
	    bool dont_use_buffer
	);
	void *udp_write;
	void *udp_read;
	void *set_ip_filter;
	void *arp;
	void *set_parameters;
	void *set_station_ip;
	void *set_packets;
	efi_pxe_base_code_mode *mode;
};

struct efi_pxe_base_code_dhcpv4_packet {
	uint8_t bootp_opcode;
	uint8_t bootp_hw_type;
	uint8_t bootp_hw_addr_len;
	uint8_t bootp_gate_hops;
	uint32_t bootp_ident;
	uint16_t bootp_seconds;
	uint16_t bootp_flags;
	uint8_t bootp_ci_addr[4];
	uint8_t bootp_yi_addr[4];
	uint8_t bootp_si_addr[4];
	uint8_t bootp_gi_addr[4];
	uint8_t bootp_hw_addr[16];
	uint8_t bootp_srv_name[64];
	uint8_t bootp_boot_file[128];
	uint32_t dhcp_magic;
	uint8_t dhcp_options[56];
};

union efi_pxe_base_code_packet {
	uint8_t raw[1472];
	efi_pxe_base_code_dhcpv4_packet dhcpv4;
	// efi_pxe_base_code_dhcpv6_packet dhcpv6;
};

struct efi_pxe_base_code_icmp_error {
	uint8_t type;
	uint8_t code;
	uint16_t checksum;

	union {
		uint32_t reserved;
		uint32_t mty;
		uint32_t pointer;
		struct {
			uint16_t identifier;
			uint16_t sequence;
		} echo;
	} u;
	uint8_t data[494];
};

struct efi_pxe_base_code_tftp_error {
	uint8_t error_code;
	char error_string[127];
};

struct efi_pxe_base_code_arp_entry {
	efi_ip_address ip_addr;
	efi_mac_address mac_addr;
};

struct efi_pxe_base_code_route_entry {
	efi_ip_address ip_addr;
	efi_ip_address subnet_mask;
	efi_ip_address gw_addr;
};

constexpr size_t EFI_PXE_BASE_CODE_MAX_ARP_ENTRIES = 8;
constexpr size_t EFI_PXE_BASE_CODE_MAX_ROUTE_ENTRIES = 8;

struct efi_pxe_base_code_mode {
	bool started;
	bool ipv6_available;
	bool ipv6_supported;
	bool using_ipv6;
	bool bis_supported;
	bool bis_detected;
	bool auto_arp;
	bool send_guid;
	bool dhcp_discover_valid;
	bool dhcp_ack_received;
	bool proxy_offer_received;
	bool pxe_discover_valid;
	bool pxe_reply_received;
	bool pxe_bis_reply_received;
	bool icmp_error_received;
	bool tftp_error_received;
	bool make_callbacks;
	uint8_t ttl;
	uint8_t tos;
	efi_ip_address station_ip;
	efi_ip_address subnet_mask;
	efi_pxe_base_code_packet dhcp_discover;
	efi_pxe_base_code_packet dhcp_ack;
	efi_pxe_base_code_packet proxy_offer;
	efi_pxe_base_code_packet pxe_discover;
	efi_pxe_base_code_packet pxe_reply;
	efi_pxe_base_code_packet pxe_bis_reply;
	efi_pxe_base_code_packet ip_filter;
	uint32_t arp_cache_entries;
	efi_pxe_base_code_arp_entry arp_cache[EFI_PXE_BASE_CODE_MAX_ARP_ENTRIES];
	uint32_t route_table_entries;
	efi_pxe_base_code_route_entry route_table[EFI_PXE_BASE_CODE_MAX_ROUTE_ENTRIES];
	efi_pxe_base_code_icmp_error icmp_error;
	efi_pxe_base_code_tftp_error tftp_error;
};

// 14.1 EFI Service Binding Protocol
struct efi_service_binding_protocol {
	efi_status (*create_child)(struct efi_service_binding_protocol *self, efi_handle *child_handle);
	efi_status (*destroy_child)(struct efi_service_binding_protocol *self, efi_handle child_handle);
};

// 26.3 EFI HTTP Service Binding Protocol
constexpr efi_guid EFI_HTTP_SERVICE_BINDING_PROTOCOL_GUID = {
    0xbdc8e6af, 0xd9bc, 0x4379, {0xa7, 0x2a, 0xe0, 0xc4, 0xe7, 0x5d, 0xae, 0x1c}
};

using efi_http_service_binding_protocol = efi_service_binding_protocol;

// 26 EFI Hypertext Transfer Protocol

constexpr efi_guid EFI_HTTP_PROTOCOL_GUID = {
    0x7A59B29B, 0x910B, 0x4171, {0x82, 0x42, 0xA8, 0x5A, 0x0D, 0xF2, 0x5B, 0x5B}
};

enum efi_http_method {
	HttpMethodGet,
	HttpMethodPost,
	HttpMethodPatch,
	HttpMethodOptions,
	HttpMethodConnect,
	HttpMethodHead,
	HttpMethodPut,
	HttpMethodDelete,
	HttpMethodTrace,
	HttpMethodMax
};

enum efi_http_status_code {
	HTTP_STATUS_UNSUPPORTED_STATUS = 0,
	HTTP_STATUS_100_CONTINUE,
	HTTP_STATUS_101_SWITCHING_PROTOCOLS,
	HTTP_STATUS_200_OK,
	HTTP_STATUS_201_CREATED,
	HTTP_STATUS_202_ACCEPTED,
	HTTP_STATUS_203_NON_AUTHORITATIVE_INFORMATION,
	HTTP_STATUS_204_NO_CONTENT,
	HTTP_STATUS_205_RESET_CONTENT,
	HTTP_STATUS_206_PARTIAL_CONTENT,
	HTTP_STATUS_300_MULTIPLE_CHOICES,
	HTTP_STATUS_301_MOVED_PERMANENTLY,
	HTTP_STATUS_302_FOUND,
	HTTP_STATUS_303_SEE_OTHER,
	HTTP_STATUS_304_NOT_MODIFIED,
	HTTP_STATUS_305_USE_PROXY,
	HTTP_STATUS_307_TEMPORARY_REDIRECT,
	HTTP_STATUS_400_BAD_REQUEST,
	HTTP_STATUS_401_UNAUTHORIZED,
	HTTP_STATUS_402_PAYMENT_REQUIRED,
	HTTP_STATUS_403_FORBIDDEN,
	HTTP_STATUS_404_NOT_FOUND,
	HTTP_STATUS_405_METHOD_NOT_ALLOWED,
	HTTP_STATUS_406_NOT_ACCEPTABLE,
	HTTP_STATUS_407_PROXY_AUTHENTICATION_REQUIRED,
	HTTP_STATUS_408_REQUEST_TIME_OUT,
	HTTP_STATUS_409_CONFLICT,
	HTTP_STATUS_410_GONE,
	HTTP_STATUS_411_LENGTH_REQUIRED,
	HTTP_STATUS_412_PRECONDITION_FAILED,
	HTTP_STATUS_413_REQUEST_ENTITY_TOO_LARGE,
	HTTP_STATUS_414_REQUEST_URI_TOO_LARGE,
	HTTP_STATUS_415_UNSUPPORTED_MEDIA_TYPE,
	HTTP_STATUS_416_REQUESTED_RANGE_NOT_SATISFIED,
	HTTP_STATUS_417_EXPECTATION_FAILED,
	HTTP_STATUS_500_INTERNAL_SERVER_ERROR,
	HTTP_STATUS_501_NOT_IMPLEMENTED,
	HTTP_STATUS_502_BAD_GATEWAY,
	HTTP_STATUS_503_SERVICE_UNAVAILABLE,
	HTTP_STATUS_504_GATEWAY_TIME_OUT,
	HTTP_STATUS_505_HTTP_VERSION_NOT_SUPPORTED,
	HTTP_STATUS_308_PERMANENT_REDIRECT
};

struct efi_http_header {
	char *field_name;
	char *field_value;
};

struct efi_http_request_data {
	efi_http_method method;
	char16_t *url;
};

struct efi_http_response_data {
	efi_http_status_code status_code;
};

struct efi_http_message {
	union {
		efi_http_request_data *request;
		efi_http_response_data *response;
	};
	size_t header_count;
	efi_http_header *headers;
	size_t body_length;
	void *body;
};

enum efi_http_version { HttpVersion10, HttpVersion11, HttpVersionUnsupported };

struct efi_httpv4_access_point {
	bool use_default_address;
	efi_ipv4_address local_address;
	efi_ipv4_address local_subnet;
	uint16_t local_port;
};

struct efi_httpv6_access_point {
	efi_ipv6_address local_address;
	uint16_t local_port;
};

struct efi_http_config_data {
	efi_http_version http_version;
	uint32_t timeout_millisec;
	bool local_address_is_ipv6;
	union {
		efi_httpv4_access_point *ipv4_node;
		efi_httpv6_access_point *ipv6_node;
	};
};

struct efi_http_mode_data {
	efi_http_config_data config_data;
	uint16_t supported_versions;
};

struct efi_http_token {
	efi_event event;
	efi_status status;
	efi_http_message *message;
};

struct efi_http_protocol;

typedef efi_status (*efi_http_get_mode_data_t)(
    struct efi_http_protocol *self, struct efi_http_mode_data *http_mode_data
);

typedef efi_status (*efi_http_configure_t)(
    struct efi_http_protocol *self, struct efi_http_config_data *http_config_data
);

typedef efi_status (*efi_http_request_t)(
    struct efi_http_protocol *self, struct efi_http_token *token
);

typedef efi_status (*efi_http_cancel_t)(
    struct efi_http_protocol *self, struct efi_http_token *token
);

typedef efi_status (*efi_http_response_t)(
    struct efi_http_protocol *self, struct efi_http_token *token
);

typedef efi_status (*efi_http_poll_t)(struct efi_http_protocol *self);

struct efi_http_protocol {
	efi_http_get_mode_data_t get_mode_data;
	efi_http_configure_t configure;
	efi_http_request_t request;
	efi_http_cancel_t cancel;
	efi_http_response_t response;
	efi_http_poll_t poll;
};

// 7.3 Protocol Handler Services
constexpr uint32_t EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL = 0x00000001;
constexpr uint32_t EFI_OPEN_PROTOCOL_GET_PROTOCOL = 0x00000002;
constexpr uint32_t EFI_OPEN_PROTOCOL_TEST_PROTOCOL = 0x00000004;
constexpr uint32_t EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER = 0x00000008;
constexpr uint32_t EFI_OPEN_PROTOCOL_BY_DRIVER = 0x00000010;
constexpr uint32_t EFI_OPEN_PROTOCOL_EXCLUSIVE = 0x00000020;

// Appendix D

constexpr efi_status EFI_SUCCESS = 0;

constexpr efi_status EFI_WARN_UNKNOWN_GLYPH = 1;
constexpr efi_status EFI_WARN_DELETE_FAILURE = 2;
constexpr efi_status EFI_WARN_WRITE_FAILURE = 3;
constexpr efi_status EFI_WARN_BUFFER_TOO_SMALL = 4;
constexpr efi_status EFI_WARN_STALE_DATA = 5;
constexpr efi_status EFI_WARN_FILE_SYSTEM = 6;
constexpr efi_status EFI_WARN_RESET_REQUIRED = 7;

constexpr efi_status EFI_LOAD_ERROR = (INTPTR_MAX + 1ULL) + 1;
constexpr efi_status EFI_INVALID_PARAMETER = (INTPTR_MAX + 1ULL) + 2;
constexpr efi_status EFI_UNSUPPORTED = (INTPTR_MAX + 1ULL) + 3;
constexpr efi_status EFI_BAD_BUFFER_SIZE = (INTPTR_MAX + 1ULL) + 4;
constexpr efi_status EFI_BUFFER_TOO_SMALL = (INTPTR_MAX + 1ULL) + 5;
constexpr efi_status EFI_NOT_READY = (INTPTR_MAX + 1ULL) + 6;
constexpr efi_status EFI_DEVICE_ERROR = (INTPTR_MAX + 1ULL) + 7;
constexpr efi_status EFI_WRITE_PROTECTED = (INTPTR_MAX + 1ULL) + 8;
constexpr efi_status EFI_OUT_OF_RESOURCES = (INTPTR_MAX + 1ULL) + 9;
constexpr efi_status EFI_VOLUME_CORRUPTED = (INTPTR_MAX + 1ULL) + 10;
constexpr efi_status EFI_VOLUME_FULL = (INTPTR_MAX + 1ULL) + 11;
constexpr efi_status EFI_NO_MEDIA = (INTPTR_MAX + 1ULL) + 12;
constexpr efi_status EFI_MEDIA_CHANGED = (INTPTR_MAX + 1ULL) + 13;
constexpr efi_status EFI_NOT_FOUND = (INTPTR_MAX + 1ULL) + 14;
constexpr efi_status EFI_ACCESS_DENIED = (INTPTR_MAX + 1ULL) + 15;
constexpr efi_status EFI_NO_RESPONSE = (INTPTR_MAX + 1ULL) + 16;
constexpr efi_status EFI_NO_MAPPING = (INTPTR_MAX + 1ULL) + 17;
constexpr efi_status EFI_TIMEOUT = (INTPTR_MAX + 1ULL) + 18;
constexpr efi_status EFI_NOT_STARTED = (INTPTR_MAX + 1ULL) + 19;
constexpr efi_status EFI_ALREADY_STARTED = (INTPTR_MAX + 1ULL) + 20;
constexpr efi_status EFI_ABORTED = (INTPTR_MAX + 1ULL) + 21;
constexpr efi_status EFI_ICMP_ERROR = (INTPTR_MAX + 1ULL) + 22;
constexpr efi_status EFI_TFTP_ERROR = (INTPTR_MAX + 1ULL) + 23;
constexpr efi_status EFI_PROTOCOL_ERROR = (INTPTR_MAX + 1ULL) + 24;
constexpr efi_status EFI_INCOMPATIBLE_VERSION = (INTPTR_MAX + 1ULL) + 25;
constexpr efi_status EFI_SECURITY_VIOLATION = (INTPTR_MAX + 1ULL) + 26;
constexpr efi_status EFI_CRC_ERROR = (INTPTR_MAX + 1ULL) + 27;
constexpr efi_status EFI_END_OF_MEDIA = (INTPTR_MAX + 1ULL) + 28;
constexpr efi_status EFI_END_OF_FILE = (INTPTR_MAX + 1ULL) + 31;
constexpr efi_status EFI_INVALID_LANGUAGE = (INTPTR_MAX + 1ULL) + 32;
constexpr efi_status EFI_COMPROMISED_DATA = (INTPTR_MAX + 1ULL) + 33;
constexpr efi_status EFI_IP_ADDRESS_CONFLICT = (INTPTR_MAX + 1ULL) + 34;
constexpr efi_status EFI_HTTP_ERROR = (INTPTR_MAX + 1ULL) + 35;

// Related Documents: RISC-V EFI Boot Protocol

constexpr efi_guid RISCV_EFI_BOOT_PROTOCOL_GUID = {
    0xccd15fec, 0x6f73, 0x4eec, {0x83, 0x95, 0x3e, 0x69, 0xe4, 0xb9, 0x40, 0xbf}
};

struct riscv_efi_boot_protocol {
	uint64_t revision;
	efi_status (*get_boot_hartid)(riscv_efi_boot_protocol *self, size_t *boot_hart_id);
};

// ------------------------------------------------------------------------------------------------
// Some things heavily rely on declarations later in the spec, so place them last.
// ------------------------------------------------------------------------------------------------

// 4.4.1 EFI_BOOT_SERVICES

constexpr uint32_t EVT_NOTIFY_SIGNAL = 0x200;

constexpr efi_tpl TPL_APPLICATION = 4;
constexpr efi_tpl TPL_CALLBACK = 8;
constexpr efi_tpl TPL_NOTIFY = 16;
constexpr efi_tpl TPL_HIGH_LEVEL = 31;

typedef void (*efi_event_notify)(efi_event, void *context);

struct efi_boot_services {
	efi_table_header hdr;
	void *raise_tpl;
	void *restore_tpl;
	efi_status (*allocate_pages)(
	    efi_allocate_type type, efi_memory_type memory_type, size_t pages, efi_physical_addr *memory
	);
	void *free_pages;
	efi_status (*get_memory_map)(
	    size_t *memory_map_size,
	    efi_memory_descriptor *memory_map,
	    size_t *map_key,
	    size_t *descriptor_size,
	    uint32_t *descriptor_version
	);
	efi_status (*allocate_pool)(efi_memory_type pool_type, size_t size, void **buffer);
	efi_status (*free_pool)(void *buffer);
	efi_status (*create_event)(
	    uint32_t type,
	    efi_tpl notify_tpl,
	    efi_event_notify notify_function,
	    void *notify_context,
	    efi_event *event
	);
	void *set_timer;
	efi_status (*wait_for_event)(size_t number_of_events, efi_event *event, size_t *index);
	void *signal_event;
	efi_status (*close_event)(efi_event event);
	void *check_event;
	void *install_protocol_interface;
	void *reinstall_protocol_interface;
	void *uninstall_protocol_interface;
	efi_status (*handle_protocol)(efi_handle handle, efi_guid *protocol, void **interface);
	void *reserved;
	void *register_protocol_notify;
	void *locate_handle;
	void *locate_device_path;
	void *install_configuration_table;
	void *load_image;
	void *start_image;
	void *exit;
	void *unload_image;
	efi_status (*exit_boot_services)(efi_handle image_handle, size_t map_key);
	void *get_next_monotonic_count;
	efi_status (*stall)(size_t microseconds);
	efi_status (*set_watchdog_timer)(
	    size_t timeout, uint64_t watchdog_code, size_t data_size, char16_t *watchdog_data
	);
	void *connect_controller;
	void *disconnect_controller;
	efi_status (*open_protocol)(
	    efi_handle handle,
	    efi_guid *protocol,
	    void **interface,
	    efi_handle agent_handle,
	    efi_handle controller_handle,
	    uint32_t attributes
	);
	efi_status (*close_protocol)(
	    efi_handle handle, efi_guid *protocol, efi_handle agent_handle, efi_handle controller_handle
	);
	void *open_protocol_information;
	void *protocols_per_handle;
	void *locate_handle_buffer;
	efi_status (*locate_protocol)(efi_guid *protocol, void *registration, void **interface);
	void *install_multiple_protocol_interface;
	void *uninstall_multiple_protocol_interface;
	void *calculate_crc32;
	void *copy_mem;
	void *set_mem;
	void *create_event_ex;
};
