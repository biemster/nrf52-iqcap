#include "nrf.h"
#include "rfx.h"
#include "nrf_radio.h"

static char *str_radio_state(nrf_radio_state_t state) {
    switch (state) {
        case NRF_RADIO_STATE_DISABLED: return "DISABLED";
        case NRF_RADIO_STATE_RXRU: return "RXRU";
        case NRF_RADIO_STATE_RXIDLE: return "RXIDLE";
        case NRF_RADIO_STATE_RXDISABLE: return "RXDISABLE";
        case NRF_RADIO_STATE_RX: return "RX";
        case NRF_RADIO_STATE_TXRU: return "TXRU";
        case NRF_RADIO_STATE_TXIDLE: return "TXIDLE";
        case NRF_RADIO_STATE_TXDISABLE: return "TXDISABLE";
        case NRF_RADIO_STATE_TX: return "TX";
        default: return "UNKNOWN";
    }
}

void init_radio(uint32_t access_addr, int frequency)
{
    nrf_radio_power_set(NRF_RADIO, true);
    /* Set modulation and bitrate */
    nrf_radio_mode_set(NRF_RADIO, NRF_RADIO_MODE_BLE_1MBIT);

    /* Set channel */
    nrf_radio_frequency_set(NRF_RADIO, frequency);
    // IV needs to match the frequency if using packet handler at all
    nrf_radio_datawhiteiv_set(NRF_RADIO, 38);

    /* Set packet config */
    nrf_radio_packet_conf_t pkt_conf = {
        .lflen = 6UL, // Length field length in bits
        .s0len = 1UL,
        .s1len = 2UL,
        .s1incl = false,
        .plen = NRF_RADIO_PREAMBLE_LENGTH_8BIT,
        .crcinc = false, // CRC included in length field
        .statlen = 0UL,
        .balen = 3UL,
        .big_endian = false,
        .maxlen = 0, // Max payload length
        .whiteen = true            // Packet whitening
    };
    nrf_radio_packet_configure(NRF_RADIO, &pkt_conf);

    /* Address */
    nrf_radio_base0_set(NRF_RADIO, access_addr << 8);
    nrf_radio_prefix0_set(NRF_RADIO, access_addr >> 24);
    nrf_radio_txaddress_set(NRF_RADIO, 0); // Transmit using logical address 0 (base0 and prefix0)
    
    if (access_addr) {
        nrf_radio_rxaddresses_set(NRF_RADIO, 1<<0); // Enable RX of logical address 0
    } else {
        nrf_radio_rxaddresses_set(NRF_RADIO, 0); // Disable RX
    }

    /* CRC Config */
    nrf_radio_crc_configure(NRF_RADIO,
                            3,
                            NRF_RADIO_CRC_ADDR_SKIP,
                            0x00065B);
    nrf_radio_crcinit_set(NRF_RADIO, 0x55555555);

    /* Shortcuts */
    // Ready -> Start shortcut
    nrf_radio_shorts_set(NRF_RADIO, NRF_RADIO_SHORT_READY_START_MASK);
}

int radio_set_bcc(int bitcount) {
    if (bitcount < 0 || bitcount > 255) {
        return -EINVAL;
    }
    
    NRF_RADIO->BCC = bitcount;

    nrf_radio_shorts_enable(NRF_RADIO, NRF_RADIO_SHORT_ADDRESS_BCSTART_MASK);
    return 0;
}

int radio_disable_bcc(void) {
    nrf_radio_shorts_disable(NRF_RADIO, NRF_RADIO_SHORT_ADDRESS_BCSTART_MASK);
    return 0;
}

int radio_set_iq_capture(uint32_t *data, size_t nsamp) {

    if (!data) {
        RADIO_REG(PHDMASEL) = RFX_RADIO_PHDMA_PKT;
        RADIO_REG(IQCONF) = RFX_RADIO_IQCAP_DIS;
        return 0;
    }

    RADIO_REG(IQCONF) = RFX_RADIO_IQCAP_DIS;

    // Never gets set?
    RADIO_REG(REG_A40) = 0; // Clear A40

    // Set the IQ capture pointer and length
    RADIO_REG(REG_IQPTR) = (uint32_t)data;
    RADIO_REG(REG_IQLEN) = nsamp;

    // Without A30 bit 0 set, IQ capture does not start
    // Without A44 bit 0 set, capture gets 0 samples but packet handler works
    RADIO_REG(PHDMASEL) = RFX_RADIO_PHDMA_IQ;
    RADIO_REG(IQCONF) = RFX_RADIO_IQCAP_EN;

    return 0;
}

int radio_trigger_iq_capture(void) {
    nrf_radio_task_trigger(NRF_RADIO, RFX_RADIO_TASK_IQCAP);
    return 0;
}

int radio_stop(void) {
    bool is_disabled = false;
    while (!is_disabled) {
        nrf_radio_state_t state = nrf_radio_state_get(NRF_RADIO);
        switch (state) {
            case NRF_RADIO_STATE_RX:
            case NRF_RADIO_STATE_TX:
                /* Stop TX/RX, return to TXIDLE/RXIDLE */
                nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_STOP);
                break;
            case NRF_RADIO_STATE_TXIDLE:
            case NRF_RADIO_STATE_RXIDLE:
                /* Need to go to DISABLE */
                nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_DISABLE);
                break;
            case NRF_RADIO_STATE_DISABLED:
                is_disabled = true;
                break;
            case NRF_RADIO_STATE_RXDISABLE:
            case NRF_RADIO_STATE_TXDISABLE:
                /* Keep waiting until we're in DISABLED */
                break;
            default:
                /* Radio in incompatible state. Give up */
                return -EIO;
        }
    }

    return 0;
}

int radio_start_rx(void) {
    // We want to be in DISABLED
    radio_stop();

    NRF_RADIO->PCNF1 = (NRF_RADIO->PCNF1 & ~RADIO_PCNF1_MAXLEN_Msk);

    // Clear the event flags
    nrf_radio_event_clear(NRF_RADIO, RFX_RADIO_EVENT_IQCAPEND);
    nrf_radio_event_clear(NRF_RADIO, RFX_RADIO_EVENT_IQCAPSTART);

    // Enter the RX state and begin listening (will shortcut from RXIDLE to RX)
    nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_RXEN);

    return 0;
}

void radio_wait_for_state(nrf_radio_state_t desired_state) {
    while (nrf_radio_state_get(NRF_RADIO) != desired_state);
}