CXX = g++
CXXSRC = $(wildcard *.cpp)
CXXINCLUDE = -I./include -I./lib/include/
CXXFLAGS = -O3 -march=native -funroll-loops -mtune=native -std=c++20 -flto
SPIKE ?= 0
SOFTFLOAT_LIB ?= ./lib/softfloat/softfloat.a

LDFLAGS = -lz -Wl,-rpath,./lib
ifeq ($(SPIKE),1)
  CXXFLAGS += -DENABLE_SPIKE
  LDFLAGS += -L./lib -lriscv -lfesvr
else
  ifeq ($(wildcard $(SOFTFLOAT_LIB)),)
    $(error Missing softfloat archive: $(SOFTFLOAT_LIB))
  endif
  CXXFLAGS += -DUSE_SIMULATOR_SOFTFLOAT
  CXXINCLUDE += -I./lib/softfloat/include
  LDFLAGS += $(SOFTFLOAT_LIB)
endif
GDB_FLAGS = -g -march=native

TARGET = a.out
IMG = ./baremetal/memory

all: $(TARGET)

$(TARGET): $(CXXSRC) $(wildcard include/*.h)
	$(CXX) $(CXXINCLUDE) $(CXXFLAGS) $(CXXSRC) -o $@ $(LDFLAGS)

run: $(TARGET)
	./$(TARGET) $(IMG)

with_spike:
	$(MAKE) SPIKE=1 TARGET=a.out

no_spike:
	$(MAKE) SPIKE=0 TARGET=a.out.nospike

clean:
	rm -f a.out a.out.nospike

gdb: $(CXXSRC)
	$(CXX) $(CXXINCLUDE) $(GDB_FLAGS) $^ -o $(TARGET) $(LDFLAGS)
	gdb --args ./$(TARGET) $(IMG)

.PHONY: all clean run gdb with_spike no_spike
