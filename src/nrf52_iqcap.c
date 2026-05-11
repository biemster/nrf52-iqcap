#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <sys/stat.h>

#include "nrf.h"
#include "rfx.h"
#include "tusb.h"
#include "device/usbd_pvt.h"

// #define DEBUG_USB

#define NRF_USB_EP_IN_BULK       0x81 
#define NRF_USB_EP_IN_ISO        0x88 // for the USB DMA feed, keep those in line with src/usb_descriptors.c and the python scripts

#define NRF_CMD_USBTEST          0xa1
#define NRF_CMD_REBOOT           0xa2
#define NRF_CMD_IQCAPTURE_TRIG   0xca
#define NRF_CMD_IQCAPTURE_NOW    0xcb
#define NRF_CMD_IQCAPTURE_STREAM 0xcc
#define NRF_CMD_RADIO_STOP       0xcf
#define NRF_CMD_PEEK32           0xd1
#define NRF_CMD_POKE32           0xd2

#define TRIG_TIMER               NRF_TIMER2
#define TRIG_TIMER_IRQn          TIMER2_IRQn
#define TRIG_GPIOTE_CHAN         2
#define TRIG_PPI_CHAN            2

#define LED                      (1 << 15) // the red led on the nice!nano
#define MAXSAMP                  (48*1024)
__attribute__((aligned(8192))) static uint32_t iq_buf[MAXSAMP];
__attribute__((aligned(8192))) static uint32_t usb_ring_buf[4][240];

static volatile int gs_usb_cmd;
static volatile int gs_streaming;
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
                if(gs_streaming && gs_usb_cmd != NRF_CMD_IQCAPTURE_STREAM) {
                    // have to do this here, because the usb cmd handler is not serviced during streaming mode
                    gs_streaming = 0;
                }
            }
        }
    }
}

void prepare_capture(int freq) {
    init_radio(/*access address*/0, freq);

    nrf_radio_event_clear(NRF_RADIO, RFX_RADIO_EVENT_IQCAPSTART);
    nrf_radio_event_clear(NRF_RADIO, RFX_RADIO_EVENT_IQCAPEND);

    radio_set_iq_capture(iq_buf, MAXSAMP);
    radio_start_rx();

    radio_wait_for_state(NRF_RADIO_STATE_RX);
    delay_us(10);
}

void arm_capture(int freq, int delay_ticks) {
    prepare_capture(freq);
    set_trigger(delay_ticks);
}

void iq_capture(int freq) {
    prepare_capture(freq);
    // Trigger IQ capture immediately
    radio_trigger_iq_capture();
}

void bulk_send(uint8_t *buf, int num_bytes) {
    uint32_t bytes_sent = 0;
    uint8_t* ptr = buf;
    uint32_t timeout_start = DWT->CYCCNT;

    while ((bytes_sent < num_bytes) && tud_vendor_mounted()) {
        uint32_t chunk = num_bytes - bytes_sent;
        if (chunk > 1024) chunk = 1024;

        // Wait until BULK endpoint is free
        while (tud_vendor_mounted() && usbd_edpt_busy(0, NRF_USB_EP_IN_BULK)) {
            tud_task();
            if ((DWT->CYCCNT - timeout_start) > 64000000) return; // 1s safety timeout
        }

        // Send via Bulk (100% lossless, no padding required)
        if (usbd_edpt_xfer(0, NRF_USB_EP_IN_BULK, ptr, chunk, false)) {
            ptr += chunk;
            bytes_sent += chunk;
            timeout_start = DWT->CYCCNT; 
        }
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

// 1. Extract using MUL
#define ASM_EXTRACT_1(ACC) \
    "ldr.w %[rVal], [%[p]], #32 \n"        /* Load every 8th sample */ \
    "sbfx %[rI], %[rVal], #0, #12 \n"      /* Extract I (sign extended) */ \
    "sbfx %[rQ], %[rVal], #12, #12 \n"     /* Extract Q (sign extended) */ \
    "mul %[rT1], %[rI], %[rI] \n"          /* rT1 = I^2 */ \
    "mul %[rT2], %[rQ], %[rQ] \n"          /* rT2 = Q^2 */ \
    "cmp %[rT1], %[rT2] \n"                /* Compare. Carry=1 if I^2 >= Q^2 */ \
    "ubfx %[rT1], %[rVal], #23, #1 \n"     /* Extract Qs */ \
    "ubfx %[rT2], %[rVal], #11, #1 \n"     /* Extract Is */ \
    "eor %[rI], %[rT1], %[rT2] \n"         /* b1 = Qs ^ Is */ \
    "adc %[rQ], %[rZ], %[rZ] \n"           /* rQ = C */ \
    "eor %[rQ], %[rI], %[rQ] \n"           /* b0_inv = b1 ^ C */ \
    "eor %[rQ], %[rQ], #1 \n"              /* b0 = b0_inv ^ 1 */ \
    "orr %[rQ], %[rQ], %[rI], lsl #1 \n"   /* rQ = b0 | (b1 << 1) */ \
    "orr %[rQ], %[rQ], %[rT1], lsl #2 \n"  /* rQ = b0 | (b1<<1) | (Qs<<2) */ \
    "lsr " ACC ", " ACC ", #3 \n"          /* Shift accumulator */ \
    "orr " ACC ", " ACC ", %[rQ], lsl #21 \n"

// 2. Multipliers
#define ASM_EXTRACT_4(ACC) ASM_EXTRACT_1(ACC) ASM_EXTRACT_1(ACC) ASM_EXTRACT_1(ACC) ASM_EXTRACT_1(ACC)
#define ASM_EXTRACT_8(ACC) ASM_EXTRACT_4(ACC) ASM_EXTRACT_4(ACC)

// 3. Process 32 Samples (Generates four 24-bit chunks and packs them into THREE 32-bit words)
#define ASM_PROCESS_32 \
    "mov %[AccA], #0 \n" \
    ASM_EXTRACT_8("%[AccA]")                                /* Chunk 0 */ \
    "mov %[AccB], #0 \n" \
    ASM_EXTRACT_8("%[AccB]")                                /* Chunk 1 */ \
    "orr %[rVal], %[AccA], %[AccB], lsl #24 \n"             /* Word 0: All of C0, lower 8 bits of C1 */ \
    "str %[rVal], [%[out_buf]], #4 \n" \
    "lsr %[AccA], %[AccB], #8 \n"                           /* Save remaining 16 bits of C1 to AccA */ \
    "mov %[AccB], #0 \n" \
    ASM_EXTRACT_8("%[AccB]")                                /* Chunk 2 */ \
    "orr %[rVal], %[AccA], %[AccB], lsl #16 \n"             /* Word 1: Upper 16 of C1, lower 16 of C2 */ \
    "str %[rVal], [%[out_buf]], #4 \n" \
    "lsr %[AccA], %[AccB], #16 \n"                          /* Save remaining 8 bits of C2 to AccA */ \
    "mov %[AccB], #0 \n" \
    ASM_EXTRACT_8("%[AccB]")                                /* Chunk 3 */ \
    "orr %[rVal], %[AccA], %[AccB], lsl #8 \n"              /* Word 2: Upper 8 of C2, all of C3 */ \
    "str %[rVal], [%[out_buf]], #4 \n"

// 4. The Wrapper Function
// NOTE: __attribute__((noinline)) is critical to prevent I-Cache thrashing.
__attribute__((noinline, optimize("no-unroll-loops")))
void process_and_push_frame(const uint32_t* stream_buf, uint32_t* out_buf) {
    uint32_t AccA, AccB, rVal, rI, rQ, rT1, rT2;
    const uint32_t* p = stream_buf;
    uint32_t rZ = 0;
    
    uint32_t* out = out_buf;

#ifdef DEBUG_USB
    uint32_t timestamps[16];
#endif

    // Generate exactly 960 bytes of pure samples linearly.
    // 80 blocks * 12 bytes = 960 bytes.
    for (int j = 0; j < 16; j++) {
        for (int i = 0; i < 5; i++) {
            __asm__ volatile ( 
                ASM_PROCESS_32 
                :[AccA]"=&r"(AccA), [AccB]"=&r"(AccB),[rVal]"=&r"(rVal),[rI]"=&r"(rI),[rQ]"=&r"(rQ),[rT1]"=&r"(rT1),[rT2]"=&r"(rT2),[p]"+r"(p),[out_buf]"+r"(out) 
                :[rZ]"r"(rZ) 
                :"cc", "memory" 
            );
        }

#ifdef DEBUG_USB
        // Snapshot the timer AFTER every 60 bytes generated
        timestamps[j] = DWT->CYCCNT >> 6;
#endif

    }

#ifdef DEBUG_USB
    // Overwrite the first 4 bytes of every 64-byte chunk (15 chunks total)
    for (int b = 0; b < 15; b++) {
        uint32_t sync_word = (b == 0) ? 0x55BB : 0x55AA;
        out_buf[b * 16] = ((timestamps[b] & 0xFFFF) << 16) | sync_word;
    }
#endif
}

void iqcapture_stream(int freq) {
    init_radio(0, freq);
    radio_start_rx();
    while (nrf_radio_state_get(NRF_RADIO) != NRF_RADIO_STATE_RX) { tud_task(); }

    nrf_radio_event_clear(NRF_RADIO, RFX_RADIO_EVENT_IQCAPSTART);
    nrf_radio_event_clear(NRF_RADIO, RFX_RADIO_EVENT_IQCAPEND);

    // 20,480 samples perfectly produces 960 bytes of pure 3-bit samples (1.28ms pacing)
    int streambuf_size = 20480; 
    uint32_t *capture_buf = iq_buf;
    uint32_t *process_buf = NULL;
    
    int write_idx = 0;
    int read_idx = 0;

    radio_set_iq_capture(capture_buf, streambuf_size);
    radio_trigger_iq_capture();
    uint32_t start_capture = DWT->CYCCNT;

    while(gs_streaming) {

        // Radio captures at 1.28ms
        if (nrf_radio_event_check(NRF_RADIO, RFX_RADIO_EVENT_IQCAPEND)) {
            nrf_radio_event_clear(NRF_RADIO, RFX_RADIO_EVENT_IQCAPEND);

            // Swap buffers and instantly re-trigger capture to maintain 0-gap continuity
            process_buf = capture_buf;
            capture_buf = (capture_buf == iq_buf) ? &iq_buf[streambuf_size] : iq_buf;
            radio_set_iq_capture(capture_buf, streambuf_size);
            radio_trigger_iq_capture();
            start_capture = DWT->CYCCNT;

            // Process directly into the ring buffer
            process_and_push_frame(process_buf, usb_ring_buf[write_idx]);
            write_idx = (write_idx + 1) & 3; // Advance modulo 4
        }

        // keep USB endpoint primed (1.00ms)
        if (tud_vendor_mounted() && !usbd_edpt_busy(0, NRF_USB_EP_IN_ISO)) {
            if (read_idx != write_idx) {
                // Buffer has data, Queue the 960 bytes.
                usbd_edpt_xfer(0, NRF_USB_EP_IN_ISO, (uint8_t*)usb_ring_buf[read_idx], 960, false);
                read_idx = (read_idx + 1) & 3; // Advance modulo 4
            }
            else {
                // send full dummy frame to keep the flow going
                int last_idx = (read_idx + 3) & 3;
                usb_ring_buf[last_idx][0] = (usb_ring_buf[last_idx][0] & 0xFFFF0000) | 0x55DD;
                usbd_edpt_xfer(0, NRF_USB_EP_IN_ISO, (uint8_t*)usb_ring_buf[last_idx], 960, false);
            }
        }

        // lower priority tasks, stop these when the IQ capture is about to end
        // 1600 cycles is 25us
        if((DWT->CYCCNT - start_capture) < (81920 - 1600)) {
            tud_task();

            if (gs_usb_cmd == NRF_CMD_RADIO_STOP) { 
                gs_streaming = 0; 
                gs_usb_cmd = 0; 
                break; 
            }
        }
    }
    
    radio_stop();
}

void usb_cmd_handler() {
    int freq, delay_ticks;
    if(gs_usb_cmd) {
        int cmd = gs_usb_cmd;
        gs_usb_cmd = 0; 

        switch(cmd) {
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
        case NRF_CMD_IQCAPTURE_STREAM:
            freq = 2400 + gs_usb_buf[1];
            gs_streaming = 1;
            iqcapture_stream(freq);
            break;
        case NRF_CMD_RADIO_STOP:
            gs_streaming = 0;
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

    // enable Instruction Cache
    NRF_NVMC->ICACHECNF = NVMC_ICACHECNF_CACHEEN_Enabled;

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
