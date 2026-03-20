#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <nrfx_clock.h>
#include <nrfx_power.h>
#include <tusb.h>
#include <sys/stat.h>

#define LED (1 << 15) // the blue led on the nice!nano

/* Dummy implementations to satisfy the linker and silence warnings */
int _write(int handle, char *buffer, int size) { return size; }
int _close(int file) { return -1; }
int _fstat(int file, struct stat *st) { return -1; }
int _isatty(int file) { return 1; }
int _lseek(int file, int ptr, int dir) { return 0; }
int _read(int file, char *ptr, int len) { return 0; }
int _kill(int pid, int sig) { return -1; }
int _getpid(void) { return 1; }

extern void tusb_hal_nrf_power_event(uint32_t event);

// Value is chosen to be as same as NRFX_POWER_USB_EVT_* in nrfx_power.h
enum {
	USB_EVT_DETECTED = 0,
	USB_EVT_REMOVED = 1,
	USB_EVT_READY = 2
};

void USBD_IRQHandler(void) {
	tud_int_handler(0);
}

void POWER_CLOCK_IRQHandler(void) {
	nrfx_power_irq_handler();
}

void delay_init(void) {
	// Enable the Trace and Debug block
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->CYCCNT = 0; // Clear the cycle counter
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk; // Enable the cycle counter
}

void delay_ms(uint32_t ms) {
	uint32_t start = DWT->CYCCNT;

	// nRF52840 runs at 64 MHz. Therefore, 64,000 cycles = 1 millisecond.
	uint32_t delay_ticks = ms * 64000;
	while ((DWT->CYCCNT - start) < delay_ticks) {
		__NOP();
	}
}

static void power_event_handler(nrfx_power_usb_evt_t event) {
	tusb_hal_nrf_power_event((uint32_t) event);
}

// hook for clock init callback.
void clock_handler(nrfx_clock_evt_type_t event) {}

void jump_bootloader() {
	// go back to uf2 bootloader
	NRF_POWER->GPREGRET = 0x57; // 0x57 tells the Adafruit UF2 bootloader to stay in bootloader mode
	NVIC_SystemReset();         // Reboot the chip
}

void blink(int n) {
	for(int i = n-1; i >= 0; i--) {
		NRF_P0->OUT |= LED; // ON
		delay_ms(33);
		NRF_P0->OUT &= ~LED; // OFF
		if(i) delay_ms(33);
	}
}

void tud_vendor_rx_cb(uint8_t intf, const uint8_t *buffer, uint32_t bufsize) {
	// - CFG_TUD_VENDOR_TXRX_BUFFERED = 1: buffer and bufsize must not be used (both NULL,0) since data is in RX FIFO
	uint8_t buf[64]; // Buffer to hold the incoming command
	uint32_t count = tud_vendor_available();

	if (count > 0) {
		uint32_t bytes_read = tud_vendor_read(buf, sizeof(buf));
		if (bytes_read == 4 && buf[0] == 0xa2) {
			blink(3);
			jump_bootloader();
		}
	}
}

// entry point
void main(void) {
	// Relocate the interrupt vector table to the app start address for UF2
	SCB->VTOR = 0x26000;

	// Start LFCLK (Low Frequency Clock)
	NRF_CLOCK->EVENTS_LFCLKSTARTED = 0;
	NRF_CLOCK->TASKS_LFCLKSTART = 1;
	while (NRF_CLOCK->EVENTS_LFCLKSTARTED == 0) {
		// Wait for LFCLK to start
	}

	// Start HFCLK (High Frequency 32MHz Crystal)
	NRF_CLOCK->EVENTS_HFCLKSTARTED = 0;
	NRF_CLOCK->TASKS_HFCLKSTART = 1;
	while (NRF_CLOCK->EVENTS_HFCLKSTARTED == 0) {
		// Wait for HFCLK to stabilize
	}

	// Power	
	const nrfx_power_config_t pwr_cfg = {0};
	nrfx_power_init(&pwr_cfg);
	NVIC_EnableIRQ(POWER_CLOCK_IRQn);

	// Register tusb function as USB power handler
	// cause cast-function-type warning
	const nrfx_power_usbevt_config_t config = {.handler = power_event_handler};
	nrfx_power_usbevt_init(&config);
	nrfx_power_usbevt_enable();

	// USB power may already be ready at this time -> no event generated
	// We need to invoke the handler based on the status initially
	uint32_t usb_reg = NRF_POWER->USBREGSTATUS;
	if ( usb_reg & POWER_USBREGSTATUS_VBUSDETECT_Msk ) {
		tusb_hal_nrf_power_event(USB_EVT_DETECTED);
	}
	if ( usb_reg & POWER_USBREGSTATUS_OUTPUTRDY_Msk  ) {
		tusb_hal_nrf_power_event(USB_EVT_READY);
	}

	// Init TinyUSB
	tusb_init();
	NVIC_EnableIRQ(USBD_IRQn);

	delay_init();

	// LED
	NRF_P0->DIRSET = LED;
	NRF_P0->OUTCLR = LED;

	blink(3);
	while(1) {
		tud_task();
	}
}
