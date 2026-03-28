#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <sys/stat.h>

#include "nrf.h"
#include "rfx.h"
#include "tusb.h"

#define NRF_CMD_USBTEST         0xa1
#define NRF_CMD_REBOOT          0xa2
#define NRF_CMD_IQCAPTURE_TRIG  0xca
#define NRF_CMD_IQCAPTURE_NOW   0xcb
#define NRF_CMD_RADIO_STOP      0xcf
#define NRF_CMD_PEEK32          0xd1
#define NRF_CMD_POKE32          0xd2

#define TRIG_TIMER              NRF_TIMER2
#define TRIG_TIMER_IRQn         TIMER2_IRQn
#define TRIG_GPIOTE_CHAN        2
#define TRIG_PPI_CHAN           2

#define LED     (1 << 15) // the red led on the nice!nano
#define MAXSAMP (16*1024)
__attribute__((aligned(8192))) static uint32_t iq_buf[MAXSAMP];

static volatile int gs_usb_cmd;
static volatile uint8_t gs_usb_buf[64];

void main_loop(void);

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

void HardFault_Handler(void) {
    NRF_P0->OUTSET = LED; // Force LED on to show we crashed
    while(1) { __NOP(); }
}

void TIMER2_IRQHandler(void) {
    // Trigger IQ capture: inlined radio_trigger_iq_capture()
    RADIO_REG(TASKS_IQCAP) = 1;

    // Reset the timer
    NRF_TIMER2->EVENTS_COMPARE[0] = 0;
    // Disable the PPI channel until the next trigger
    NRF_PPI->CHENCLR = (1 << TRIG_PPI_CHAN);
    // Turn off the LED when triggered
    NRF_P0->OUTCLR = LED;
}

void USBD_IRQHandler(void) {
    tud_int_handler(0);
}

void POWER_CLOCK_IRQHandler(void) {
    uint32_t inten = NRF_POWER->INTENSET;

    // Cable plugged in
    if ((inten & POWER_INTENSET_USBDETECTED_Msk) && NRF_POWER->EVENTS_USBDETECTED) {
        NRF_POWER->EVENTS_USBDETECTED = 0;
        tusb_hal_nrf_power_event(USB_EVT_DETECTED);
    }

    // Cable unplugged
    if ((inten & POWER_INTENSET_USBREMOVED_Msk) && NRF_POWER->EVENTS_USBREMOVED) {
        NRF_POWER->EVENTS_USBREMOVED = 0;
        tusb_hal_nrf_power_event(USB_EVT_REMOVED);
    }

    // Power ready to use
    if ((inten & POWER_INTENSET_USBPWRRDY_Msk) && NRF_POWER->EVENTS_USBPWRRDY) {
        NRF_POWER->EVENTS_USBPWRRDY = 0;
        tusb_hal_nrf_power_event(USB_EVT_READY);
    }
}

void init_usb_power_irq(void) {
    // Enable events for USB insertion, removal, and power ready
    NRF_POWER->INTENSET = (POWER_INTENSET_USBDETECTED_Msk |
                           POWER_INTENSET_USBREMOVED_Msk  |
                           POWER_INTENSET_USBPWRRDY_Msk);

    // Enable the POWER_CLOCK IRQ in the NVIC
    NVIC_EnableIRQ(POWER_CLOCK_IRQn);

    // USB power may already be ready at this time -> no event generated
    // We need to invoke the handler based on the status initially
    uint32_t usb_reg = NRF_POWER->USBREGSTATUS;
    if ( usb_reg & POWER_USBREGSTATUS_VBUSDETECT_Msk ) {
        tusb_hal_nrf_power_event(USB_EVT_DETECTED);
    }
    if ( usb_reg & POWER_USBREGSTATUS_OUTPUTRDY_Msk  ) {
        tusb_hal_nrf_power_event(USB_EVT_READY);
    }
}

void clock_init() {
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
}

void delay_init(void) {
    // Enable the Trace and Debug block
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0; // Clear the cycle counter
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk; // Enable the cycle counter
}

void delay_us(uint32_t us) {
    uint32_t start = DWT->CYCCNT;

    // nRF52840 runs at 64 MHz. Therefore, 64 cycles = 1 microsecond.
    uint32_t delay_ticks = us * 64;
    while ((DWT->CYCCNT - start) < delay_ticks) {
        __NOP();
    }
}
void delay_ms(uint32_t ms) { delay_us(ms *1000); }

void timer2_init(void) {
    // Configure TIMER2 as a 32 bit one-shot timer at 16 MHz
    // Counts from 0 to CC[0], then fires the IRQ, resets to 0, and stops
    TRIG_TIMER->MODE = TIMER_MODE_MODE_Timer;
    TRIG_TIMER->PRESCALER = 1; // 16 MHz / 1
    TRIG_TIMER->BITMODE = TIMER_BITMODE_BITMODE_32Bit << TIMER_BITMODE_BITMODE_Pos;
    TRIG_TIMER->TASKS_STOP = 1;
    TRIG_TIMER->TASKS_CLEAR = 1;
    // One-shot mode: stop the timer when it reaches the compare value
    TRIG_TIMER->SHORTS = TIMER_SHORTS_COMPARE0_STOP_Msk | TIMER_SHORTS_COMPARE0_CLEAR_Msk;
    TRIG_TIMER->INTENSET = TIMER_INTENSET_COMPARE0_Msk;

    NVIC_EnableIRQ(TRIG_TIMER_IRQn);
}

void init_trigger(int port, int pin, bool active_high) {
    // Configure pin as input with pull-up/down as needed
    (port ? NRF_P1 : NRF_P0)->PIN_CNF[pin] =
        (GPIO_PIN_CNF_DIR_Input << GPIO_PIN_CNF_DIR_Pos) |
        ((active_high ? GPIO_PIN_CNF_PULL_Pulldown : GPIO_PIN_CNF_PULL_Pullup) << GPIO_PIN_CNF_PULL_Pos);

    // Configure GPIOTE channel 0 to generate an event on a rising edge on the specified pin
    NRF_GPIOTE->CONFIG[TRIG_GPIOTE_CHAN] = (GPIOTE_CONFIG_MODE_Event << GPIOTE_CONFIG_MODE_Pos) |
                            (port << GPIOTE_CONFIG_PORT_Pos) |
                            (pin << GPIOTE_CONFIG_PSEL_Pos) |
                            ((active_high ? GPIOTE_CONFIG_POLARITY_LoToHi : GPIOTE_CONFIG_POLARITY_HiToLo) << GPIOTE_CONFIG_POLARITY_Pos);
    NRF_PPI->CH[TRIG_PPI_CHAN].EEP = (uint32_t) &NRF_GPIOTE->EVENTS_IN[TRIG_GPIOTE_CHAN];
    NRF_PPI->CH[TRIG_PPI_CHAN].TEP =  (uint32_t) &TRIG_TIMER->TASKS_START;
    // Disable PPI channel until we arm trigger
    NRF_PPI->CHENCLR = (1 << TRIG_PPI_CHAN);
}


void set_trigger(uint32_t delay_ticks) {
    // Set the timer for the specified delay (in 16 MHz ticks)
    NRF_TIMER2->CC[0] = delay_ticks;
    NRF_TIMER2->TASKS_CLEAR = 1;

    // Enable the PPI channel to start the timer on the next trigger event
    NRF_GPIOTE->EVENTS_IN[TRIG_GPIOTE_CHAN] = 0; // Clear any pending events
    NRF_PPI->CHENSET = (1 << TRIG_PPI_CHAN);

    // Turn on the LED when armed
    NRF_P0->OUTSET = LED;
}

void jump_bootloader() {
    // go back to uf2 bootloader
    NRF_POWER->GPREGRET = 0x57; // 0x57 tells the Adafruit UF2 bootloader to stay in bootloader mode
    NVIC_SystemReset();         // Reboot the chip
}

void blink(int n) {
    for(int i = n-1; i >= 0; i--) {
        NRF_P0->OUTSET = LED;
        delay_ms(33);
        NRF_P0->OUTCLR = LED;
        if(i) delay_ms(33);
    }
}

void tud_mount_cb(void) {
    tud_vendor_write_clear();

    uint8_t dump_buf[64];
    while (tud_vendor_available()) {
        tud_vendor_read(dump_buf, sizeof(dump_buf));
    }

    gs_usb_cmd = 0;
}

void tud_vendor_rx_cb(uint8_t intf, const uint8_t *buffer, uint32_t bufsize) {
    // - CFG_TUD_VENDOR_TXRX_BUFFERED = 1: buffer and bufsize must not be used (both NULL,0) since data is in RX FIFO
    uint8_t buf[64]; // Buffer to hold the incoming command

    while (tud_vendor_available() > 0) {
        uint32_t bytes_read = tud_vendor_read(buf, sizeof(buf));
        if(!gs_usb_cmd && bytes_read > 0) {
            gs_usb_cmd = buf[0];
            if (bytes_read > 1) {
                memcpy((void*)gs_usb_buf, buf, bytes_read);
            }
        }
    }
}

void arm_capture(int freq, int delay_ticks) {

    init_radio(/*access address*/0, freq);

    nrf_radio_event_clear(NRF_RADIO, RFX_RADIO_EVENT_IQCAPSTART);
    nrf_radio_event_clear(NRF_RADIO, RFX_RADIO_EVENT_IQCAPEND);

    radio_set_iq_capture(iq_buf, MAXSAMP);
    radio_start_rx();

    radio_wait_for_state(NRF_RADIO_STATE_RX);
    delay_us(10);

    set_trigger(delay_ticks);
}

void iq_capture(int freq) {
    init_radio(/*access address*/0, freq);

    nrf_radio_event_clear(NRF_RADIO, RFX_RADIO_EVENT_IQCAPSTART);
    nrf_radio_event_clear(NRF_RADIO, RFX_RADIO_EVENT_IQCAPEND);

    radio_set_iq_capture(iq_buf, MAXSAMP);
    radio_start_rx();

    radio_wait_for_state(NRF_RADIO_STATE_RX);
    delay_us(10);

    // Trigger IQ capture immediately
    radio_trigger_iq_capture();
}

void bulk_send(uint8_t *buf, int num_bytes) {
    uint32_t total_bytes = num_bytes;
    uint32_t bytes_sent = 0;
    uint8_t* ptr = (uint8_t*)buf;

    while ((bytes_sent < total_bytes) && tud_vendor_mounted()) {
        // Calculate remaining bytes, but cap the request to 1024 bytes
        uint32_t chunk = total_bytes - bytes_sent;
        if (chunk > 1024) {
            chunk = 1024;
        }

        uint32_t pushed = tud_vendor_write(ptr, chunk);
        if (pushed > 0) {
            ptr += pushed;
            bytes_sent += pushed;
            tud_vendor_write_flush(); // Tell TinyUSB the data is ready to go
        }

        // Keep the USB state machine moving
        tud_task();
    }
}

void send_iq_samples(uint32_t *buf, int nsamp) {
    // Convert in place to sign extend 12-bit samples
    for(int i = 0; i < nsamp; i++) {
        uint32_t val = buf[i];

        int16_t i_sample = (int16_t)( ((int32_t)(val << 20)) >> 20 );
        int16_t q_sample = (int16_t)( ((int32_t)(val << 8))  >> 20 );

        // Pack as little endian shorts, I first, then Q
        buf[i] = (q_sample << 16) | (i_sample & 0xFFFF);

        // Yield to TinyUSB every 512 iterations so the USB connection doesn't drop
        if ((i % 256) == 0) {
            tud_task();
        }
    }

    bulk_send((uint8_t*)buf, nsamp * sizeof(uint32_t));
}

void usb_cmd_handler() {
    int freq, delay_ticks;
    if(gs_usb_cmd) {
        switch(gs_usb_cmd) {
        case NRF_CMD_REBOOT:
            blink(3);
            jump_bootloader();
            break;
        case NRF_CMD_USBTEST:
            blink(2);
            break;
        case NRF_CMD_IQCAPTURE_TRIG:
            freq = 2400 + gs_usb_buf[1];
            delay_ticks = ((uint32_t)gs_usb_buf[2] << 8) | gs_usb_buf[3];
            arm_capture(freq, delay_ticks);
            break;
        case NRF_CMD_IQCAPTURE_NOW:
            freq = 2400 + gs_usb_buf[1];
            iq_capture(freq);
            blink(1);
            break;
        case NRF_CMD_RADIO_STOP:
            radio_stop();
            blink(1);
            break;
        case NRF_CMD_PEEK32:
            volatile uint32_t* peek_addr =  (uint32_t*)(
                                            (gs_usb_buf[4] << 24) |
                                            (gs_usb_buf[5] << 16) |
                                            (gs_usb_buf[6] <<  8) |
                                            (gs_usb_buf[7] <<  0));
            uint32_t peek_val = *peek_addr;
            uint8_t peek_res[4] = {
                (peek_val >> 24) & 0xFF,
                (peek_val >> 16) & 0xFF,
                (peek_val >> 8) & 0xFF,
                peek_val & 0xFF
            };
            blink(1);
            bulk_send(peek_res, sizeof(peek_res));
            break;
        case NRF_CMD_POKE32:
            volatile uint32_t* poke_addr =  (uint32_t*)(
                                            (gs_usb_buf[4] << 24) |
                                            (gs_usb_buf[5] << 16) |
                                            (gs_usb_buf[6] <<  8) |
                                            (gs_usb_buf[7] <<  0));
            uint32_t poke_val = (gs_usb_buf[8] << 24) |
                                (gs_usb_buf[9] << 16) |
                                (gs_usb_buf[10] << 8) |
                                (gs_usb_buf[11] << 0);
            *poke_addr = poke_val;
            blink(1);
            break;            
        }
        gs_usb_cmd = 0;
    }
}

void capture_handler() {
    if (nrf_radio_event_check(NRF_RADIO, RFX_RADIO_EVENT_IQCAPEND)) {
        nrf_radio_event_clear(NRF_RADIO, RFX_RADIO_EVENT_IQCAPEND);
        send_iq_samples(iq_buf, MAXSAMP);
        blink(2);
    }
}

// entry point
void main(void) {
    // Relocate the interrupt vector table to the app start address for UF2
    SCB->VTOR = 0x26000;

    clock_init();
    delay_init();
    timer2_init();
    // Trigger on falling edge of P0.17
    init_trigger(0, 17, false);

    // LED
    NRF_P0->DIRSET = LED;
    NRF_P0->OUTCLR = LED;

    // Init USB
    init_usb_power_irq();
    tusb_init();
    NVIC_EnableIRQ(USBD_IRQn);

    blink(3);
    while(1) {
        tud_task();
        usb_cmd_handler();
        capture_handler();
    }
}
