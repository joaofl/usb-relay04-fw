PORT ?= /dev/ttyACM0
RELAY ?= /dev/ttyUSB0
BOARD ?= icse012a
SDCC = sdcc -mstm8 --std-c99 --opt-code-size
B = build/$(BOARD)
BOARD_FLAGS = -Ifirmware '-DBOARD_H="boards/$(BOARD).h"'
DEPS = firmware/stm8s.h firmware/board.h firmware/boards/$(BOARD).h
BOARDS = $(patsubst firmware/boards/%.h,%,$(wildcard firmware/boards/*.h))
VERSION = $(shell sed -n 's/^\#define VERSION "\(.*\)"/\1/p' firmware/relay04.c)

all: $(B)/boot.ihx $(B)/relay04.ihx $(B)/relay04-full.ihx

boards:
	@echo $(BOARDS)

version:
	@echo $(VERSION)

# Release files for every board: relay04-<version>-<board>{,-full}.ihx, relaytest, relayctl.py, SHA256SUMS.
dist:
	rm -rf dist && mkdir -p dist
	for b in $(BOARDS); do \
		$(MAKE) --no-print-directory BOARD=$$b all build/$$b/relaytest.ihx || exit 1; \
		cp build/$$b/relay04.ihx dist/relay04-$(VERSION)-$$b.ihx; \
		cp build/$$b/relay04-full.ihx dist/relay04-$(VERSION)-$$b-full.ihx; \
		cp build/$$b/relaytest.ihx dist/relaytest-$(VERSION)-$$b.ihx; \
	done
	cp tools/relayctl.py dist/
	cd dist && sha256sum * > SHA256SUMS

$(B)/boot.ihx: firmware/boot.c firmware/stm8s.h
	@mkdir -p $(B)
	$(SDCC) -o $(B)/ $<
	@python3 tools/relayctl.py size $@ 0x8000 0x8800

$(B)/relay04.ihx: firmware/relay04.c $(DEPS)
	@mkdir -p $(B)
	$(SDCC) $(BOARD_FLAGS) --code-loc 0x8800 -o $(B)/ $<
	@python3 tools/relayctl.py size $@ 0x8800 0xA000

$(B)/relaytest.ihx: firmware/relaytest.c $(DEPS)
	@mkdir -p $(B)
	$(SDCC) $(BOARD_FLAGS) -o $(B)/ $<

# Bootloader and application in one image, for the first install over SWIM.
$(B)/relay04-full.ihx: $(B)/boot.ihx $(B)/relay04.ihx
	python3 tools/relayctl.py merge $@ $^

swim-flash: $(B)/relay04-full.ihx
	python3 tools/swim-flash.py --port $(PORT) $<

swim-flash-relaytest: $(B)/relaytest.ihx
	python3 tools/swim-flash.py --port $(PORT) $<

update: $(B)/relay04.ihx
	python3 tools/relayctl.py -d $(RELAY) update $<

clean:
	rm -rf build dist

.PHONY: all boards version dist swim-flash swim-flash-relaytest update clean
