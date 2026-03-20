#!/usr/bin/env python
import argparse
import usb.core
import usb.util
from time import sleep

NRF_USB_EP_IN        = 0x81      # endpoint for data transfer in
NRF_USB_EP_OUT       = 0x01      # endpoint for command transfer out
NRF_USB_PACKET_SIZE  = 256       # packet size
NRF_USB_TIMEOUT_MS   = 100       # timeout for normal USB operations

NRF_CMD_REBOOT       = 0xa2
NRF_STR_REBOOT       = (NRF_CMD_REBOOT, 0x01, 0x00, 0x01)

device = usb.core.find(idVendor=0xcafe, idProduct=0x4000)
device.set_configuration()

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('-b', '--bootloader', help='Reboot to bootloader', action='store_true')
    args = parser.parse_args()

    if device is None:
        print("nRF52 not found")
        exit(0)

    # Clear any pending data in the pipes
    try:
        device.read(CH_USB_EP_IN, NRF_USB_PACKET_SIZE, 10)
    except:
        pass

    if args.bootloader:
        print('rebooting to bootloader')
        try:
            device.write(NRF_USB_EP_OUT, NRF_STR_REBOOT)
            sleep(.3)
        except Exception as e:
            print(f"Command sent (device may have reset already): {e}")
    
    print('done')

def capture():
    with open('capture.raw', 'wb') as f:
        while True:
            try:
                # Read from Bulk IN endpoint (e.g., 0x81)
                data = device.read(NRF_USB_EP_IN, 1024, timeout=NRF_USB_TIMEOUT_MS)
                f.write(data)
            except usb.core.USBError as e:
                # Handle timeout/disconnects
                break

if __name__ == '__main__':
    main()
