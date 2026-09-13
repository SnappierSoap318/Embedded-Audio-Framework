#define _POSIX_C_SOURCE 200809L
#include "../platform/esp32_lms/src/diagnostics.h"
#include "check.h"
#include <eaf/eaf_hal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static atomic_bool finished;
static void writer(void *ctx) {
    (void)ctx;
    for (unsigned i = 0; i < 200; ++i)
        board_log("concurrent-%u", i);
    atomic_store(&finished, true);
}
int main(void) {
    char buffer[4096];
    CHECK(!fflush(stdout));
    int saved_stdout = dup(STDOUT_FILENO);
    FILE *capture = tmpfile();
    CHECK(saved_stdout >= 0 && capture);
    CHECK(dup2(fileno(capture), STDOUT_FILENO) >= 0);
    board_log_memory("memory-only-%u", 123u);
    CHECK(!fflush(stdout));
    CHECK(ftell(capture) == 0);
    CHECK(board_log_read(buffer, sizeof(buffer)) > 0);
    CHECK(strstr(buffer, "memory-only-123"));
    board_log("uart-event-%u", 456u);
    CHECK(!fflush(stdout));
    CHECK(ftell(capture) > 0);
    CHECK(!fseek(capture, 0, SEEK_SET));
    CHECK(fgets(buffer, sizeof(buffer), capture));
    CHECK(strstr(buffer, "uart-event-456"));
    CHECK(dup2(saved_stdout, STDOUT_FILENO) >= 0);
    CHECK(!close(saved_stdout));
    CHECK(!fclose(capture));
    for (unsigned i = 0; i < 40; ++i)
        board_log("entry-%02u", i);
    size_t size = board_log_read(buffer, sizeof(buffer));
    CHECK(size < sizeof(buffer) && buffer[size] == '\0');
    CHECK(!strstr(buffer, "entry-00"));
    CHECK(strstr(buffer, "entry-16") && strstr(buffer, "entry-39"));
    CHECK(strstr(buffer, "entry-16") < strstr(buffer, "entry-39"));
    char long_line[300];
    memset(long_line, 'x', sizeof(long_line) - 1u);
    long_line[sizeof(long_line) - 1u] = '\0';
    board_log("%s", long_line);
    size = board_log_read(buffer, sizeof(buffer));
    CHECK(size < sizeof(buffer) && buffer[size] == '\0');
    char tiny[2] = {'a', 'b'};
    CHECK(board_log_read(tiny, 1) == 0 && tiny[0] == '\0' && tiny[1] == 'b');
    CHECK(board_log_read(NULL, 10) == 0);
    CHECK(board_log_read(buffer, 0) == 0);
    eaf_thread_t thread = {0};
    CHECK(!hal_thread_create(&thread, writer, NULL));
    do {
        size = board_log_read(buffer, sizeof(buffer));
        CHECK(size < sizeof(buffer) && buffer[size] == '\0');
    } while (!atomic_load(&finished));
    CHECK(!hal_thread_join(&thread));
    return 0;
}
