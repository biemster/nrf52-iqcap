#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
 extern "C" {
#endif

//--------------------------------------------------------------------
// RHPort (Root Hub Port) Configuration
//--------------------------------------------------------------------
// For nRF52840, we use the onboard USB peripheral (0)
// OPT_MODE_DEVICE means this board acts as a USB device (not a host)
#define CFG_TUSB_RHPORT0_MODE   OPT_MODE_DEVICE

// If you are NOT using a second USB controller, 
// you can leave RHPORT1 undefined or set to 0.

// -- Board & OS Configuration --
#define CFG_TUSB_MCU               OPT_MCU_NRF5X
#define CFG_TUSB_OS                OPT_OS_NONE
#define CFG_TUD_ENABLED            1

// nRF52840 USB peripheral uses EasyDMA. 
// TinyUSB handles this gracefully, but memory needs to be aligned.
#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN         __attribute__ ((aligned(4)))

// -- Device Configuration --
#define CFG_TUD_ENDPOINT0_SIZE     64

// -- Class Configuration --
// Disable standard classes
#define CFG_TUD_CDC                0
#define CFG_TUD_MSC                0
#define CFG_TUD_HID                0
#define CFG_TUD_MIDI               0
#define CFG_TUD_AUDIO              0

// Enable Vendor Class
#define CFG_TUD_VENDOR             1

// -- Vendor Class Specifics --
// 64 bytes is the maximum packet size for Full Speed (12Mbps) Bulk endpoints
#define CFG_TUD_VENDOR_EPSIZE      64

// Buffer sizes for TinyUSB internal FIFOs.
// Because you are pushing large IQ arrays, a larger TX buffer helps 
// queue data efficiently without dropping samples.
#define CFG_TUD_VENDOR_RX_BUFSIZE  64
#define CFG_TUD_VENDOR_TX_BUFSIZE  64

#ifdef __cplusplus
 }
#endif

#endif /* _TUSB_CONFIG_H_ */
