TARGET = nrf52_iqcap

COMPILER_PREFIX = arm-none-eabi-
CC = $(COMPILER_PREFIX)gcc
OBJCOPY = $(COMPILER_PREFIX)objcopy
AR = $(COMPILER_PREFIX)ar
SIZE = $(COMPILER_PREFIX)size
UF2 = uf2conv

SRC += src/startup_nrf52840.s
SRC += src/$(TARGET).c
SRC += src/usb_descriptors.c
SRC += src/rfx.c

SRC += tinyusb/src/tusb.c
SRC += tinyusb/src/common/tusb_fifo.c
SRC += tinyusb/src/device/usbd.c
SRC += tinyusb/src/device/usbd_control.c
SRC += tinyusb/src/class/vendor/vendor_device.c
SRC += tinyusb/src/portable/nordic/nrf5x/dcd_nrf5x.c

INC += include
INC += tinyusb/src

LIB += m
LIB += c
LIB += gcc
LIB += nosys

LDSCRIPT += src/nrf52840.ld

OBJ += $(patsubst %.S,%.o,$(patsubst %.s,%.o,$(patsubst %.c,%.o,$(SRC))))
DEP += $(patsubst %.o,%.d,$(OBJ))

ifdef DEBUG
CFLAGS += -DDEBUG
CFLAGS += -g
CFLAGS += -O0
else
# -DNDEBUG disables asserts which usually isn't what we want
#CFLAGS += -DNDEBUG
CFLAGS += -Os
CFLAGS += -flto
endif

CFLAGS += -DNRF52840_XXAA

CFLAGS += -mthumb
CFLAGS += -mcpu=cortex-m4
CFLAGS += -mfpu=fpv4-sp-d16
CFLAGS += -mfloat-abi=softfp
CFLAGS += -std=c99
CFLAGS += -Wall -Wno-format
CFLAGS += -fno-common
CFLAGS += -ffunction-sections
CFLAGS += -fdata-sections
CFLAGS += -ffreestanding
CFLAGS += -fno-builtin
CFLAGS += -MMD -MP
CFLAGS += $(patsubst %,-I%,$(INC))

ASMFLAGS += $(CFLAGS)

LDFLAGS += $(CFLAGS)
LDFLAGS += -T$(LDSCRIPT)
LDFLAGS += -static
LDFLAGS += --specs=nano.specs
LDFLAGS += --specs=nosys.specs
LDFLAGS += -Wl,--gc-sections
LDFLAGS += -Wl,-static
LDFLAGS += -Wl,-z -Wl,muldefs
LDFLAGS += -Wl,--start-group $(patsubst %,-l%,$(LIB)) -Wl,--end-group

.PHONY: all flash
all: build size uf2

flash: all
	python usb_ctrl.py -b
	sleep 3
	udisksctl mount -b /dev/sd? && cp nrf52_iqcap.uf2 /run/media/*/NICENANO

.PHONY: build
build: $(TARGET).elf

.PHONY: uf2
uf2: $(TARGET).uf2

.PHONY: size
size: $(TARGET).elf
	$(SIZE) $<

.PHONY: tags
tags:
	ctags $(SRC) $$(find $(INC) -name '*.h')

-include $(DEP)

$(TARGET).elf: $(OBJ) $(LDSCRIPT)
	$(CC) $(OBJ) $(LDFLAGS) -o $@

%.o: %.c
	$(CC) -c -MMD $(CFLAGS) $< -o $@

%.s: %.c
	$(CC) -S -MMD $(CFLAGS) $< -o $@

%.o: %.s
	$(CC) -c -MMD $(ASMFLAGS) $< -o $@

%.o: %.S
	$(CC) -c -MMD $(ASMFLAGS) $< -o $@

%.bin: %.elf
	$(OBJCOPY) -O binary $< $@

%.uf2: %.bin
	$(UF2) $< -c -f 0xADA52840 -b 0x26000 -o $@

clean:
	rm -f $(TARGET).elf
	rm -f $(OBJ)
	rm -f $(DEP)
