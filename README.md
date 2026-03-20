## Baremetal NRF52840 example

This is a minimal example of a baremetal NRF52840 project with raw IQ capture sent over USB Bulk

It has vendored code from three libraries
- [CMSIS][CMSIS] - Provided by ARM
- [nrfx][nrfx] - Provided by Nordic
- [TinyUSB][tinyusb] - Provided by hathach (tinyusb.org)

## Usage

This includes a Makefile that compiles using `arm-none-eabi-gcc`:

``` bash
$ make
```

## Notes

- This is a very minimal example.
- nrfx provides drivers for Nordic's peripherals, while CMSIS provides useful
  intrinsics for ARM cores.

[CMSIS]: https://github.com/ARM-software/CMSIS_5
[nrfx]: https://github.com/NordicSemiconductor/nrfx
[tinyusb]: https://github.com/hathach/tinyusb
