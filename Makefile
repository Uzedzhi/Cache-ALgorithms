CXX := g++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Iinclude -IMyCppLibs

SRC := $(wildcard src/*.cpp)
OBJ := $(SRC:.cpp=.o)
TARGET := cache_sim

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJ)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET) src/*.o
