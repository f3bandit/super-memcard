#include "bus.h"
#include "vcard.h"

#include <string.h>

uint8_t mc_flag;

size_t bus_frame(const uint8_t *out, uint8_t *in, bool *ack, size_t n) {
    size_t i;
    vcard_select(true);
    for (i = 0; i < n; i++) {
        bool a = false;
        uint8_t r = vcard_xfer(out[i], i + 1 == n, &a);
        if (in)
            in[i] = r;
        if (ack)
            ack[i] = a;
        if (!a && i + 1 < n) {
            i++;
            break;
        }
    }
    vcard_select(false);
    return i;
}

void bus_wake(void) {
    vcard_select(true);
    vcard_select(false);
}

int mc_get_id(uint8_t reply[9]) {
    uint8_t out[10] = {0x81, 'S'}, in[10];
    if (bus_frame(out, in, NULL, 10) != 10)
        return MC_NO_REPLY;
    mc_flag = in[1];
    memcpy(reply, in + 1, 9);
    return 0;
}

int mc_read(uint16_t sector, uint8_t data[128]) {
    uint8_t out[140] = {0x81, 'R', 0, 0, sector >> 8, sector & 0xFF}, in[140];
    if (bus_frame(out, in, NULL, 140) != 140)
        return MC_NO_REPLY;
    mc_flag = in[1];
    if (in[2] != 0x5A || in[3] != 0x5D || in[6] != 0x5C || in[7] != 0x5D)
        return -2;
    if (in[8] != (sector >> 8) || in[9] != (sector & 0xFF))
        return -2;
    uint8_t chk = in[8] ^ in[9];
    for (int i = 0; i < 128; i++)
        chk ^= in[10 + i];
    if (in[138] != chk)
        return -2;
    memcpy(data, in + 10, 128);
    return in[139];
}

static size_t write_frame(uint16_t sector, const uint8_t data[128], uint8_t chk_xor, uint8_t *in, size_t n) {
    uint8_t out[138] = {0x81, 'W', 0, 0, sector >> 8, sector & 0xFF};
    uint8_t chk = out[4] ^ out[5];
    for (int i = 0; i < 128; i++) {
        out[6 + i] = data[i];
        chk ^= data[i];
    }
    out[134] = chk ^ chk_xor;
    return bus_frame(out, in, NULL, n);
}

int mc_write(uint16_t sector, const uint8_t data[128], uint8_t chk_xor) {
    uint8_t in[138];
    if (write_frame(sector, data, chk_xor, in, 138) != 138)
        return MC_NO_REPLY;
    mc_flag = in[1];
    return in[137];
}

void mc_write_cut(uint16_t sector, const uint8_t data[128], int bytes) {
    uint8_t in[138];
    write_frame(sector, data, 0, in, 6 + (size_t)bytes);
}

int mmce_game_id(const char *id) {
    uint8_t out[300] = {0x81, 0x21, 0, 0}, in[300];
    bool ack[300];
    size_t len = strlen(id);
    out[3] = (uint8_t)len;
    memcpy(out + 4, id, len);
    size_t n = 4 + len;
    size_t sent = bus_frame(out, in, ack, n);
    int acked = 0;
    for (size_t i = 0; i < sent; i++)
        acked += ack[i];
    return acked;
}

int mmce_ping(void) {
    uint8_t out[6] = {0x81, 0x20}, in[6];
    if (bus_frame(out, in, NULL, 6) != 6)
        return MC_NO_REPLY;
    return (in[2] == 0x00 && in[3] == 0x00 && in[4] == 0x27 && in[5] == 0xFF) ? 0 : -2;
}
