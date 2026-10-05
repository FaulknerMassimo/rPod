/*
 * audio/aap.c against packets captured from real AirPods Pro 2 (the examples
 * in LibrePods' docs/AAP Definitions.md). Host-built: `make test`.
 */

#include "audio/aap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
            failures++;                                                        \
        }                                                                      \
    } while (0)

/* "04 00 04 00 ..." -> bytes; returns the length. */
static size_t hex(const char *s, uint8_t *out, size_t cap)
{
    size_t n = 0;
    while (*s != '\0' && n < cap) {
        while (*s == ' ') {
            s++;
        }
        if (*s == '\0') {
            break;
        }
        unsigned v;
        if (sscanf(s, "%2x", &v) != 1) {
            break;
        }
        out[n++] = (uint8_t)v;
        s += 2;
    }
    return n;
}

static bool built_equals(const uint8_t *pkt, size_t len, const char *expect)
{
    uint8_t want[64];
    size_t n = hex(expect, want, sizeof(want));
    return n == len && memcmp(pkt, want, n) == 0;
}

static void test_builders(void)
{
    uint8_t pkt[RPOD_AAP_MAX_PACKET];
    size_t n = rpod_aap_build_handshake(pkt);
    CHECK(built_equals(pkt, n, "00 00 04 00 01 00 02 00 00 00 00 00 00 00 00 00"));
    n = rpod_aap_build_features(pkt);
    CHECK(built_equals(pkt, n, "04 00 04 00 4d 00 ff 00 00 00 00 00 00 00"));
    n = rpod_aap_build_notifications(pkt);
    CHECK(built_equals(pkt, n, "04 00 04 00 0F 00 FF FF FF FF"));
    n = rpod_aap_build_control(pkt, RPOD_AAP_CTL_LISTENING_MODE, RPOD_AAP_MODE_TRANSPARENCY);
    CHECK(built_equals(pkt, n, "04 00 04 00 09 00 0D 03 00 00 00"));
    n = rpod_aap_build_control(pkt, RPOD_AAP_CTL_CONVERSATION, RPOD_AAP_OFF);
    CHECK(built_equals(pkt, n, "04 00 04 00 09 00 28 02 00 00 00"));
}

static void test_battery(void)
{
    uint8_t pkt[64];
    rpod_aap_event_t ev;
    size_t n = hex("04 00 04 00 04 00 03 02 01 64 02 01 04 01 63 01 01 08 01 11 02 01", pkt, sizeof(pkt));
    CHECK(rpod_aap_parse(pkt, n, &ev));
    CHECK(ev.type == RPOD_AAP_EV_BATTERY);
    CHECK(ev.battery.count == 3);
    CHECK(ev.battery.items[0].component == RPOD_AAP_BATT_RIGHT);
    CHECK(ev.battery.items[0].level == 100);
    CHECK(ev.battery.items[0].status == RPOD_AAP_BATT_DISCHARGING);
    CHECK(ev.battery.items[1].component == RPOD_AAP_BATT_LEFT);
    CHECK(ev.battery.items[1].level == 99);
    CHECK(ev.battery.items[1].status == RPOD_AAP_BATT_CHARGING);
    CHECK(ev.battery.items[2].component == RPOD_AAP_BATT_CASE);
    CHECK(ev.battery.items[2].level == 17);

    /* Claims three components but carries two. */
    CHECK(!rpod_aap_parse(pkt, n - 5, &ev));
    CHECK(ev.type == RPOD_AAP_EV_NONE);
}

static void test_control_and_ear(void)
{
    uint8_t pkt[64];
    rpod_aap_event_t ev;
    size_t n = hex("04 00 04 00 09 00 0D 03 00 00 00", pkt, sizeof(pkt));
    CHECK(rpod_aap_parse(pkt, n, &ev));
    CHECK(ev.type == RPOD_AAP_EV_CONTROL);
    CHECK(ev.control.id == RPOD_AAP_CTL_LISTENING_MODE);
    CHECK(ev.control.value[0] == RPOD_AAP_MODE_TRANSPARENCY);

    n = hex("04 00 04 00 09 00 28 01 00 00 00", pkt, sizeof(pkt));
    CHECK(rpod_aap_parse(pkt, n, &ev));
    CHECK(ev.control.id == RPOD_AAP_CTL_CONVERSATION && ev.control.value[0] == RPOD_AAP_ON);
    CHECK(!rpod_aap_parse(pkt, 10, &ev)); /* truncated */

    n = hex("04 00 04 00 06 00 00 01", pkt, sizeof(pkt));
    CHECK(rpod_aap_parse(pkt, n, &ev));
    CHECK(ev.type == RPOD_AAP_EV_EAR);
    CHECK(ev.ear.primary == RPOD_AAP_EAR_IN && ev.ear.secondary == RPOD_AAP_EAR_OUT);

    n = hex("04 00 04 00 4B 00 02 00 01 02", pkt, sizeof(pkt));
    CHECK(rpod_aap_parse(pkt, n, &ev));
    CHECK(ev.type == RPOD_AAP_EV_CONVERSATION && ev.conversation_level == 2);

    n = hex("04 00 04 00 19 00 06 02", pkt, sizeof(pkt));
    CHECK(rpod_aap_parse(pkt, n, &ev));
    CHECK(ev.type == RPOD_AAP_EV_PRESS);
    CHECK(ev.press.type == RPOD_AAP_PRESS_DOUBLE && ev.press.bud == 2);

    /* The handshake's acknowledgement, and an opcode rPod ignores. */
    n = hex("01 00 04 00 00 00 01 00 02 00", pkt, sizeof(pkt));
    CHECK(!rpod_aap_parse(pkt, n, &ev));
    n = hex("04 00 04 00 53 00 84 00 02 02", pkt, sizeof(pkt));
    CHECK(!rpod_aap_parse(pkt, n, &ev));
}

static void test_info(void)
{
    static const char *capture =
        "040004001d0002d5000400416972506f64732050726f004133303438004170706c6520496e632e0051584e52"
        "4848595850360036312e313836383034303030323030303030302e323731330036312e313836383034303030"
        "323030303030302e3237313300312e302e3000636f6d2e6170706c652e6163636573736f72792e7570646174"
        "65722e6170702e3731004859394c5432454632364a59004833504c5748444a32364b30003633353735333600"
        "89312a6567a5400f84a3ca234947efd40b90d78436ae5946748d70273e66066a258930003533393530363036"
        "3400";
    uint8_t pkt[512];
    size_t n = 0;
    for (const char *s = capture; s[0] != '\0' && s[1] != '\0' && n < sizeof(pkt); s += 2) {
        unsigned v;
        sscanf(s, "%2x", &v);
        pkt[n++] = (uint8_t)v;
    }
    rpod_aap_event_t ev;
    CHECK(rpod_aap_parse(pkt, n, &ev));
    CHECK(ev.type == RPOD_AAP_EV_INFO);
    CHECK(strcmp(ev.info.name, "AirPods Pro") == 0);
    CHECK(strcmp(ev.info.model, "A3048") == 0);
    CHECK(strcmp(ev.info.serial, "QXNRHHYXP6") == 0);
    CHECK(strcmp(ev.info.firmware, "61.1868040002000000.2713") == 0);
}

int main(void)
{
    test_builders();
    test_battery();
    test_control_and_ear();
    test_info();
    if (failures > 0) {
        fprintf(stderr, "test_aap: %d check(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    printf("test_aap: all checks passed\n");
    return EXIT_SUCCESS;
}
