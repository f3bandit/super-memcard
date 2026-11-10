# The virtual card: sd2psXtd's PS1 card code, unchanged, built for the PC.
#   ./get-firmware.sh && make test
FW      ?= firmware
# SMC=1: with Super MemCard (../../firmware applied to a copy of the firmware), in build-smc
ifeq ($(SMC),1)
B       ?= build-smc
FWSRC   = $(B)/fw
DEFS    = -DWITH_SUPERMEMCARD=1
else
FWSRC   = $(FW)
endif
CC      ?= cc
OPT     ?= -O2 -g
# SAN=address (or thread, undefined) for a sanitizer build, in its own build folder
ifdef SAN
OPT     += -fsanitize=$(SAN) -fno-omit-frame-pointer
B       ?= build-$(SAN)
endif
# _FORTIFY_SOURCE off: glibc would abort on game_db.c's strlcpy(idString, split, 11) into a
# 10-byte buffer, a bug in the firmware that only overflows for IDs of 10+ digits.
CFLAGS  = $(OPT) -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -std=gnu11 -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable \
           -Wno-format-truncation -Wno-stringop-truncation -Wno-array-bounds \
           -MMD -MP -Istubs -Ihost $(DEFS) -I$(FWSRC)/src -I$(FWSRC)/src/ps1 -I$(FWSRC)/ext/inih -I$(FWSRC)/ext/ESP8266SdFatWrapper/include
ifdef DEBUG
CFLAGS  += -DDEBUG_USB_UART
endif
LDFLAGS += $(OPT) -no-pie -pthread \
           -Wl,--defsym=_binary_gamedbps1_dat_size=8 -Wl,--defsym=_binary_gamedbps2_dat_size=8 \
           -Wl,--defsym=_binary_gamedbcoh_dat_size=8

FW_SRC  = src/ps1.c src/ps1/ps1_memory_card.c src/ps1/ps1_mc_data_interface.c src/ps1/ps1_dirty.c \
          src/ps1/ps1_cardman.c src/ps1/ps1_mmce.c src/ps1/ps1_empty_card.c src/card_config.c \
          src/util.c src/game_db/game_db.c src/bigmem.c ext/inih/ini.c
ifeq ($(SMC),1)
FW_SRC += src/ps1/ps1_supermemcard.c
endif
HOST_SRC = host/vcard.c host/vcard_wire.c host/sd_host.c host/firmware_host.c
B       ?= build
FW_OBJ  = $(addprefix $(B)/fw/,$(FW_SRC:.c=.o))
HOST_OBJ = $(addprefix $(B)/,$(HOST_SRC:.c=.o))

all: $(B)/test_card $(B)/vcardd $(if $(filter 1,$(SMC)),$(B)/smccli)

# for the SS1 (armv7, static so it doesn't depend on MiSTer Linux's glibc):
#   make ss1    ->  build-ss1/vcardd, build-ss1/test_card
ss1:
	$(MAKE) B=build-ss1 CC=arm-linux-gnueabihf-gcc LDFLAGS_STATIC=-static

$(B)/smc_host.o: ../../host/smc.c ../../host/smc.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fno-pie -c $< -o $@

$(B)/smc_sync.o: ../../host/smc_sync.c ../../host/smc_sync.h ../../host/smc.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fno-pie -c $< -o $@

$(B)/smc_icons.o: ../../host/smc_icons.c ../../host/smc_sync.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fno-pie -c $< -o $@

$(B)/smc_ident.o: ../../host/smc_ident.c ../../host/smc_ident.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fno-pie -c $< -o $@

$(B)/smc_gameid.o: ../../host/smc_gameid.c ../../host/smc_ident.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fno-pie -c $< -o $@

$(B)/smc_gamedb.o: ../../host/smc_gamedb.c ../../host/smc_gamedb.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fno-pie -c $< -o $@

$(B)/smc_vcard.o: ../../host/smc_vcard.c ../../host/smc_vcard.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fno-pie -c $< -o $@

$(B)/smccli.o: ../../tools/smccli/smccli.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I../../host -fno-pie -c $< -o $@

# the frontend's commands from a shell, against vcardd (tools/smccli)
$(B)/smccli: $(B)/smccli.o $(B)/smc_vcard.o $(B)/smc_host.o $(B)/smc_sync.o $(B)/smc_icons.o $(B)/smc_ident.o $(B)/smc_gameid.o $(B)/smc_gamedb.o
	$(CC) $^ $(LDFLAGS) $(LDFLAGS_STATIC) -o $@

$(B)/tests/test_card.o: tests/test_smc.inc tests/test_sync.inc

$(B)/vcardd: $(B)/host/vcardd.o $(HOST_OBJ) $(FW_OBJ)
	$(CC) $^ $(LDFLAGS) $(LDFLAGS_STATIC) -o $@

$(B)/fw/.ready: $(wildcard ../../firmware/ps1/* ../../firmware/patches/*)
	rm -rf $(B)/fw && mkdir -p $(B)/fw && cp -r $(FW)/src $(FW)/ext $(FW)/CMakeLists.txt $(B)/fw/
	../../firmware/apply.sh $(B)/fw
	touch $@

ifeq ($(SMC),1)
$(FW_OBJ): $(B)/fw/.ready
endif

$(B)/fw/%.o: $(FWSRC)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fno-pie -c $< -o $@
$(B)/%.o: %.c $(if $(filter 1,$(SMC)),$(B)/fw/.ready)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fno-pie -c $< -o $@

$(B)/test_card: $(B)/tests/test_card.o $(B)/tests/bus.o $(B)/host/vcard_client.o $(B)/smc_host.o $(B)/smc_sync.o $(B)/smc_icons.o $(B)/smc_ident.o $(B)/smc_gameid.o $(B)/smc_gamedb.o $(HOST_OBJ) $(FW_OBJ)
	$(CC) $^ $(LDFLAGS) $(LDFLAGS_STATIC) -o $@

test: $(B)/test_card
	$(B)/test_card
	$(MAKE) SMC=1
	build-smc/test_card

clean:
	rm -rf $(B)

# header dependencies (a stale object once hid a wire-protocol change, 08)
-include $(shell find $(B) -name '*.d' 2>/dev/null)
.PHONY: all test clean ss1
