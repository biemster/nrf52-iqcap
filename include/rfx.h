/**
 * @file rfx.h
 * @brief Extended RF-related functionality
 *
 */

#ifndef RFX_H__
#define RFX_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "nrf_radio.h"

#define EIO                 5  /**< I/O error */
#define EINVAL              22 /**< Invalid argument */

#define TASKS_IQCAP         (0x060)
#define EVENTS_IQCAPEND     (0x160)
#define EVENTS_IQCAPSTART   (0x164)
#define IQCONF              (0xa30)
#define REG_IQPTR           (0xa34)
#define REG_IQLEN           (0xa38)
#define REG_IQAMOUNT        (0xa3c)
#define REG_A40             (0xa40)
#define PHDMASEL            (0xa44)

#define RFX_RADIO_IQCAP_EN  (1 << 0)
#define RFX_RADIO_IQCAP_DIS (0 << 0)

#define RFX_RADIO_PHDMA_PKT (0 << 0)
#define RFX_RADIO_PHDMA_IQ  (1 << 0)


#define RFX_RADIO_TASK_IQCAP            (TASKS_IQCAP)
#define RFX_RADIO_EVENT_IQCAPEND        (EVENTS_IQCAPEND)
#define RFX_RADIO_EVENT_IQCAPSTART      (EVENTS_IQCAPSTART)


#define RADIO_REG(x) (((volatile uint32_t *)NRF_RADIO)[x >> 2])

#define BLE_LENGTH_OFFSET   1
#define BLE_PAYLOAD_OFFSET  3

/**
 * @brief Initialize BLE radio
 * @param access_addr The 32-bit access address to use (0x8e89bed6 for BLE advertisements)
 * @param frequency The frequency in MHz to set (note the radio is internally offset tuned)
 */
void init_radio(uint32_t access_addr, int frequency);

/**
 * @brief Configure bit counter event for packet reception
 * 
 * @param bitcount Number of bits to trigger after (0-255)
 * @return 0 on success
 *       - -EINVAL: Invalid bitcount value
 */
int radio_set_bcc(int bitcount);

/**
 * @brief Disable bit counter event
 * 
 * @return 0 on success
 */
int radio_disable_bcc(void);

/**
 * @brief Start IQ sample capture
 * 
 * @return 0 on success, negative error code on failure
 */
int radio_trigger_iq_capture(void);

/**
 * @brief Configure IQ sample capture during packet reception
 * 
 * If `data` is NULL, IQ capture is disabled.
 * 
 * @param data Pointer to buffer to store IQ samples
 * @param nsamp Number of IQ samples to capture
 * @return 0 on success, negative error code on failure
 */
int radio_set_iq_capture(uint32_t *data, size_t nsamp);

/**
 * @brief Start radio in RX mode
 * 
 * @return 0 on success, negative error code on failure
 */
int radio_start_rx(void);

/**
 * @brief Wait for the radio to enter a specific state
 * @param desired_state The radio state to wait for
 */
void radio_wait_for_state(nrf_radio_state_t desired_state);

#ifdef __cplusplus
}
#endif

#endif // RFX_H__
