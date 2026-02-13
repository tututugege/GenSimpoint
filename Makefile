CXX = g++
CXXSRC = $(wildcard *.cpp)
CXXINCLUDE = -I./include -I/home/tututu/riscv-tools/include
CXXFLAGS = -O3 -march=native -funroll-loops -mtune=native -std=c++20
LDFLAGS = -lz -L/home/tututu/riscv-tools/lib -lriscv -lfesvr -Wl,-rpath,/home/tututu/riscv-tools/lib
GDB_FLAGS = -g -march=native

TARGET = a.out
IMG = ./baremetal/memory

all: $(TARGET)

$(TARGET): $(CXXSRC) $(wildcard include/*.h)
	$(CXX) $(CXXINCLUDE) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

run: $(TARGET)
	./$(TARGET) $(IMG)

clean:
	rm -f $(TARGET)

gdb: $(CXXSRC)
	$(CXX) $(CXXINCLUDE) $(GDB_FLAGS) $^ -o $(TARGET) $(LDFLAGS)
	gdb --args ./$(TARGET) $(IMG)

.PHONY: all clean run gdb
