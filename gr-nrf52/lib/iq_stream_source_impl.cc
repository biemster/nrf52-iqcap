/* -*- c++ -*- */
/*
 * Copyright 2026 iraciemsgter.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "iq_stream_source_impl.h"
#include <gnuradio/io_signature.h>
#include <iostream>
#include <cstring>
#include <cmath>
#include <chrono>

namespace gr {
namespace nrf52 {

iq_stream_source::sptr
iq_stream_source::make(int freq, bool angle_mode)
{
    return gnuradio::get_initial_sptr(
        new iq_stream_source_impl(freq, angle_mode));
}

iq_stream_source_impl::iq_stream_source_impl(int freq, bool angle_mode)
    : gr::sync_block("iq_stream_source",
                     gr::io_signature::make(0, 0, 0),
                     gr::io_signature::make(1, 1, sizeof(gr_complex))),
      d_freq(freq), d_angle_mode(angle_mode),
      d_ctx(nullptr), d_dev(nullptr), d_running(false),
      d_dropped_frames(0), d_total_frames(0)
{
    set_output_multiple(2);
    d_local_buf.reserve(1000000);

    memset(d_last_frame, 0, sizeof(d_last_frame));
    d_last_report_time = std::chrono::steady_clock::now();

    // PRE-COMPUTE THE LOOK-UP TABLE (Eliminates all math from the work loop)
    const float iq_lut_vals[4] = {0.5f, 1.5f, -1.5f, -0.5f};

    for (int i = 0; i < 256; i++) {
        uint8_t n0 = i & 0x0F;
        uint8_t n1 = i >> 4;

        if (d_angle_mode) {
            float phase0 = (n0 / 16.0f) * 2.0f * M_PI - M_PI;
            float phase1 = (n1 / 16.0f) * 2.0f * M_PI - M_PI;
            d_lut[i][0] = gr_complex(std::cos(phase0), std::sin(phase0));
            d_lut[i][1] = gr_complex(std::cos(phase1), std::sin(phase1));
        }
        else {
            float i0 = iq_lut_vals[n0 & 0x03];
            float q0 = iq_lut_vals[n0 >> 2];
            float i1 = iq_lut_vals[n1 & 0x03];
            float q1 = iq_lut_vals[n1 >> 2];
            d_lut[i][0] = gr_complex(i0, q0);
            d_lut[i][1] = gr_complex(i1, q1);
        }
    }
}

iq_stream_source_impl::~iq_stream_source_impl()
{
}

void iq_stream_source_impl::set_freq(int freq)
{
    d_freq = freq;
    if (d_running && d_dev) {
        uint8_t cmd_stop[4] = { 0xcf, 0x00, 0x00, 0x00 };
        int transferred;
        libusb_bulk_transfer(d_dev, 0x01, cmd_stop, sizeof(cmd_stop), &transferred, 100);
        
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        
        uint8_t hw_freq = (uint8_t)((d_freq + 1) - 2400);
        uint8_t cmd_start[4] = { 0xcc, hw_freq, (uint8_t)(d_angle_mode ? 1 : 0), 0x01 };
        libusb_bulk_transfer(d_dev, 0x01, cmd_start, sizeof(cmd_start), &transferred, 100);
    }
}

bool iq_stream_source_impl::start()
{
    if (libusb_init(&d_ctx) < 0) return false;

    d_dev = libusb_open_device_with_vid_pid(d_ctx, 0xcafe, 0x4000);
    if (!d_dev) {
        libusb_exit(d_ctx);
        return false;
    }

    libusb_set_auto_detach_kernel_driver(d_dev, 1);
    libusb_set_configuration(d_dev, 1);
    
    if (libusb_claim_interface(d_dev, 0) < 0) return false;

    libusb_clear_halt(d_dev, 0x01);

    int transferred;
    uint8_t cmd_stop[4] = { 0xcf, 0x00, 0x00, 0x00 };
    libusb_bulk_transfer(d_dev, 0x01, cmd_stop, sizeof(cmd_stop), &transferred, 200);

    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    uint8_t hw_freq = (uint8_t)((d_freq + 1) - 2400);
    uint8_t cmd_start[4] = { 0xcc, hw_freq, (uint8_t)(d_angle_mode ? 1 : 0), 0x01 };
    libusb_bulk_transfer(d_dev, 0x01, cmd_start, sizeof(cmd_start), &transferred, 200);

    for (int i = 0; i < NUM_XFERS; i++) {
        d_xfers[i] = libusb_alloc_transfer(8);
        d_buffers[i] = (uint8_t*)malloc(1000 * 8);
        libusb_fill_iso_transfer(d_xfers[i], d_dev, 0x88, d_buffers[i],
                                 1000 * 8, 8, xfer_cb, this, 1000);
        libusb_set_iso_packet_lengths(d_xfers[i], 1000);
        libusb_submit_transfer(d_xfers[i]);
    }

    d_running = true;
    d_usb_thread = std::thread(&iq_stream_source_impl::usb_thread_func, this);

    std::cout << "[nRF52] Stream started on " << d_freq << " MHz. HW Tuned to " << d_freq + 1 << " MHz." << std::endl;
    return true;
}

bool iq_stream_source_impl::stop()
{
    d_running = false;
    d_cond.notify_all();

    if (d_dev) {
        uint8_t cmd_stop[4] = { 0xcf, 0x00, 0x00, 0x00 };
        int transferred;
        libusb_bulk_transfer(d_dev, 0x01, cmd_stop, sizeof(cmd_stop), &transferred, 1000);

        for (int i = 0; i < NUM_XFERS; i++) {
            libusb_cancel_transfer(d_xfers[i]);
        }
    }

    if (d_usb_thread.joinable()) d_usb_thread.join();

    for (int i = 0; i < NUM_XFERS; i++) {
        libusb_free_transfer(d_xfers[i]);
        free(d_buffers[i]);
    }

    if (d_dev) {
        libusb_release_interface(d_dev, 0);
        libusb_close(d_dev);
        d_dev = nullptr;
    }
    if (d_ctx) {
        libusb_exit(d_ctx);
        d_ctx = nullptr;
    }

    return true;
}

void iq_stream_source_impl::usb_thread_func()
{
    struct timeval tv = {0, 100000};
    while (d_running) {
        libusb_handle_events_timeout_completed(d_ctx, &tv, nullptr);
    }
    for(int i=0; i<5; i++) libusb_handle_events_timeout_completed(d_ctx, &tv, nullptr);
}

void LIBUSB_CALL iq_stream_source_impl::xfer_cb(struct libusb_transfer *xfer)
{
    auto *block = static_cast<iq_stream_source_impl*>(xfer->user_data);
    if (xfer->status == LIBUSB_TRANSFER_COMPLETED) block->handle_usb_data(xfer);
    if (block->d_running) libusb_submit_transfer(xfer);
}

void iq_stream_source_impl::handle_usb_data(struct libusb_transfer *xfer)
{
    std::lock_guard<std::mutex> lock(d_mutex);
    bool data_added = false;

    for (int i = 0; i < xfer->num_iso_packets; i++) {
        auto &pack = xfer->iso_packet_desc[i];
        if (pack.status == LIBUSB_TRANSFER_COMPLETED && pack.actual_length == 1000) {
            uint8_t *buf = libusb_get_iso_packet_buffer_simple(xfer, i);

            if (buf[0] == 0xDD && buf[1] == 0x55 && memcmp(buf + 4, d_last_frame + 4, 996) == 0) {
                // dummy frame to keep the USB feed going, we can drop this
                continue; 
            }
            memcpy(d_last_frame, buf, 1000);

            // Keep latency under 100ms. No IO printing to prevent thread stalling.
            if (d_byte_queue.size() >= 100000) {
                d_byte_queue.erase(d_byte_queue.begin(), d_byte_queue.begin() + 1000);
                d_dropped_frames++;
            }

            d_byte_queue.insert(d_byte_queue.end(), buf, buf + 1000);
            d_total_frames++;
            data_added = true;
        }
    }

    if (data_added) d_cond.notify_one();
}

int iq_stream_source_impl::work(int noutput_items,
                                gr_vector_const_void_star &input_items,
                                gr_vector_void_star &output_items)
{
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::seconds>(now - d_last_report_time).count() >= 1) {
        unsigned int dropped = d_dropped_frames.exchange(0);
        unsigned int total = d_total_frames.exchange(0);
        
        if (total > 0 || dropped > 0) {
            float drop_pct = (dropped * 100.0f) / (total + 0.0001f); // Fixed math!
            std::cout << "[nRF52] Frames from USB: " << total 
                      << " | Dropped: " << dropped 
                      << " (" << drop_pct << "%)" << std::endl;
        }
        d_last_report_time = now;
    }

    gr_complex *out = (gr_complex *) output_items[0];
    int bytes_to_process = 0;

    {
        std::unique_lock<std::mutex> lock(d_mutex);
        d_cond.wait_for(lock, std::chrono::milliseconds(200), [this] {
            return !d_byte_queue.empty() || !d_running;
        });

        if (!d_running && d_byte_queue.empty()) return -1;
        if (d_byte_queue.empty()) return 0;

        // Because of set_output_multiple(2), noutput_items is guaranteed to be even!
        bytes_to_process = std::min((int)d_byte_queue.size(), noutput_items / 2);
        
        // Zero-allocation memory copy
        d_local_buf.assign(d_byte_queue.begin(), d_byte_queue.begin() + bytes_to_process);
        d_byte_queue.erase(d_byte_queue.begin(), d_byte_queue.begin() + bytes_to_process);
    } 

    // Blazing fast array lookup
    for (int i = 0; i < bytes_to_process; i++) {
        uint8_t b = d_local_buf[i];
        out[2 * i]     = d_lut[b][0];
        out[2 * i + 1] = d_lut[b][1];
    }

    return bytes_to_process * 2;
}

} /* namespace nrf52 */
} /* namespace gr */
