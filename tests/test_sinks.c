/*
 * pw-dump's sinks (src/audio/sinks_parse.c): which are Bluetooth and which
 * AirPlay, and which one is the default.
 */

#include "audio/sinks_parse.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static const char k_dump[] =
    "[ {\"id\":0,\"type\":\"PipeWire:Interface:Core\",\"info\":{}},"
    "  {\"id\":41,\"type\":\"PipeWire:Interface:Node\",\"info\":{\"props\":{"
    "    \"media.class\":\"Audio/Sink\",\"node.name\":\"raop_sink.Kitchen.local.192.168.1.7.7000\","
    "    \"node.description\":\"Kitchen\"}}},"
    "  {\"id\":52,\"type\":\"PipeWire:Interface:Node\",\"info\":{\"props\":{"
    "    \"media.class\":\"Audio/Sink\",\"node.name\":\"bluez_output.AA_BB.1\","
    "    \"node.description\":\"AirPods Pro\"}}},"
    "  {\"id\":53,\"type\":\"PipeWire:Interface:Node\",\"info\":{\"props\":{"
    "    \"media.class\":\"Audio/Source\",\"node.name\":\"bluez_input.AA_BB.0\"}}},"
    "  {\"id\":60,\"type\":\"PipeWire:Interface:Node\",\"info\":{\"props\":{"
    "    \"media.class\":\"Audio/Sink\",\"node.name\":\"auto_null\"}}},"
    "  {\"id\":44,\"type\":\"PipeWire:Interface:Node\",\"info\":{\"props\":{"
    "    \"media.class\":\"Audio/Sink\",\"node.name\":\"raop_sink.Bedroom.local.192.168.1.8.7000\","
    "    \"node.nick\":\"Bedroom\"}}},"
    "  {\"id\":30,\"type\":\"PipeWire:Interface:Metadata\",\"props\":{\"metadata.name\":\"default\"},"
    "   \"metadata\":[{\"subject\":0,\"key\":\"default.audio.sink\",\"type\":\"Spa:String:JSON\","
    "    \"value\":{\"name\":\"raop_sink.Kitchen.local.192.168.1.7.7000\"}}]} ]";

static const char k_owntone[] =
    "{\"outputs\":[{\"id\":\"25497184543\",\"name\":\"Living Room\",\"type\":\"AirPlay 2\",\"selected\":true,"
    "\"has_password\":false,\"requires_auth\":false,\"volume\":50},"
    "{\"id\":\"0\",\"name\":\"Computer\",\"type\":\"ALSA\",\"selected\":false},"
    "{\"id\":\"1234\",\"name\":\"Basement\",\"type\":\"AirPlay\",\"selected\":false},"
    "{\"id\":\"bad id\",\"name\":\"Nope\",\"type\":\"AirPlay\"}]}";

int main(void)
{
    rpod_sink_t s[RPOD_SINK_MAX];
    size_t n = rpod_sinks_parse(k_dump, strlen(k_dump), s, RPOD_SINK_MAX);
    CHECK(n == 1); /* the headset: no source, no auto_null, no raop */
    CHECK(s[0].kind == RPOD_SINK_BLUETOOTH && s[0].id == 52 && strcmp(s[0].label, "AirPods Pro") == 0);
    CHECK(!s[0].current);

    n = rpod_sinks_parse_owntone(k_owntone, strlen(k_owntone), s, RPOD_SINK_MAX);
    CHECK(n == 2); /* no local output, no bad id */
    CHECK(strcmp(s[0].label, "Basement") == 0 && strcmp(s[0].ref, "1234") == 0 && !s[0].current);
    CHECK(strcmp(s[1].label, "Living Room") == 0 && strcmp(s[1].ref, "25497184543") == 0 && s[1].current);
    CHECK(s[1].kind == RPOD_SINK_AIRPLAY);
    CHECK(rpod_sinks_parse_owntone("nope", 4, s, RPOD_SINK_MAX) == 0);
    CHECK(rpod_sinks_parse_owntone("{}", 2, s, RPOD_SINK_MAX) == 0);

    /* A cap cuts the list, junk gives nothing. */
    CHECK(rpod_sinks_parse(k_dump, strlen(k_dump), s, 0) == 0);
    CHECK(rpod_sinks_parse("nope", 4, s, RPOD_SINK_MAX) == 0);
    CHECK(rpod_sinks_parse("{}", 2, s, RPOD_SINK_MAX) == 0);
    CHECK(rpod_sinks_parse("[]", 2, s, RPOD_SINK_MAX) == 0);

    if (failures == 0) {
        puts("test_sinks: ok");
    }
    return failures != 0;
}
