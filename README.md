# nRF52840 IQ Capture Tool

A tool using undocumented test features of the nRF52 RADIO core as a cheap one-shot 2.4GHz SDR

### Build

Build the uf2 file using the default `make` target.
This repo targets the nice!nano v2 nRF52840 board and generic clones,
available for a few dollars from the typical sources.

This project is compatible with the nicenano 6.x.x sd140 bootloader. 
This usually ships preflashed on the clones, but can be installed manually by following the 
[instructions here](https://github.com/Nice-Keyboards/nice-keyboards-docs/blob/master/docs/nice!nano/troubleshooting.md). Newer bootloader versions may use a different memory layout which will not work.


### Run

The IQ capture is triggered by a GPIO event plus a configurable delay (up to 4 ms).
After being armed by the host python script, the LED comes on.
A falling edge on the trigger pin (default P0.17) starts a delay timer.
After the configured delay, the status LED goes off and capture begins.
Capture data is saved to `capture.raw`.
The format is interleaved shorts of 12-bit I and Q samples sign extended to 16 bits.
The sample rate is fixed at 16 Msps.

```sh
$ python usb_ctrl.py -c 37 -t -d 100
$ python const_plot.py
```

This will capture on 2402 MHz BLE channel 37 (`-c 37`) starting 100 us (`-d 100`) after a falling edge trigger (`-t`). 
The `const_plot.py` script will show a constellation plot of the captured data.

## Credits

- *[@iracigt](https://github.com/iracigt)* nRF52840 reverse engineering 
- *[@biemster](https://github.com/biemster)* Bare-metal rewrite and USB support
- Using the [nrf52480-baremetal](https://github.com/geky/nrf52480-baremetal) setup from [@geky](https://github.com/geky/)

## License

The capture code and host side scripts are available under the [MIT License](./LICENSE).
The TinyUSB project is also [MIT licensed](./tinyusb/LICENSE).
Vendor libraries in [include](./include/) are provided under their corresponding licenses.
See file headers for details.