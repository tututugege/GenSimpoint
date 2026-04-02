CXX = g++
CXXSRC = $(wildcard *.cpp)
CXXINCLUDE = -I./include -I./lib/include/
CXXFLAGS = -O3 -march=native -funroll-loops -mtune=native -std=c++20 -flto
LDFLAGS = -lz -L./lib -lriscv -lfesvr -Wl,-rpath,./lib
GDB_FLAGS = -g -march=native

TARGET = a.out
IMG = ./baremetal/memory

all: $(TARGET)

$(TARGET): $(CXXSRC) $(wildcard include/*.h)
	$(CXX) $(CXXINCLUDE) $(CXXFLAGS) $(CXXSRC) -o $@ $(LDFLAGS)

run: $(TARGET)
	./$(TARGET) $(IMG)

clean:
	rm -f $(TARGET)

gdb: $(CXXSRC)
	$(CXX) $(CXXINCLUDE) $(GDB_FLAGS) $^ -o $(TARGET) $(LDFLAGS)
	gdb --args ./$(TARGET) $(IMG)

.PHONY: all clean run gdb
