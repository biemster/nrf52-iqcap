#!/usr/bin/env python3
import sys, argparse, time
import gc
import threading
import queue
import usb.core
import usb.util
import numpy as np
from scipy import signal

# ==============================================================================
# CONFIGURATION
# ==============================================================================
PAYLOAD_BYTES    = 35       
ACCESS_ADDRESS   = 0x8E89BED6 # 0x63683332 # b'ch32'

NRF_USB_EP_IN        = 0x88
NRF_USB_EP_OUT       = 0x01      
NRF_USB_PACKET_SIZE  = 1000 * 100
NRF_USB_TIMEOUT_MS   = 2000

NRF_CMD_IQCAP_STREAM = 0xcc
NRF_CMD_RADIO_STOP   = 0xcf
NRF_STR_IQCAP_STREAM = (NRF_CMD_IQCAP_STREAM, 0x00, 0x00, 0x01)
NRF_STR_RADIO_STOP   = (NRF_CMD_RADIO_STOP, 0x00, 0x00, 0x00)

BLE_FREQUENCIES =[
    2404, 2406, 2408, 2410, 2412, 2414, 2416, 2418, 2420, 2422, 2424, 
    2428, 2430, 2432, 2434, 2436, 2438,                               
    2440, 2442, 2444, 2446, 2448, 2450, 2452, 2454, 2456, 2458, 2460, 
    2462, 2464, 2466, 2468, 2470, 2472, 2474, 2476, 2478,             
    2402, 2426, 2480                                                  
]

class USBDevice:
    def __init__(self, idVendor=0xcafe, idProduct=0x4000):
        self.dev = usb.core.find(idVendor=idVendor, idProduct=idProduct)
        if self.dev is not None:
            try: self.dev.set_configuration()
            except Exception: pass

    def found(self): return self.dev is not None
    
    def clear_pipes(self):
        try:
            self.dev.set_configuration()
            self.dev.clear_halt(NRF_USB_EP_OUT)
            self.dev.read(NRF_USB_EP_IN, NRF_USB_PACKET_SIZE, 10)
        except Exception: pass
        
    def write(self, data):
        if not self.found(): raise RuntimeError("USB device not found")
        return self.dev.write(NRF_USB_EP_OUT, data)
        
    def read_packet(self, timeout=NRF_USB_TIMEOUT_MS):
        if not self.found(): raise RuntimeError("USB device not found")
        return self.dev.read(NRF_USB_EP_IN, NRF_USB_PACKET_SIZE, timeout=timeout)
        
    def start_stream(self, freq, is_angles):
        cmd = bytearray(NRF_STR_IQCAP_STREAM)
        cmd[1] = int(freq) - 2400
        cmd[2] = 1 if is_angles else 0
        cmd[3] = 1
        self.write(cmd)
        
    def stop_stream(self):
        self.write(NRF_STR_RADIO_STOP)

def get_expected_aa_bits(sync_hex):
    sync_bytes =[(sync_hex >> 24) & 0xFF, (sync_hex >> 16) & 0xFF, 
                  (sync_hex >> 8) & 0xFF, sync_hex & 0xFF]
    sync_bytes = sync_bytes[::-1] 
    bits =[]
    for b in sync_bytes:
        for i in range(8):
            bits.append((b >> i) & 1)
    return np.array(bits)

def bits_to_lsb_hex(bit_array):
    if len(bit_array) == 0: return np.array([]), ""
    byte_arrays = bit_array.reshape(-1, 8)
    lsb_first_bytes = np.packbits(np.fliplr(byte_arrays))
    return lsb_first_bytes, ' '.join([f"{b:02X}" for b in lsb_first_bytes])

# ==============================================================================
# THREAD: Dedicated USB Poller
# ==============================================================================
def usb_reader_thread(device, data_queue, stop_event):
    while not stop_event.is_set():
        try:
            data = device.read_packet(timeout=NRF_USB_TIMEOUT_MS)
            if data:
                data_queue.put(data)
        except usb.core.USBError:
            pass 

# ==============================================================================
# MAIN DEMODULATOR LOGIC
# ==============================================================================
def stream_and_demodulate(device, freq, is_angles, threshold, max_errors, debug_usb):
    mode_str = "DEBUG (Sync/Telemetry)" if debug_usb else "RELEASE (Pure IQ Stream)"
    print(f"\n[+] Starting continuous 4-bit Phase demodulation on {freq} MHz")
    print(f"[+] USB Parsing Mode        : {mode_str}")
    print(f"[+] Look for Access Address : 0x{ACCESS_ADDRESS:08X}")
    print(f"[+] Max Bit Errors Allowed  : {max_errors}/32")
    print("[!] Press Ctrl+C to stop...\n")
    
    device.clear_pipes()
    device.start_stream(freq, is_angles)

    data_queue = queue.Queue()
    stop_event = threading.Event()
    reader_thread = threading.Thread(target=usb_reader_thread, args=(device, data_queue, stop_event))
    reader_thread.daemon = True

    print("[+] Disabling Python GC during capture to stop frame drops...")
    gc.disable() 

    reader_thread.start()

    sps = 2
    ideal_aa_bits = get_expected_aa_bits(ACCESS_ADDRESS)
    ideal_waveform = np.repeat(ideal_aa_bits * 2 - 1, sps)
    b_filt, a_filt = signal.butter(6, 555e3 / (2e6 / 2), btype='low')

    angle_buffer = np.array([], dtype=np.float32)
    total_bits_to_extract = (1 + 4 + PAYLOAD_BYTES) * 8
    
    noise_frames_ignored = 0
    raw_usb_buffer = bytearray()
    
    # Sync Words (For Debug Mode)
    SYNC_MAIN  = b'\xBB\x55' 
    SYNC_DUMMY = b'\xDD\x55' 
    SYNC_SUB   = b'\xAA\x55' 

    # Look-up table for high-speed symmetric 2-bit 2's complement conversion
    # Maps unsigned bits (00, 01, 10, 11) to perfectly symmetric floats
    iq_lut = np.array([0.5, 1.5, -1.5, -0.5], dtype=np.float32)

    expected_ts = None
    stats_count = 0
    stats_sum = 0.0
    stats_sum_sq = 0.0
    last_valid_frame = None

    try:
        while True:
            # 1. READ CHUNK
            data = device.read_packet(timeout=NRF_USB_TIMEOUT_MS)
            raw_usb_buffer.extend(data)

            # 2. PARSE ALL FRAMES (1000 byte blocks)
            batch_data_bytes = bytearray()
            
            while len(raw_usb_buffer) >= 1000:
                
                # ==========================================
                # PATH A: DEBUG FIRMWARE (Hunts for sync)
                # ==========================================
                if debug_usb:
                    b0 = raw_usb_buffer[0:2]
                    b1 = raw_usb_buffer[100:102]

                    is_valid = (b0 == SYNC_MAIN and b1 == SYNC_SUB)
                    is_dummy = (b0 == SYNC_DUMMY and b1 == SYNC_SUB)

                    if not (is_valid or is_dummy):
                        idx = -1
                        for i in range(1, len(raw_usb_buffer) - 101):
                            s0 = raw_usb_buffer[i:i+2]
                            s1 = raw_usb_buffer[i+100:i+102]
                            if (s0 == SYNC_MAIN or s0 == SYNC_DUMMY) and s1 == SYNC_SUB:
                                idx = i
                                break
                        
                        if idx == -1:
                            raw_usb_buffer = raw_usb_buffer[-200:]
                            break
                            
                        sys.stdout.write(f"\r\033[K[!] Skipped {idx} bytes of garbage to re-sync\n")
                        raw_usb_buffer = raw_usb_buffer[idx:]
                        if len(raw_usb_buffer) < 1000:
                            break 
                        
                        is_dummy = (raw_usb_buffer[0:2] == SYNC_DUMMY)

                    frame = raw_usb_buffer[:1000]
                    del raw_usb_buffer[:1000] 
                    
                    if is_dummy:
                        continue
                    
                    timestamps =[]
                    for b in range(10):
                        header = int.from_bytes(frame[b*100 : b*100+4], byteorder='little')
                        timestamps.append((header >> 16) & 0xFFFF)
                        batch_data_bytes.extend(frame[b*100+4 : (b+1)*100])
                    
                    ts_us = timestamps[0]
                    if expected_ts is not None:
                        delta_us = (ts_us - expected_ts) & 0xFFFF

                        if delta_us < 2000:
                            stats_count += 1
                            stats_sum += delta_us
                            stats_sum_sq += delta_us * delta_us
                        
                        if delta_us > 1800:
                            missed = round(delta_us / 1000.0) - 1
                            sys.stdout.write(f"\n[!] 🔴 DROP DETECTED: Gap of {delta_us} µs (~{missed} frames lost)\n")
                            sys.stdout.flush()
                            
                    expected_ts = ts_us

                    if stats_count > 1:
                        avg_d = stats_sum / stats_count
                        internal_deltas = [(timestamps[i] - timestamps[i-1]) & 0xFFFF for i in range(1, 10)]
                        avg_exec = sum(internal_deltas) / len(internal_deltas)
                        max_exec = max(internal_deltas)
                        
                        sys.stdout.write(f"\r\033[K[MCU] Radio Gap: {avg_d:.1f}µs | ASM Block Exec: {avg_exec:.1f}µs/block (Max: {max_exec}µs)")
                        sys.stdout.flush()

                # ==========================================
                # PATH B: RELEASE FIRMWARE (Pure 1000 bytes)
                # ==========================================
                else:
                    frame = raw_usb_buffer[:1000]
                    del raw_usb_buffer[:1000]
                    
                    if last_valid_frame is not None and frame[4:] == last_valid_frame[4:]:
                        continue
                        
                    last_valid_frame = frame
                    batch_data_bytes.extend(frame)

            # --- VECTORIZED NUMPY 4-BIT EXTRACTION ---
            if batch_data_bytes:
                raw_bytes = np.frombuffer(batch_data_bytes, dtype=np.uint8)
                nibbles = np.empty(len(raw_bytes)*2, dtype=np.uint8)
                nibbles[0::2] = raw_bytes & 0x0F
                nibbles[1::2] = raw_bytes >> 4

                if is_angles:
                    # Scale directly to Radians (-pi to pi)
                    new_angles = (nibbles.astype(np.float32) / 16.0) * (2 * np.pi) - np.pi
                else:
                    I_val = nibbles & 0x03
                    Q_val = nibbles >> 2
    
                    # Fast Look-up to convert unsigned to 2's comp signed floats
                    I_signed = iq_lut[I_val]
                    Q_signed = iq_lut[Q_val]
    
                    new_angles = np.arctan2(Q_signed, I_signed)
                angle_buffer = np.concatenate((angle_buffer, new_angles))


            # 3. RUN DSP ON THE WHOLE BUFFER ONCE
            if len(angle_buffer) >= 10000:
                # Phase unwrap via difference and modulo pi map
                fm_demod = angle_buffer[1:] - angle_buffer[:-1]
                fm_demod = (fm_demod + np.pi) % (2 * np.pi) - np.pi
                
                # Scale phase diff (radians) by (4 / pi) to perfectly match your old 3-bit threshold limits (-4 to +4 mag)
                fm_demod *= (4.0 / np.pi) 

                fm_filtered = signal.lfilter(b_filt, a_filt, fm_demod)

                corr = np.correlate(fm_filtered, ideal_waveform, mode='valid')
                score = np.abs(corr) / len(ideal_waveform)

                peaks, _ = signal.find_peaks(score, height=threshold, distance=200*sps)
                highest_processed_peak = -1

                for peak in peaks:
                    aa_start_idx = peak
                    preamble_start_idx = aa_start_idx - (8 * sps)
                    
                    if preamble_start_idx < 0 or (preamble_start_idx + total_bits_to_extract * sps) > len(fm_filtered):
                        break

                    first_sample = preamble_start_idx + (sps // 2)
                    sample_indices = np.arange(first_sample, first_sample + total_bits_to_extract * sps, sps)
                    sampled_values = fm_filtered[sample_indices]
                    
                    dc_offset = np.mean(sampled_values[:8]) 
                    sampled_values -= dc_offset 
                    
                    is_inverted = corr[peak] < 0
                    if is_inverted:
                        sampled_values = -sampled_values
                    
                    raw_bits = (sampled_values > 0).astype(int)
                    aa_bits = raw_bits[8:40]
                    
                    bit_errors = np.sum(aa_bits != ideal_aa_bits)
                    
                    if bit_errors <= max_errors:
                        full_bytes, full_hex = bits_to_lsb_hex(raw_bits)
                        ascii_str = ''.join([chr(b) if 32 <= b <= 126 else '.' for b in full_bytes])
                        
                        sys.stdout.write("\n\n--- 🟢 VALID PACKET FOUND 🟢 ---\n")
                        if is_inverted:
                            print("⚠️ I/Q Swap Detected! Automatically inverted signal to fix.")
                        print(f"Match Index   : {peak} | Correlation: {score[peak]:.2f} | AA Bit Errors: {bit_errors}/32")
                        print(f"Payload HEX   : {full_hex}")
                        print(f"Payload ASCII : {ascii_str}\n")
                    else:
                        noise_frames_ignored += 1
                        if not debug_usb:
                            sys.stdout.write(f"\r\033[K[~] Scanning... Ignored false-positive noise peaks: {noise_frames_ignored}")
                            sys.stdout.flush()
                    
                    highest_processed_peak = peak

                if highest_processed_peak != -1:
                    angle_buffer = angle_buffer[highest_processed_peak + total_bits_to_extract * sps:]
                else:
                    angle_buffer = angle_buffer[max(0, len(angle_buffer) - 2000):]

    except KeyboardInterrupt:
        print("\n\n[+] Stopping stream...")
        stop_event.set()
        reader_thread.join(timeout=1.0)
        device.stop_stream()
        gc.enable()

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('-c', '--channel', help='Start on BLE channel index (0-39)')
    parser.add_argument('-f', '--frequency', help='Start on frequency (MHz)')
    parser.add_argument('-a', '--angles', action='store_true', help='Stream 4 bit angles instead of IQ samples')
    parser.add_argument('-t', '--threshold', help='Correlation threshold (0.0 to 1.0)', type=float, default=0.40)
    parser.add_argument('-e', '--errors', help='Max Bit errors allowed in AA', type=int, default=2)
    parser.add_argument('-d', '--debug-usb', action='store_true', help='Analyze USB frame drops and MCU timings')
    args = parser.parse_args()

    device = USBDevice()
    if not device.found():
        print("Error: nRF52 USB device not found.")
        sys.exit(1)

    if args.channel:
        freq = BLE_FREQUENCIES[int(args.channel)] + 1 
    elif args.frequency:
        freq = int(args.frequency)
    else:
        freq = BLE_FREQUENCIES[37] + 1

    stream_and_demodulate(device, freq, args.angles, args.threshold, args.errors, args.debug_usb)

if __name__ == '__main__':
    main()
