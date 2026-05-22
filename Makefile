CXX = g++
CXXSRC = $(wildcard *.cpp)
LIBSRC = exec.cpp simpoint.cpp refcpu_api.cpp
LIBHDR = $(wildcard include/*.h) $(wildcard include/api/*.h)
CXXINCLUDE = -I./include -I./include/api -I./lib/include/
CXXFLAGS = -O3 -march=native -funroll-loops -mtune=native -std=c++20 -flto
CXXFLAGS += -MMD -MP
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
REFCPU_LIB = librefcpu.a
TARGET_OBJS = $(CXXSRC:.cpp=.o)
LIBOBJS = $(LIBSRC:.cpp=.o)
DEPS = $(TARGET_OBJS:.o=.d)
IMG = ./baremetal/memory

all: $(TARGET)

$(REFCPU_LIB): $(LIBOBJS)
	ar rcs $@ $(LIBOBJS)

%.o: %.cpp Makefile $(LIBHDR)
	$(CXX) $(CXXINCLUDE) $(CXXFLAGS) -c $< -o $@

$(TARGET): $(TARGET_OBJS)
	$(CXX) $(CXXINCLUDE) $(CXXFLAGS) $(TARGET_OBJS) -o $@ $(LDFLAGS)

run: $(TARGET)
	./$(TARGET) $(IMG)

with_spike:
	$(MAKE) SPIKE=1 TARGET=a.out

no_spike:
	$(MAKE) SPIKE=0 TARGET=a.out.nospike

gdb: $(CXXSRC)
	$(CXX) $(CXXINCLUDE) $(GDB_FLAGS) $^ -o $(TARGET) $(LDFLAGS)
	gdb --args ./$(TARGET) $(IMG)

clean:
	rm -f a.out a.out.nospike $(REFCPU_LIB) *.o *.d

-include $(DEPS)

.PHONY: all clean run gdb with_spike no_spike
