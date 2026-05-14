/* -*- c++ -*- */
/*
 * Copyright 2026 iraciemsgter.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef INCLUDED_NRF52_IQ_STREAM_SOURCE_H
#define INCLUDED_NRF52_IQ_STREAM_SOURCE_H

#include <gnuradio/nrf52/api.h>
#include <gnuradio/sync_block.h>

namespace gr {
namespace nrf52 {

/*!
 * \brief <+description of block+>
 * \ingroup nrf52
 *
 */
class NRF52_API iq_stream_source : virtual public gr::sync_block
{
public:
    typedef std::shared_ptr<iq_stream_source> sptr;

    /*!
     * \brief Return a shared_ptr to a new instance of nrf52::iq_stream_source.
     *
     * To avoid accidental use of raw pointers, nrf52::iq_stream_source's
     * constructor is in a private implementation
     * class. nrf52::iq_stream_source::make is the public interface for
     * creating new instances.
     */
    static sptr make(int freq, bool angle_mode);
};

} // namespace nrf52
} // namespace gr

#endif /* INCLUDED_NRF52_IQ_STREAM_SOURCE_H */
