TARGET = remotejoy-minus
OBJS = main.o remotejoy_minus.o rjm_log.o kmode.o impose_stub.o exports.o

USE_KERNEL_LIBS = 1
RJM_REBOOT_LOAD_POPS ?= 1

INCDIR =
CFLAGS = -Os -mno-gpopt -Wall -fno-builtin-printf
ifneq ($(RJM_ENABLE_LOG),)
CFLAGS += -DRJM_ENABLE_LOG=1
endif
ifneq ($(RJM_REBOOT_LOAD_POPS),0)
CFLAGS += -DRJM_REBOOT_LOAD_POPS=1
endif
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = $(CFLAGS)
LIBDIR =
LIBS = -lpspusb_driver -lpspusbbus_driver -lpspctrl_driver -lpsppower_driver -lpspsystemctrl_kernel

PSPSDK=$(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build_prx.mak
