.DEFAULT_GOAL := temper

CPPFLAGS := $(shell pkg-config --cflags libusb-1.0)
CXXFLAGS := -std=c++14 -Wpedantic -Wall -Wextra -O2
LDFLAGS := $(shell pkg-config --libs libusb-1.0)
