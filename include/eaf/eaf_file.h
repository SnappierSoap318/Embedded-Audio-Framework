#pragma once
#include <eaf/eaf_source.h>
typedef struct {
    int descriptor;
    bool opened;
} eaf_file_t;
/* Regular files only; no stdio buffering or per-read allocation. */
int hal_file_open(eaf_file_t *file, eaf_reader_t *reader, const char *path);
void hal_file_close(eaf_file_t *file);
