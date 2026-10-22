# INF2004 IS29 robot application. Included at the end of pico_rp2040.mk.
#
# Adds the app_program/ include paths and the few Pico SDK 2.2.0 peripheral
# drivers board.c uses (ADC, I2C, timeout helper; GPIO when the radio build
# does not already compile it). The BSP keeps its own startup; only the
# peripheral sources are imported.
#
# Robot profiles (see scripts/dev.sh build):
#   offline  make SMP=0 CONSOLE=uart WIFI=none
#   mqtt     make SMP=0 CONSOLE=uart WIFI=cyw43 NET=lwip ROBOT_MQTT=1

ifneq ($(MAKECMDGOALS),clean)
ifneq ($(SMP),0)
$(error This application uses core-0 IRQ masking and requires SMP=0)
endif
ifneq ($(CONSOLE),uart)
$(error This robot uses the UART0 console on GP0/GP1 (shared with the HC-SR04 after start-up); use CONSOLE=uart)
endif
ifeq ($(wildcard $(PICO_SDK_PATH)/src/rp2_common/hardware_adc/adc.c),)
$(error Set PICO_SDK_PATH to the Pico SDK 2.2.0 checkout)
endif
endif

ROBOT_SDK_INCLUDES := common/pico_base_headers common/pico_binary_info \
    common/pico_time common/hardware_claim \
    rp2040/pico_platform rp2040/hardware_regs rp2040/hardware_structs \
    rp2_common/pico_platform_common rp2_common/pico_platform_compiler \
    rp2_common/pico_platform_sections rp2_common/pico_platform_panic \
    rp2_common/hardware_base rp2_common/hardware_gpio \
    rp2_common/hardware_adc rp2_common/hardware_i2c rp2_common/hardware_pwm \
    rp2_common/hardware_irq rp2_common/hardware_resets \
    rp2_common/hardware_clocks rp2_common/hardware_timer \
    rp2_common/hardware_sync rp2_common/hardware_sync_spin_lock

# Override the BSP's stale 2.1.0 version header with the actual SDK version.
CFLAGS := -I"./robot_sdk_headers" $(CFLAGS)
CFLAGS += -I"../app_program" \
    -I"../app_program/subsystem1" -I"../app_program/subsystem2" \
    -I"../app_program/subsystem3" -I"../app_program/subsystem4" \
    -I"../app_program/subsystem5" \
    -I"../lib/libtm/sysdepend/pico_rp2040/usb" \
    -DPICO_RP2040=1 -DPICO_32BIT=1 -DPICO_ON_DEVICE=1 \
    -DPICO_NO_HARDWARE=0 -DNDEBUG -ffunction-sections -fdata-sections \
    -fstack-usage \
    $(foreach part,$(ROBOT_SDK_INCLUDES),-I"$(PICO_SDK_PATH)/src/$(part)/include")
LFLAGS += -Wl,--gc-sections
LDLIBS += -lm

ROBOT_SDK_OBJS := ./mtkernel_3/robot_sdk/adc.o \
    ./mtkernel_3/robot_sdk/i2c.o \
    ./mtkernel_3/robot_sdk/timeout_helper.o
ifneq ($(WIFI),cyw43)
ROBOT_SDK_OBJS += ./mtkernel_3/robot_sdk/gpio.o
endif
OBJS += $(ROBOT_SDK_OBJS)
C_DEPS += $(ROBOT_SDK_OBJS:.o=.d)

mtkernel_3/robot_sdk/adc.o: $(PICO_SDK_PATH)/src/rp2_common/hardware_adc/adc.c
	@mkdir -p "$(@D)"
	$(GCC) $(CFLAGS) -D$(TARGET) $(INCPATH) -c "$<" -o "$@"

mtkernel_3/robot_sdk/i2c.o: $(PICO_SDK_PATH)/src/rp2_common/hardware_i2c/i2c.c
	@mkdir -p "$(@D)"
	$(GCC) $(CFLAGS) -D$(TARGET) $(INCPATH) -c "$<" -o "$@"

mtkernel_3/robot_sdk/gpio.o: $(PICO_SDK_PATH)/src/rp2_common/hardware_gpio/gpio.c
	@mkdir -p "$(@D)"
	$(GCC) $(CFLAGS) -D$(TARGET) $(INCPATH) -c "$<" -o "$@"

mtkernel_3/robot_sdk/timeout_helper.o: $(PICO_SDK_PATH)/src/common/pico_time/timeout_helper.c
	@mkdir -p "$(@D)"
	$(GCC) $(CFLAGS) -D$(TARGET) $(INCPATH) -c "$<" -o "$@"
