#include "aap.h"

#include <string.h>

#define OP_BATTERY       0x0004
#define OP_EAR           0x0006
#define OP_CONTROL       0x0009
#define OP_NOTIFICATIONS 0x000F
#define OP_PRESS         0x0019
#define OP_INFO          0x001D
#define OP_CONVERSATION  0x004B
#define OP_FEATURES      0x004D

static const uint8_t header[4] = { 0x04, 0x00, 0x04, 0x00 };

/* Header + opcode; returns the offset the payload starts at. */
static size_t put_header(uint8_t *out, uint16_t opcode)
{
    memcpy(out, header, sizeof(header));
    out[4] = (uint8_t)(opcode & 0xff);
    out[5] = (uint8_t)(opcode >> 8);
    return 6;
}

size_t rpod_aap_build_handshake(uint8_t *out)
{
    static const uint8_t pkt[16] = { 0x00, 0x00, 0x04, 0x00, 0x01, 0x00, 0x02, 0x00 };
    memcpy(out, pkt, sizeof(pkt));
    return sizeof(pkt);
}

size_t rpod_aap_build_features(uint8_t *out)
{
    /* All feature bits on, as macOS sends on Apple silicon. */
    size_t n = put_header(out, OP_FEATURES);
    memset(out + n, 0, 8);
    out[n] = 0xFF;
    return n + 8;
}

size_t rpod_aap_build_notifications(uint8_t *out)
{
    size_t n = put_header(out, OP_NOTIFICATIONS);
    memset(out + n, 0xFF, 4);
    return n + 4;
}

size_t rpod_aap_build_control(uint8_t *out, uint8_t id, uint8_t value)
{
    size_t n = put_header(out, OP_CONTROL);
    out[n] = id;
    out[n + 1] = value;
    memset(out + n + 2, 0, 3);
    return n + 5;
}

/* Copies at most dst_size - 1 bytes without splitting a UTF-8 sequence. */
static void copy_utf8(char *dst, size_t dst_size, const uint8_t *src, size_t n)
{
    if (n >= dst_size) {
        n = dst_size - 1;
        while (n > 0 && (src[n] & 0xC0) == 0x80) {
            n--;
        }
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* Device information: two bytes of unknown meaning after the opcode, then
 * NUL-terminated strings -- name, model, manufacturer, serial, firmware,
 * then more versions, the buds' serials, and encrypted bytes. A stray binary
 * token sits ahead of the name; anything with a control character in it is
 * skipped, which drops that and keeps the strings in order. */
static void parse_info(const uint8_t *pkt, size_t len, rpod_aap_event_t *out)
{
    struct {
        char *dst;
        size_t size;
    } fields[] = {
        { out->info.name, sizeof(out->info.name) },
        { out->info.model, sizeof(out->info.model) },
        { NULL, 0 }, /* manufacturer: always "Apple Inc." */
        { out->info.serial, sizeof(out->info.serial) },
        { out->info.firmware, sizeof(out->info.firmware) },
    };
    size_t nfield = 0;
    size_t i = 8;
    while (i < len && nfield < sizeof(fields) / sizeof(fields[0])) {
        size_t start = i;
        bool text = true;
        while (i < len && pkt[i] != 0) {
            if (pkt[i] < 0x20 || pkt[i] == 0x7F) {
                text = false;
            }
            i++;
        }
        if (i > start && text) {
            if (fields[nfield].dst != NULL) {
                copy_utf8(fields[nfield].dst, fields[nfield].size, pkt + start, i - start);
            }
            nfield++;
        }
        i++; /* the NUL */
    }
}

bool rpod_aap_parse(const uint8_t *pkt, size_t len, rpod_aap_event_t *out)
{
    memset(out, 0, sizeof(*out));
    if (len < 6 || memcmp(pkt, header, sizeof(header)) != 0) {
        return false;
    }
    uint16_t opcode = (uint16_t)(pkt[4] | pkt[5] << 8);

    switch (opcode) {
    case OP_BATTERY: {
        /* count, then per component: id, 01, level, status, 01 */
        if (len < 7) {
            return false;
        }
        size_t count = pkt[6];
        if (len < 7 + count * 5) {
            return false;
        }
        if (count > RPOD_AAP_MAX_BATTERIES) {
            count = RPOD_AAP_MAX_BATTERIES;
        }
        for (size_t i = 0; i < count; i++) {
            const uint8_t *c = pkt + 7 + i * 5;
            out->battery.items[i].component = c[0];
            out->battery.items[i].level = c[2];
            out->battery.items[i].status = c[3];
        }
        out->battery.count = count;
        out->type = RPOD_AAP_EV_BATTERY;
        return true;
    }
    case OP_EAR:
        if (len < 8) {
            return false;
        }
        out->ear.primary = pkt[6];
        out->ear.secondary = pkt[7];
        out->type = RPOD_AAP_EV_EAR;
        return true;
    case OP_CONTROL:
        if (len < 11) {
            return false;
        }
        out->control.id = pkt[6];
        memcpy(out->control.value, pkt + 7, 4);
        out->type = RPOD_AAP_EV_CONTROL;
        return true;
    case OP_CONVERSATION:
        /* 4B 00 02 00 01 [level] */
        if (len < 10) {
            return false;
        }
        out->conversation_level = pkt[9];
        out->type = RPOD_AAP_EV_CONVERSATION;
        return true;
    case OP_INFO:
        parse_info(pkt, len, out);
        out->type = RPOD_AAP_EV_INFO;
        return true;
    case OP_PRESS:
        if (len < 8) {
            return false;
        }
        out->press.type = pkt[6];
        out->press.bud = pkt[7];
        out->type = RPOD_AAP_EV_PRESS;
        return true;
    default:
        return false;
    }
}
