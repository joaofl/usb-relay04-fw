PORT ?= /dev/ttyACM0
RELAY ?= /dev/ttyUSB0
BOARD ?= icse012a
SDCC = sdcc -mstm8 --std-c99 --opt-code-size
B = build/$(BOARD)
BOARD_FLAGS = -Ifirmware '-DBOARD_H="boards/$(BOARD).h"'
DEPS = firmware/stm8s.h firmware/board.h firmware/boards/$(BOARD).h

all: $(B)/boot.ihx $(B)/relay04.ihx $(B)/relay04-full.ihx

boards:
	@ls firmware/boards | sed 's/\.h$$//'

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
	rm -rf build

.PHONY: all boards swim-flash swim-flash-relaytest update clean
