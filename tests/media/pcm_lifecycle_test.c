#include "nj2me/nj.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int completed;
static void capture(void *ud, int level, const char *message)
{
    (void)ud; (void)level;
    puts(message);
    fflush(stdout);
    if (strstr(message, "SHORT_PCM_OK state=300 events=1")) completed = 1;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    nj_port port;
    assert(nj_port_headless(&port) == NJ_OK);
    port.log = capture;
    nj_run_opts opts = { .jar_path = argv[1], .heap_bytes = 1024 * 1024, .run_frames = 300 };
    assert(nj_run(&port, &opts) == 0);
    assert(completed);
    assert(nj_port_headless_pcm_frames(&port) == 32);
    assert(nj_port_headless_pcm_closes(&port) >= 1);
    if (port.shutdown) port.shutdown(port.ud);
    puts("Short PCM endOfMedia regression passed");
}
