# name for the executable file
EXE := main

# important special files
MAIN_FILE := main.cpp

# compiler to use
CXX := clang++

# compiler flags
CXX_FLAGS := -std=c++14
CXX_FLAGS += -Wall
CXX_FLAGS += -Wextra
CXX_FLAGS += -O0
# CXX_FLAGS += -march=native
# CXX_FLAGS += -DNDEBUG
CXX_FLAGS += -pipe
CXX_FLAGS += -g
CXX_FLAGS += -rdynamic

# llvm flags
LLVM_FLAGS := `llvm-config --cppflags --ldflags`

# llvm libraries
LLVM_LIBS := `llvm-config --system-libs --libs all`

# definition of targets
.PHONY: clean

all:
	$(CXX) $(CXX_FLAGS) $(CLANG_FLAGS) $(LLVM_FLAGS) *.cpp $(LLVM_LIBS) $(LIBS) -o $(EXE)

clean:
	rm -f $(EXE)
