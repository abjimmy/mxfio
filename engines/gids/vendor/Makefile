# Standalone vendor validation: available before the fio engine is introduced.
MACA_PATH ?= /opt/maca
CUCC ?= $(MACA_PATH)/tools/cu-bridge/bin/cucc
CXX ?= g++
AR ?= ar
BUILD ?= build
V := Tutti
NVM := $(V)/csrc/device_manager/nvme/libnvm
IO := $(V)/csrc/data_paths/local_nvme/io
INCLUDES := -I$(V) -I$(V)/csrc/include -I$(V)/csrc/include/uapi -I$(NVM)/include -I$(NVM)/src
HOST_INCLUDES := -I$(MACA_PATH)/tools/cu-bridge/include -I$(MACA_PATH)/include -I$(MACA_PATH)/include/mcr
FLAGS := -O2 -g -std=gnu++17 -pthread -DTUTTI_USE_MACA=1
LIBS := -L$(MACA_PATH)/lib -Wl,-rpath,$(MACA_PATH)/lib -lmcruntime -lruntime_cu -lmxc-runtime64
HOST_SRC := $(wildcard $(NVM)/src/*.cpp $(NVM)/src/linux/*.cpp)
GPU_SRC := $(IO)/submit_one.cu $(IO)/device_target.cu $(IO)/nvme_queue_group.cu
OBJ := $(patsubst %.cpp,$(BUILD)/%.o,$(HOST_SRC)) $(patsubst %.cu,$(BUILD)/%.o,$(GPU_SRC))

.PHONY: all test module
all: $(BUILD)/vendor_link_check $(BUILD)/maca_timer_compile.o
$(BUILD)/%.o: %.cpp
	@mkdir -p $(@D)
	$(CXX) $(INCLUDES) $(HOST_INCLUDES) $(FLAGS) -MMD -MP -c $< -o $@
$(BUILD)/%.o: %.cu
	@mkdir -p $(@D)
	$(CUCC) $(INCLUDES) $(FLAGS) -MMD -MP -c $< -o $@
$(BUILD)/libtutti_nvme.a: $(OBJ)
	$(AR) rcs $@ $^
$(BUILD)/vendor_link_check: $(BUILD)/libtutti_nvme.a $(BUILD)/tests/host_headers.o
	$(CUCC) -pthread $(BUILD)/tests/host_headers.o -Wl,--whole-archive $< -Wl,--no-whole-archive $(LIBS) -o $@
$(BUILD)/maca_timer_compile.o: tests/maca_timer_compile.cu
	@mkdir -p $(@D)
	$(CUCC) $(INCLUDES) $(FLAGS) -MMD -MP -c $< -o $@
$(BUILD)/maca_clock_contract: tests/maca_clock_contract.cpp
	@mkdir -p $(@D)
	$(CXX) -std=c++17 -Wall -Wextra -Werror -MMD -MP -I$(V)/csrc/include $< -o $@
test: all $(BUILD)/maca_clock_contract
	python3 ../scripts/check_vendor.py
	$(BUILD)/vendor_link_check
	$(BUILD)/maca_clock_contract
module:
	bash ./build-module.sh "$(abspath $(BUILD))/module"

-include $(OBJ:.o=.d) $(BUILD)/tests/host_headers.d \
	$(BUILD)/maca_timer_compile.d $(BUILD)/maca_clock_contract.d
