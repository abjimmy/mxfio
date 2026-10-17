# External fio engine; MACA cu-bridge compiles the original Tutti files.
FIO_ROOT ?= ../..
MACA_PATH ?= /opt/maca
CUCC ?= $(MACA_PATH)/tools/cu-bridge/bin/cucc
CC ?= gcc
BUILD ?= build
GIDS_RDMA ?= off
ifeq ($(GIDS_RDMA),off)
GIDS_HAVE_MLX5 := 0
else ifeq ($(GIDS_RDMA),mlx5)
GIDS_HAVE_MLX5 := 1
else
$(error GIDS_RDMA must be off or mlx5)
endif
GIDS_CONFIG := $(BUILD)/gids_config.h
GIDS_CPPFLAGS := -include $(GIDS_CONFIG)
V := vendor/Tutti
NVM := $(V)/csrc/device_manager/nvme/libnvm
IO := $(V)/csrc/data_paths/local_nvme/io
CPPFLAGS += -I$(V) -I$(V)/csrc/include -I$(V)/csrc/include/uapi -I$(NVM)/include -I$(NVM)/src -DTUTTI_USE_MACA=1
CXXFLAGS := -O2 -g -std=gnu++17 -fPIC -pthread
HOST_CPPFLAGS := -I$(MACA_PATH)/tools/cu-bridge/include -I$(MACA_PATH)/include -I$(MACA_PATH)/include/mcr
NVM_SRC := $(wildcard $(NVM)/src/*.cpp $(NVM)/src/linux/*.cpp)
OBJ := $(patsubst %.cpp,$(BUILD)/%.o,$(NVM_SRC))
OBJ += $(BUILD)/adapter.o $(BUILD)/pcie.o
OBJ += $(BUILD)/pcie_batch.o $(BUILD)/pcie_profile.o $(BUILD)/profile_clock.o
OBJ += $(BUILD)/device_target.o $(BUILD)/nvme_queue_group.o $(BUILD)/fio_gids.o
LIBS := -L$(MACA_PATH)/lib -Wl,-rpath,$(MACA_PATH)/lib -lmcruntime -lruntime_cu -lmxc-runtime64 -pthread
ifeq ($(GIDS_RDMA),mlx5)
OBJ += $(BUILD)/rdma.o $(BUILD)/rdma_gpu.o
LIBS += -libverbs -lrdmacm -lmlx5
endif
.PHONY: all clean test module FORCE
all: $(BUILD)/gids.so
$(BUILD)/gids.so: $(OBJ)
	$(CUCC) -shared -o $@ $^ $(LIBS)
# Update only when the selection changes, including mlx5 -> off. All
# objects then rebuild and the plugin relinks without stale RDMA objects.
$(GIDS_CONFIG): FORCE
	@mkdir -p $(BUILD)
	@printf '#define GIDS_HAVE_MLX5 %s\n' '$(GIDS_HAVE_MLX5)' > $@.tmp
	@cmp -s $@.tmp $@ && rm -f $@.tmp || mv -f $@.tmp $@
FORCE:
$(BUILD)/fio_gids.o: fio_gids.c adapter.h pcie_config.h $(FIO_ROOT)/config-host.h
	@mkdir -p $(@D)
	$(CC) $(GIDS_CPPFLAGS) -O2 -g -Wall -Wextra -Wno-unused-parameter -fPIC -D_GNU_SOURCE -DFIO_INTERNAL -I$(FIO_ROOT) -include $(FIO_ROOT)/config-host.h -c $< -o $@
$(BUILD)/%.o: %.cpp
	@mkdir -p $(@D)
	$(CXX) $(GIDS_CPPFLAGS) $(CPPFLAGS) $(HOST_CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@
$(BUILD)/pcie.o: pcie.cu
	@mkdir -p $(@D)
	$(CXX) $(GIDS_CPPFLAGS) $(CPPFLAGS) $(HOST_CPPFLAGS) $(CXXFLAGS) -MMD -MP -x c++ -c $< -o $@
$(BUILD)/profile_include/nvm_parallel_queue.h: $(NVM)/include/nvm_parallel_queue.h scripts/profile_tutti.py
	python3 scripts/profile_tutti.py $< $@
$(BUILD)/pcie_profile.o: pcie_profile.cu $(BUILD)/profile_include/nvm_parallel_queue.h
	$(CUCC) $(GIDS_CPPFLAGS) -I$(BUILD)/profile_include -I. $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@
$(BUILD)/%.o: %.cu
	@mkdir -p $(@D)
	$(CUCC) $(GIDS_CPPFLAGS) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@
$(BUILD)/%.o: $(IO)/%.cu
	@mkdir -p $(@D)
	$(CUCC) $(GIDS_CPPFLAGS) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@
module:
	bash ./build-module.sh
test:
	@mkdir -p $(BUILD)
	$(CXX) -std=c++17 -Wall -Wextra -Werror tests/completion_test.cpp -o $(BUILD)/completion_test
	$(BUILD)/completion_test
	$(CC) -O2 -g -Wall -Wextra -Werror -Wno-unused-parameter -D_GNU_SOURCE -DFIO_INTERNAL -DGIDS_HAVE_MLX5=0 -I$(FIO_ROOT) -include $(FIO_ROOT)/config-host.h tests/scheduler_test.c -o $(BUILD)/scheduler_test
	$(BUILD)/scheduler_test
	$(CXX) -std=c++17 -Wall -Wextra -Werror tests/pcie_batch_test.cpp -o $(BUILD)/pcie_batch_test
	$(BUILD)/pcie_batch_test
	$(CXX) -std=c++17 -Wall -Wextra -Werror tests/pcie_plan_test.cpp -o $(BUILD)/pcie_plan_test
	$(BUILD)/pcie_plan_test
	$(CXX) -std=c++17 -Wall -Wextra -Werror -pthread tests/worker_pool_test.cpp -o $(BUILD)/worker_pool_test
	$(BUILD)/worker_pool_test
	$(CXX) -std=c++17 -O2 -Wall -Wextra -Werror -Wno-unknown-pragmas -Wno-unused-parameter -Itests/queue_mock -I$(V) -I$(NVM)/include tests/shared_queue_test.cpp -o $(BUILD)/shared_queue_test
	$(BUILD)/shared_queue_test
	$(CXX) -std=c++17 -Wall -Wextra -Werror tests/rdma_batch_test.cpp -o $(BUILD)/rdma_batch_test
	$(BUILD)/rdma_batch_test
	$(CXX) -std=c++17 -Wall -Wextra -Werror tests/wire_test.cpp -o $(BUILD)/wire_test
	$(BUILD)/wire_test
	$(CXX) -std=c++17 -Wall -Wextra -Werror tests/ap_plan_test.cpp -o $(BUILD)/ap_plan_test
	$(BUILD)/ap_plan_test
	$(CXX) -std=c++17 -Wall -Wextra -Werror -pthread tests/pcie_owner_test.cpp -o $(BUILD)/pcie_owner_test
	$(BUILD)/pcie_owner_test
	python3 scripts/check_vendor.py
	python3 tests/config_test.py
	python3 tests/profile_test.py
	python3 tests/baseline_test.py
	python3 tests/baseline_command_test.py
clean:
	rm -rf -- $(BUILD)
$(OBJ): $(GIDS_CONFIG)
-include $(OBJ:.o=.d)
