/* -*- c++ -*- */
/*
 * Copyright 2026 iraciemsgter.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef INCLUDED_NRF52_IQ_STREAM_SOURCE_IMPL_H
#define INCLUDED_NRF52_IQ_STREAM_SOURCE_IMPL_H

#include <gnuradio/nrf52/iq_stream_source.h>
#include <libusb-1.0/libusb.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <atomic>
#include <chrono>

namespace gr {
namespace nrf52 {

class iq_stream_source_impl : public iq_stream_source
{
private:
    int d_freq;
    bool d_angle_mode;

    // USB State
    libusb_context *d_ctx;
    libusb_device_handle *d_dev;
    static const int NUM_XFERS = 16;
    struct libusb_transfer *d_xfers[NUM_XFERS];
    uint8_t *d_buffers[NUM_XFERS];
    uint8_t d_last_frame[1000];

    // Threading and Buffering
    bool d_running;
    std::thread d_usb_thread;
    std::mutex d_mutex;
    std::condition_variable d_cond;
    std::deque<uint8_t> d_byte_queue;

    std::atomic<unsigned int> d_dropped_frames;
    std::atomic<unsigned int> d_total_frames;
    std::chrono::steady_clock::time_point d_last_report_time;

    std::vector<uint8_t> d_local_buf;

    // HIGH-SPEED LOOKUP TABLE
    gr_complex d_lut[256][2];

    void usb_thread_func();
    void handle_usb_data(struct libusb_transfer *xfer);
    static void LIBUSB_CALL xfer_cb(struct libusb_transfer *xfer);

public:
    iq_stream_source_impl(int freq, bool angle_mode);
    ~iq_stream_source_impl();

    void set_freq(int freq);

    bool start() override;
    bool stop() override;
    int work(int noutput_items,
             gr_vector_const_void_star &input_items,
             gr_vector_void_star &output_items) override;
};

} // namespace nrf52
} // namespace gr

#endif /* INCLUDED_NRF52_IQ_STREAM_SOURCE_IMPL_H */
