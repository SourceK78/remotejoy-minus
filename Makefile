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
# POPS GunCon emulation: while the controller firmware sends gyro aim, POPS's
# controller port 1 is presented as a GunCon (see pops_aim.c). Disabled by
# default.
RJM_ENABLE_POPS_AIM ?= 0

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
ifneq ($(RJM_ENABLE_POPS_AIM),0)
CFLAGS += -DRJM_ENABLE_POPS_AIM=1
OBJS += pops_aim.o
endif
# Where POPS draws the PS1 picture, for the GunCon crosshair overlay.
# Defaults match POPS's "Original" screen mode (320x240 at 80,16). On TV
# out the picture is assumed scaled to fit; a measured rectangle can be set
# with RJM_POPS_AIM_SCREEN="RJM_POPS_AIM_TV_X=.. RJM_POPS_AIM_TV_Y=.. ...".
# The TV out screen size (16:9 / 4:3) is read from the registry; add
# RJM_POPS_AIM_TV_4_3=1 (or =0) to RJM_POPS_AIM_SCREEN to force it.
ifneq ($(RJM_POPS_AIM_SCREEN),)
CFLAGS += $(foreach v,$(RJM_POPS_AIM_SCREEN),-D$(v))
endif
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = $(CFLAGS)
LIBDIR =
LIBS = -lpspusb_driver -lpspreg_driver -lpspusbbus_driver -lpspctrl_driver -lpsppower_driver -lpspsystemctrl_kernel

PSPSDK=$(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build_prx.mak

# pops_aim.o is only in OBJS for aim builds, so clean removes it here. (Not
# through EXTRA_CLEAN: build_prx.mak makes those prerequisites of clean, so
# it would be rebuilt with the clean run's flags instead of removed.)
clean: clean-pops-aim
.PHONY: clean-pops-aim
clean-pops-aim:
	-rm -f pops_aim.o
