#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#include <eaf/eaf_file.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
static int read_at(void *ctx, uint64_t offset, void *dst, size_t bytes) {
    eaf_file_t *f = ctx;
    if (!f || !f->opened || !dst || offset > INT64_MAX || bytes > INT64_MAX - offset)
        return EAF_INVALID;
    size_t done = 0;
    while (done < bytes) {
        ssize_t got =
            pread(f->descriptor, (unsigned char *)dst + done, bytes - done, (off_t)(offset + done));
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            return EAF_IO;
        done += (size_t)got;
    }
    return EAF_OK;
}
int hal_file_open(eaf_file_t *file, eaf_reader_t *reader, const char *path) {
    if (!file || file->opened || !reader || !path)
        return EAF_INVALID;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0)
        return EAF_IO;
    struct stat info;
    if (fstat(fd, &info) || !S_ISREG(info.st_mode) || info.st_size < 0) {
        (void)close(fd);
        return EAF_INVALID;
    }
    *file = (eaf_file_t){fd, true};
    *reader = (eaf_reader_t){read_at, file, (uint64_t)info.st_size};
    return EAF_OK;
}
void hal_file_close(eaf_file_t *file) {
    if (file && file->opened) {
        (void)close(file->descriptor);
        file->opened = false;
    }
}
