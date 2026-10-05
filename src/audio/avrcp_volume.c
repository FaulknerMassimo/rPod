#include "avrcp_volume.h"

void rpod_avrcp_volume_init(rpod_avrcp_volume_t *v)
{
    *v = (rpod_avrcp_volume_t){ .anchor = -1, .last = -1 };
}

void rpod_avrcp_volume_state(rpod_avrcp_volume_t *v, bool active, uint32_t now_ms)
{
    if (active && !v->active) {
        v->settle_until = now_ms + RPOD_AVRCP_SETTLE_MS;
    }
    v->active = active;
}

int rpod_avrcp_volume_changed(rpod_avrcp_volume_t *v, int value, uint32_t now_ms, int *repin)
{
    *repin = -1;
    int before = v->last;
    v->last = value;
    if (v->broken) {
        return 0;
    }
    /* Our own set landing. A press that arrived before it was measured from
     * where the headset was then, so this isn't one. */
    if (v->repin_pending && value == v->anchor) {
        v->repin_pending = false;
        return 0;
    }
    /* Not streaming, or still setting up: whatever it's at is the new pin. */
    if (!v->active || v->anchor < 0 || before < 0 || (int32_t)(now_ms - v->settle_until) < 0) {
        v->anchor = value;
        v->repin_pending = false;
        return 0;
    }
    int delta = value - before;
    if (delta == 0) {
        return 0;
    }
    int half = RPOD_AVRCP_VOLUME_STEP / 2;
    int steps = (delta + (delta > 0 ? half : -half)) / RPOD_AVRCP_VOLUME_STEP;
    if (steps == 0) {
        steps = delta > 0 ? 1 : -1;
    }
    *repin = v->anchor;
    v->repin_pending = true;
    return steps;
}

void rpod_avrcp_volume_repin_failed(rpod_avrcp_volume_t *v)
{
    v->broken = true;
    v->repin_pending = false;
}
