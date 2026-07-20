/*
 * rPod application entry point.
 *
 * Phase 0: hello-world only, to validate the deploy path (docs/PLAN.md §9).
 * Later phases wire this into the display driver, wheel input socket, and
 * MPD client.
 */

#include <stdio.h>
#include <unistd.h>

int main(void)
{
    printf("rpod: hello from %s\n", "Pi Zero 2 W");
    fflush(stdout);

    while (1) {
        sleep(60);
    }

    return 0;
}
