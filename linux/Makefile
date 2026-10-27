CFLAGS ?= -O2 -g -Wall -Wextra -Wno-unused-parameter
# micro-ecc: only P-256, portable C (no assembly)
ECC_FLAGS = -DuECC_SUPPORTS_secp160r1=0 -DuECC_SUPPORTS_secp192r1=0 \
	-DuECC_SUPPORTS_secp224r1=0 -DuECC_SUPPORTS_secp256k1=0 -DuECC_PLATFORM=uECC_arch_other
CORE = ../core/mt76.c ../core/dongle.c ../core/gip.c ../core/sha256.c
SRC = main.c platform_libusb.c $(CORE)

xadapter: $(SRC) $(wildcard ../core/*.h) platform_libusb.h uECC.o
	$(CC) $(CFLAGS) -o $@ $(SRC) uECC.o -lusb-1.0

uECC.o: ../third_party/micro-ecc/uECC.c
	$(CC) -O2 $(ECC_FLAGS) -c -o $@ $<

clean:
	rm -f xadapter uECC.o

.PHONY: clean
