#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <nrfx_clock.h>
#include <nrfx_uarte.h>
#include <sys/stat.h>

#define LED (1 << 15) // the blue led on the nice!nano

/* Dummy implementations to satisfy the linker and silence warnings */
int _close(int file) { return -1; }
int _fstat(int file, struct stat *st) { return -1; }
int _isatty(int file) { return 1; }
int _lseek(int file, int ptr, int dir) { return 0; }
int _read(int file, char *ptr, int len) { return 0; }
int _kill(int pid, int sig) { return -1; }
int _getpid(void) { return 1; }

// uart config, note that Nordic's UARTE DMA does not
// support writing from flash
nrfx_uarte_t uart = {
	.p_reg = NRF_UARTE0,
	.drv_inst_idx = NRFX_UARTE0_INST_IDX,
};
const nrfx_uarte_config_t uart_config = {
	.pseltxd = 6,
	.pselrxd = 8,
	.pselcts = NRF_UARTE_PSEL_DISCONNECTED,
	.pselrts = NRF_UARTE_PSEL_DISCONNECTED,
	.p_context = NULL,
	.baudrate = NRF_UARTE_BAUDRATE_115200,
	.interrupt_priority = NRFX_UARTE_DEFAULT_CONFIG_IRQ_PRIORITY,
	.hal_cfg = {
		.hwfc = NRF_UARTE_HWFC_DISABLED,
		.parity = NRF_UARTE_PARITY_EXCLUDED,
		NRFX_UARTE_DEFAULT_EXTENDED_STOP_CONFIG
		NRFX_UARTE_DEFAULT_EXTENDED_PARITYTYPE_CONFIG
	}
};

// gcc stdlib write hook
int _write(int handle, char *buffer, int size) {
	// stdout or stderr only
	assert(handle == 1 || handle == 2);

	int i = 0;
	while (true) {
		char *nl = memchr(&buffer[i], '\n', size-i);
		int span = nl ? nl-&buffer[i] : size-i;
		nrfx_err_t err = nrfx_uarte_tx(&uart, (uint8_t*)&buffer[i], span);
		assert(err == NRFX_SUCCESS);
		i += span;

		if (i >= size) {
			return size;
		}

		char r[2] = "\r\n";
		err = nrfx_uarte_tx(&uart, (uint8_t*)r, sizeof(r));
		assert(err == NRFX_SUCCESS);
		i += 1;
	}
}

// hook for clock init callback.
void clock_handler(nrfx_clock_evt_type_t event) {}

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

	// setup LCLK
	nrfx_err_t err = nrfx_clock_init(clock_handler);
	assert(err == NRFX_SUCCESS);
	nrfx_clock_start(NRF_CLOCK_DOMAIN_LFCLK);

	// blink fast
	NRF_P0->DIRSET = LED;
	NRF_P0->OUTCLR = LED;
	blink(100);

	// go back to uf2 bootloader
	NRF_POWER->GPREGRET = 0x57; // 0x57 tells the Adafruit UF2 bootloader to stay in bootloader mode
	NVIC_SystemReset();         // Reboot the chip
}
