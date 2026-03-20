#include "tusb.h"

// -- Device Descriptor --
// Vendor ID and Product ID. Using the TinyUSB defaults here, 
// but you should change them if you have your own.
tusb_desc_device_t const desc_device =
{
	.bLength            = sizeof(tusb_desc_device_t),
	.bDescriptorType    = TUSB_DESC_DEVICE,
	.bcdUSB             = 0x0200,

	// Vendor Specific Class (0xFF)
	.bDeviceClass       = TUSB_CLASS_VENDOR_SPECIFIC,
	.bDeviceSubClass    = 0x00,
	.bDeviceProtocol    = 0x00,

	.bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,

	.idVendor           = 0xCafe, 
	.idProduct          = 0x4000,
	.bcdDevice          = 0x0100,

	.iManufacturer      = 0x01,
	.iProduct           = 0x02,
	.iSerialNumber      = 0x03,

	.bNumConfigurations = 0x01
};

// Invoked when GET DEVICE DESCRIPTOR is received
uint8_t const * tud_descriptor_device_cb(void) {
	return (uint8_t const *) &desc_device;
}

// -- Configuration Descriptor --
enum {
	ITF_NUM_VENDOR = 0,
	ITF_NUM_TOTAL
};

// Endpoint addresses
// 0x01 is OUT from Host to Device
// 0x81 is IN from Device to Host (Your IQ Data)
#define EPNUM_VENDOR_OUT  0x01
#define EPNUM_VENDOR_IN   0x81

// Total length of the configuration descriptor payload
#define CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_VENDOR_DESC_LEN)

// Helper macro to build the Vendor descriptor
#define TUD_VENDOR_DESCRIPTOR(_itfnum, _stridx, _epout, _epin, _epsize) \
  /* Interface Descriptor */\
  9, TUSB_DESC_INTERFACE, _itfnum, 0, 2, TUSB_CLASS_VENDOR_SPECIFIC, 0x00, 0x00, _stridx,\
  /* Endpoint Out Descriptor */\
  7, TUSB_DESC_ENDPOINT, _epout, TUSB_XFER_BULK, U16_TO_U8S_LE(_epsize), 0,\
  /* Endpoint In Descriptor */\
  7, TUSB_DESC_ENDPOINT, _epin, TUSB_XFER_BULK, U16_TO_U8S_LE(_epsize), 0

uint8_t const desc_configuration[] =
{
	// Config number, interface count, string index, total length, attribute, power in mA
	TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),

	// Vendor Interface Descriptor
	// Interface number, string index, EP Out & EP In address, EP size
	TUD_VENDOR_DESCRIPTOR(ITF_NUM_VENDOR, 0, EPNUM_VENDOR_OUT, EPNUM_VENDOR_IN, CFG_TUD_VENDOR_EPSIZE)
};

// Invoked when GET CONFIGURATION DESCRIPTOR is received
uint8_t const * tud_descriptor_configuration_cb(uint8_t index) {
	(void) index; // Multiple configurations not used
	return desc_configuration;
}

// -- String Descriptors --
// Array of pointer to string descriptors
char const* string_desc_arr [] =
{
	(const char[]) { 0x09, 0x04 }, // 0: is supported language is English (0x0409)
	"iraciemsgter",                // 1: Manufacturer
	"nRF52 IQ Capture Bulk",       // 2: Product
	"1"                            // 3: Serial Number (Can be dynamically replaced with nRF FICR)
};

static uint16_t _desc_str[32];

// Invoked when GET STRING DESCRIPTOR request is received
uint16_t const* tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
	(void) langid;
	uint8_t chr_count;

	if ( index == 0 ) {
		memcpy(&_desc_str[1], string_desc_arr[0], 2);
		chr_count = 1;
	} else {
		// Note: the 0xEE index string is a Microsoft OS 1.0 Descriptors.
		// https://docs.microsoft.com/en-us/windows-hardware/drivers/usbcon/microsoft-defined-usb-descriptors
		if ( !(index < sizeof(string_desc_arr)/sizeof(string_desc_arr[0])) ) return NULL;

		const char* str = string_desc_arr[index];
		chr_count = strlen(str);

		// Cap at max string length
		if ( chr_count > 31 ) chr_count = 31;

		// Convert ASCII string into UTF-16
		for(uint8_t i=0; i<chr_count; i++) {
			_desc_str[1+i] = str[i];
		}
	}

	// First byte is length (including header), second byte is string type
	_desc_str[0] = (TUSB_DESC_STRING << 8 ) | (2*chr_count + 2);

	return _desc_str;
}
