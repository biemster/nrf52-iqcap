#!/usr/bin/env python
import sys,argparse
import usb.core
import usb.util
from time import sleep

try:
    import tkinter as tk
    from tkinter import ttk, messagebox
    import numpy as np
    from matplotlib.figure import Figure
    from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg
    GUI_AVAILABLE = True
except ImportError:
    GUI_AVAILABLE = False

NRF_USB_EP_IN        = 0x81      # endpoint for data transfer in
NRF_USB_EP_OUT       = 0x01      # endpoint for command transfer out
NRF_USB_PACKET_SIZE  = 14*1024*4 # packet size
NRF_USB_TIMEOUT_MS   = 100       # timeout for normal USB operations

NRF_CMD_USBTEST     = 0xa1
NRF_CMD_REBOOT      = 0xa2
NRF_CMD_IQCAP_TRIG  = 0xca
NRF_CMD_IQCAP_NOW   = 0xcb
NRF_CMD_RADIO_STOP  = 0xcf
NRF_CMD_PEEK32      = 0xd1
NRF_CMD_POKE32      = 0xd2
NRF_STR_USBTEST     = (NRF_CMD_USBTEST, 0x01, 0x00, 0x01)
NRF_STR_REBOOT      = (NRF_CMD_REBOOT, 0x01, 0x00, 0x01)
NRF_STR_IQCAP_TRIG  = (NRF_CMD_IQCAP_TRIG, 0x00, 0x00, 0x01)
NRF_STR_IQCAP_NOW   = (NRF_CMD_IQCAP_NOW, 0x00, 0x00, 0x01)
NRF_STR_RADIO_STOP  = (NRF_CMD_RADIO_STOP, 0x00, 0x00, 0x00)
NRF_STR_PEEK32      = (NRF_CMD_PEEK32, 0x00, 0x00, 0x00, 0, 0, 0, 0)
NRF_STR_POKE32      = (NRF_CMD_POKE32, 0x00, 0x00, 0x00, 0, 0, 0, 0, 0, 0, 0, 0)


BLE_FREQUENCIES = [
    2404, 2406, 2408, 2410, 2412, 2414, 2416, 2418, 2420, 2422, 2424, # 0-10
    2428, 2430, 2432, 2434, 2436, 2438,                               # 11-16
    2440, 2442, 2444, 2446, 2448, 2450, 2452, 2454, 2456, 2458, 2460, # 17-27
    2462, 2464, 2466, 2468, 2470, 2472, 2474, 2476, 2478,             # 28-36
    2402, 2426, 2480                                                  # 37, 38, 39
]

class USBDevice:
    def __init__(self, idVendor=0xcafe, idProduct=0x4000):
        self.idVendor = idVendor
        self.idProduct = idProduct
        self.dev = usb.core.find(idVendor=self.idVendor, idProduct=self.idProduct)
        if self.dev is not None:
            try:
                self.dev.set_configuration()
            except Exception:
                pass

    def found(self):
        return self.dev is not None

    def clear_pipes(self):
        try:
            self.dev.set_configuration()
            self.dev.clear_halt(NRF_USB_EP_IN)
            self.dev.clear_halt(NRF_USB_EP_OUT)
            # try a short read to flush
            self.dev.read(NRF_USB_EP_IN, NRF_USB_PACKET_SIZE, 10)
        except Exception:
            pass

    def write(self, data):
        if not self.found():
            raise RuntimeError("USB device not found")
        return self.dev.write(NRF_USB_EP_OUT, data)

    def read_packet(self, timeout=NRF_USB_TIMEOUT_MS):
        if not self.found():
            raise RuntimeError("USB device not found")
        return self.dev.read(NRF_USB_EP_IN, NRF_USB_PACKET_SIZE, timeout=timeout)

    def read_stream(self, expect_trigger=False):
        raw_data = bytearray()
        while True:
            try:
                data = self.read_packet(timeout=NRF_USB_TIMEOUT_MS)
                raw_data.extend(data)
            except usb.core.USBError:
                if len(raw_data) == 0 and expect_trigger:
                    continue
                break
        return raw_data
    
    def enter_bootloader(self):
        self.write(NRF_STR_REBOOT)

    def usb_test(self):
        self.write(NRF_STR_USBTEST)

    def peek32(self, addr):
        cmd = bytearray(NRF_STR_PEEK32)
        cmd[4:8] = addr.to_bytes(4, 'big')
        self.write(cmd)
        data = self.read_packet()
        if len(data) >= 4:
            return int.from_bytes(data[0:4], 'big')
        return None

    def poke32(self, addr, value):
        cmd = bytearray(NRF_STR_POKE32)
        cmd[4:8] = addr.to_bytes(4, 'big')
        cmd[8:12] = value.to_bytes(4, 'big')
        self.write(cmd)

    def send_iqcap(self, freq, trigger=False, delay=None):
        if trigger:
            cmd = bytearray(NRF_STR_IQCAP_TRIG)
        else:
            cmd = bytearray(NRF_STR_IQCAP_NOW)
        cmd[1] = int(freq) - 2400
        if delay:
            delay_ticks = int(float(delay) * 8)
            cmd[2] = (delay_ticks >> 8) & 0xff
            cmd[3] = delay_ticks & 0xff
        else:
            cmd[2] = 0
            cmd[3] = 1
        self.write(cmd)
        return self.read_stream(expect_trigger=trigger)

def launch_gui(device, args):
    if not GUI_AVAILABLE:
        print("Error: Missing GUI libraries. Run 'pip install numpy matplotlib' first.")
        sys.exit(1)

    root = tk.Tk()
    root.title("nRF52 IQ Capture Tool")
    root.geometry("750x650") # Give the window a nice default starting size

    channel_options = []
    for ch_idx, freq in enumerate(BLE_FREQUENCIES):
        channel_options.append(f"Channel {ch_idx} ({freq} MHz)")

    fig = Figure(figsize=(6, 5), dpi=100)
    ax = fig.add_subplot(111)
    ax.set_title("IQ Constellation")
    ax.set_xlabel("In-Phase (I)")
    ax.set_ylabel("Quadrature (Q)")
    ax.grid(True, linestyle='--', alpha=0.6)

    canvas = FigureCanvasTkAgg(fig, master=root)
    canvas.get_tk_widget().pack(side=tk.TOP, fill=tk.BOTH, expand=True)

    separator = ttk.Separator(root, orient='horizontal')
    separator.pack(side=tk.TOP, fill=tk.X)

    bottom_frame = ttk.Frame(root, padding="15 15 15 15")
    bottom_frame.pack(side=tk.BOTTOM, fill=tk.X)

    controls_frame = ttk.Frame(bottom_frame)
    controls_frame.pack(anchor=tk.CENTER)

    ttk.Label(controls_frame, text="Select Frequency:").pack(side=tk.LEFT, padx=(0, 8))

    combo = ttk.Combobox(controls_frame, values=channel_options, state="readonly", width=25)
    if args.channel:
        # If a channel argument is provided, pre-select it in the dropdown
        combo.current(int(args.channel))
    else:
        combo.current(37) # Default to Advertising Channel 37 (2402 MHz)
    combo.pack(side=tk.LEFT, padx=(0, 15))

    def on_capture():
        # Get selected frequency
        idx = combo.current()
        freq = BLE_FREQUENCIES[idx]

        device.clear_pipes()

        try:
            data = device.send_iqcap(freq, trigger=args.trigger, delay=args.delay)
        except Exception as e:
            messagebox.showerror("USB Error", f"Failed to send command: {e}")
            return

        if not data:
            messagebox.showwarning("Warning", "No data received from USB endpoint.")
            return

        # Save the raw file identically to the CLI
        with open('capture.raw', 'wb') as f:
            f.write(data)

        # Process the 10-bit signed IQ data
        arr = np.frombuffer(data, dtype=np.int16)
        if len(arr) % 2 != 0:
            arr = arr[:-1]

        I_samples = arr[0::2]
        Q_samples = arr[1::2]

        # Update the plot
        ax.clear()
        ax.set_title(f"IQ Constellation - {combo.get()}")
        ax.set_xlabel("In-Phase (I)")
        ax.set_ylabel("Quadrature (Q)")
        ax.grid(True, linestyle='--', alpha=0.6)

        # Scatter plot (s=point size, alpha=transparency for dense overlaps)
        ax.scatter(I_samples, Q_samples, s=1, alpha=0.4, color='#1f77b4')

        ax.set_aspect('equal', adjustable='datalim')
        canvas.draw()

    btn = ttk.Button(controls_frame, text="Capture", command=on_capture)
    btn.pack(side=tk.LEFT)

    root.mainloop()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('-g', '--gui', help='Launch GUI', action='store_true')
    parser.add_argument('-b', '--bootloader', help='Reboot to bootloader', action='store_true')
    parser.add_argument('-c', '--channel', help='start IQ capture on BLE channel')
    parser.add_argument('-f', '--frequency', help='start IQ capture on frequency (MHz)')
    parser.add_argument('-d', '--delay', help='delay after trigger before starting capture (us)')
    parser.add_argument('-t', '--trigger', help='Wait for GPIO trigger', action='store_true')
    parser.add_argument('-u', '--usbtest', help='Send test command over USB to blink the LED', action='store_true')
    parser.add_argument('--peek', help='Peek 32-bit value from address', type=lambda x: int(x, base=0))
    parser.add_argument('--poke', help='Poke 32-bit value to address (addr value)', nargs=2, type=lambda x: int(x, base=0))
    args = parser.parse_args()

    device = USBDevice()

    if not device.found():
        print("nRF52 not found")
        exit(0)

    if len(sys.argv) == 1 or args.gui:
        launch_gui(device, args)
        return

    device.clear_pipes()

    if args.bootloader:
        print('rebooting to bootloader')
        try:
            device.enter_bootloader()
        except Exception as e:
            print(f"Command sent (device may have reset already): {e}")
    elif args.usbtest:
        print('USB test, blinking the LED')
        try:
            device.usb_test()
        except Exception as e:
            print(f"Exception: {e}")
    elif args.peek is not None:
        print(f'Peeking address 0x{args.peek:08x}')
        peek_val = device.peek32(args.peek)
        print(f'Value at 0x{args.peek:08x} = 0x{peek_val:08x}')
    elif args.poke is not None:
        addr, value = args.poke
        print(f'Poking value 0x{value:08x} to address 0x{addr:08x}')
        device.poke32(addr, value)
        # device.usb_test()
    elif args.channel or args.frequency:
        if args.channel:
            freq = BLE_FREQUENCIES[int(args.channel)]
            print(f'Starting capture on channel {args.channel} ({freq} MHz)')
        elif args.frequency:
            freq = int(args.frequency)
            print(f'Starting capture on {freq} MHz')
        
        data = device.send_iqcap(freq, trigger=args.trigger, delay=args.delay)
        with open('capture.raw', 'wb') as f:
            f.write(data)
        print(f'{len(data) // 4} samples written to {f.name}')

if __name__ == '__main__':
    main()
