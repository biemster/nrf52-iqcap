#!/usr/bin/env python
import argparse
import usb.core
import usb.util
from time import sleep

NRF_USB_EP_IN        = 0x81      # endpoint for data transfer in
NRF_USB_EP_OUT       = 0x01      # endpoint for command transfer out
NRF_USB_PACKET_SIZE  = 256       # packet size
NRF_USB_TIMEOUT_MS   = 100       # timeout for normal USB operations

NRF_CMD_USBTEST      = 0xa1
NRF_CMD_REBOOT       = 0xa2
NRF_CMD_IQCAPTURE    = 0xca
NRF_STR_USBTEST      = (NRF_CMD_USBTEST, 0x01, 0x00, 0x01)
NRF_STR_REBOOT       = (NRF_CMD_REBOOT, 0x01, 0x00, 0x01)
NRF_STR_IQCAPTURE    = (NRF_CMD_IQCAPTURE, 0x00, 0x00, 0x01)

device = usb.core.find(idVendor=0xcafe, idProduct=0x4000)
device.set_configuration()

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('-b', '--bootloader', help='Reboot to bootloader', action='store_true')
    parser.add_argument('-c', '--channel', help='start IQ capture on BLE channel')
    parser.add_argument('-f', '--frequency', help='start IQ capture on frequency (MHz)')
    parser.add_argument('-u', '--usbtest', help='Send test command over USB to blink the LED', action='store_true')
    args = parser.parse_args()

    if device is None:
        print("nRF52 not found")
        exit(0)

    # Clear any pending data in the pipes
    try:
        device.set_configuration() # gets tud_mount_cb called on the mcu
        device.clear_halt(NRF_USB_EP_IN)  # reset DATA0/1 toggle
        device.clear_halt(NRF_USB_EP_OUT) # reset DATA0/1 toggle
        device.read(NRF_USB_EP_IN, NRF_USB_PACKET_SIZE, 10)
    except:
        pass

    if args.bootloader:
        print('rebooting to bootloader')
        try:
            device.write(NRF_USB_EP_OUT, NRF_STR_REBOOT)
            sleep(.3)
        except Exception as e:
            print(f"Command sent (device may have reset already): {e}")
    elif args.usbtest:
        print('USB test, blinking the LED')
        try:
            device.write(NRF_USB_EP_OUT, NRF_STR_USBTEST)
        except Exception as e:
            print(f"Exception: {e}")
    elif args.channel or args.frequency:
        if args.channel:
            ble_frequencies = [
                2404, 2406, 2408, 2410, 2412, 2414, 2416, 2418, 2420, 2422, 2424, # 0-10
                2428, 2430, 2432, 2434, 2436, 2438,                               # 11-16
                2440, 2442, 2444, 2446, 2448, 2450, 2452, 2454, 2456, 2458, 2460, # 17-27
                2462, 2464, 2466, 2468, 2470, 2472, 2474, 2476, 2478,             # 28-36
                2402, 2426, 2480                                                  # 37, 38, 39
            ]
            freq = ble_frequencies[int(args.channel)]
            print(f'Starting capture on channel {args.channel} (={freq}MHz)')
            cmd = bytearray(NRF_STR_IQCAPTURE)
            cmd[1] = freq - 2400
            try:
                device.write(NRF_USB_EP_OUT, cmd)
            except Exception as e:
                print(f"Exception: {e}")
        elif args.frequency:
            print(f'Starting capture on {args.frequency}MHz')
            cmd = bytearray(NRF_STR_IQCAPTURE)
            cmd[1] = int(args.frequency) - 2400
            try:
                device.write(NRF_USB_EP_OUT, cmd)
            except Exception as e:
                print(f"Exception: {e}")

        # wait for capture to arrive on IN endpoint
        with open('capture.raw', 'wb') as f:
            while True:
                try:
                    data = device.read(NRF_USB_EP_IN, 1024, timeout=NRF_USB_TIMEOUT_MS)
                    f.write(data)
                except usb.core.USBError as e:
                    # Handle timeout/disconnects
                    break    
    print('done')

if __name__ == '__main__':
    main()
