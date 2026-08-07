TARGET = remotejoy-minus
OBJS = main.o remotejoy_minus.o rjm_log.o kmode.o pops_hook.o impose_stub.o exports.o

USE_KERNEL_LIBS = 1
RJM_REBOOT_LOAD_POPS ?= 1
# POPS 2P support patches POPS's controller gate and feeds RP2040 multitap
# slot 2 through the existing sceCtrl hook. Keep disabled by default so the
# normal VSH/GAME-only build stays unchanged unless explicitly requested.
RJM_ENABLE_POPS_2P ?= 0
RJM_POPS_GATE_SLOT2_RETURN ?= 4
RJM_POPS_GATE_FORCE_SLOT2_ASM ?= 0

INCDIR =
CFLAGS = -Os -mno-gpopt -Wall -fno-builtin-printf
ifneq ($(RJM_ENABLE_LOG),)
CFLAGS += -DRJM_ENABLE_LOG=1
endif
ifneq ($(RJM_REBOOT_LOAD_POPS),0)
CFLAGS += -DRJM_REBOOT_LOAD_POPS=1
endif
ifneq ($(RJM_ENABLE_POPS_2P),0)
CFLAGS += -DRJM_ENABLE_POPS_2P=1
CFLAGS += -DRJM_POPS_GATE_SLOT2_RETURN=$(RJM_POPS_GATE_SLOT2_RETURN)
ifneq ($(RJM_POPS_GATE_FORCE_SLOT2_ASM),0)
CFLAGS += -DRJM_POPS_GATE_FORCE_SLOT2_ASM=1
endif
endif
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = $(CFLAGS)
LIBDIR =
LIBS = -lpspusb_driver -lpspusbbus_driver -lpspctrl_driver -lpsppower_driver -lpspsystemctrl_kernel

PSPSDK=$(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build_prx.mak
