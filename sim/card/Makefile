# The virtual card: sd2psXtd's PS1 card code, unchanged, built for the PC.
#   ./get-firmware.sh && make test
FW      ?= firmware
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
           -Istubs -Ihost -I$(FW)/src -I$(FW)/src/ps1 -I$(FW)/ext/inih -I$(FW)/ext/ESP8266SdFatWrapper/include
ifdef DEBUG
CFLAGS  += -DDEBUG_USB_UART
endif
LDFLAGS += $(OPT) -no-pie -pthread \
           -Wl,--defsym=_binary_gamedbps1_dat_size=8 -Wl,--defsym=_binary_gamedbps2_dat_size=8 \
           -Wl,--defsym=_binary_gamedbcoh_dat_size=8

FW_SRC  = src/ps1.c src/ps1/ps1_memory_card.c src/ps1/ps1_mc_data_interface.c src/ps1/ps1_dirty.c \
          src/ps1/ps1_cardman.c src/ps1/ps1_mmce.c src/ps1/ps1_empty_card.c src/card_config.c \
          src/util.c src/game_db/game_db.c src/bigmem.c ext/inih/ini.c
HOST_SRC = host/vcard.c host/vcard_wire.c host/sd_host.c host/firmware_host.c
B       ?= build
FW_OBJ  = $(addprefix $(B)/fw/,$(FW_SRC:.c=.o))
HOST_OBJ = $(addprefix $(B)/,$(HOST_SRC:.c=.o))

all: $(B)/test_card $(B)/vcardd

# for the SS1 (armv7, static so it doesn't depend on MiSTer Linux's glibc):
#   make ss1    ->  build-ss1/vcardd, build-ss1/test_card
ss1:
	$(MAKE) B=build-ss1 CC=arm-linux-gnueabihf-gcc LDFLAGS_STATIC=-static

$(B)/vcardd: $(B)/host/vcardd.o $(HOST_OBJ) $(FW_OBJ)
	$(CC) $^ $(LDFLAGS) $(LDFLAGS_STATIC) -o $@

$(B)/fw/%.o: $(FW)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fno-pie -c $< -o $@
$(B)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fno-pie -c $< -o $@

$(B)/test_card: $(B)/tests/test_card.o $(B)/tests/bus.o $(B)/host/vcard_client.o $(HOST_OBJ) $(FW_OBJ)
	$(CC) $^ $(LDFLAGS) $(LDFLAGS_STATIC) -o $@

test: $(B)/test_card
	$(B)/test_card

clean:
	rm -rf $(B)
.PHONY: all test clean ss1
