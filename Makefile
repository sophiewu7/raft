# Compiler and flags
CXX = g++
CXXFLAGS = -std=c++17 -Wall

# Targets
all: process stopall clean

# Compile process
process: process.o
	$(CXX) $(CXXFLAGS) -o process process.o

process.o: process.cpp
	$(CXX) $(CXXFLAGS) -c process.cpp

# Compile stopall
stopall: stopall.o
	$(CXX) $(CXXFLAGS) -o stopall stopall.o

stopall.o: stopall.cpp
	$(CXX) $(CXXFLAGS) -c stopall.cpp

# Clean up
clean:
	rm -f process.o stopall.o

submit-clean:
	rm -f process.o stopall.o process stopall