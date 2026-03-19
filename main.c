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

void delay_ms(uint32_t ms) {
	uint32_t count = ms * (64000000 / 1000 / 4);
	for (volatile uint32_t i = 0; i < count; i++) {
		__NOP(); // No-operation instruction to prevent compiler from optimizing the loop away
	}
}

void blink(int n) {
	for(int i = n-1; i >= 0; i--) {
		NRF_P0->OUT |= LED; // ON
		delay_ms(33);
		NRF_P0->OUT &= ~LED; // OFF
		if(i) delay_ms(33);
	}
}

// entry point
void main(void) {
	// Relocate the interrupt vector table to the app start address for UF2
	SCB->VTOR = 0x26000;

	// Clocks
	nrfx_err_t err = nrfx_clock_init(clock_handler);
	assert(err == NRFX_SUCCESS);
	nrfx_clock_start(NRF_CLOCK_DOMAIN_LFCLK); // Start LFCLK (required for USB Power management)
	nrfx_clock_start(NRF_CLOCK_DOMAIN_HFCLK); // Start HFCLK (required for USB peripheral and RADIO)

	// Power	
	const nrfx_power_config_t pwr_cfg = {0};
	nrfx_power_init(&pwr_cfg);

	// LED
	NRF_P0->DIRSET = LED;
	NRF_P0->OUTCLR = LED;

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

	while(1) {
		tud_task();
	}
}
