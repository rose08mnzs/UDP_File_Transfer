CXX = g++
CXXFLAGS = -std=c++17 -O2
THREAD_FLAGS = -pthread

.PHONY: all clean

all: client server

client: client.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(THREAD_FLAGS) $< -o $@ $(LDFLAGS) $(LDLIBS)

server: server.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(THREAD_FLAGS) $< -o $@ $(LDFLAGS) $(LDLIBS)

clean:
	rm -f client server
