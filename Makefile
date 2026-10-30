PORT ?= /dev/ttyACM0
RELAY ?= /dev/ttyUSB0
SDCC = sdcc -mstm8 --std-c99 --opt-code-size
B = build

all: $(B)/boot.ihx $(B)/relay04.ihx $(B)/relay04-full.ihx

$(B)/boot.ihx: firmware/boot.c firmware/stm8s.h
	@mkdir -p $(B)
	$(SDCC) -o $(B)/ $<
	@python3 tools/relayctl.py size $@ 0x8000 0x8800

$(B)/relay04.ihx: firmware/relay04.c firmware/stm8s.h
	@mkdir -p $(B)
	$(SDCC) --code-loc 0x8800 -o $(B)/ $<
	@python3 tools/relayctl.py size $@ 0x8800 0xA000

$(B)/relaytest.ihx: firmware/relaytest.c
	@mkdir -p $(B)
	$(SDCC) -o $(B)/ $<

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
	rm -rf $(B)

.PHONY: all swim-flash swim-flash-relaytest update clean
