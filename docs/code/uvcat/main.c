#include <assert.h>
#include <stdio.h>
#include <fcntl.h>
#include <uv.h>

void on_read(uv_fs_t *req);
void on_close(uv_fs_t *req);

uv_fs_t open_req;
uv_fs_t read_req;
uv_fs_t write_req;
uv_fs_t close_req;

static char buffer[64 * 1024];

static uv_buf_t iov;
static uv_file source_fd = -1;
static int close_started;

static void close_source(void) {
    if (close_started)
        return;

    close_started = 1;
    uv_fs_close(uv_default_loop(), &close_req, source_fd, on_close);
}

void on_close(uv_fs_t *req) {
    if (req->result < 0) {
        fprintf(stderr, "Close error: %s\n", uv_strerror((int)req->result));
    }
    uv_fs_req_cleanup(req);
}

void on_write(uv_fs_t *req) {
    if (req->result < 0) {
        fprintf(stderr, "Write error: %s\n", uv_strerror((int)req->result));
        uv_fs_req_cleanup(req);
        close_source();
    }
    else {
        uv_fs_req_cleanup(req);
        uv_fs_read(uv_default_loop(), &read_req, source_fd, &iov, 1, -1, on_read);
    }
}

void on_read(uv_fs_t *req) {
    ssize_t nread;

    nread = req->result;
    uv_fs_req_cleanup(req);

    if (nread < 0) {
        fprintf(stderr, "Read error: %s\n", uv_strerror((int)nread));
        close_source();
    }
    else if (nread == 0) {
        close_source();
    }
    else {
        iov.len = nread;
        uv_fs_write(uv_default_loop(), &write_req, 1, &iov, 1, -1, on_write);
    }
}

void on_open(uv_fs_t *req) {
    // The request passed to the callback is the same as the one the call setup
    // function was passed.
    assert(req == &open_req);
    if (req->result >= 0) {
        source_fd = req->result;
        uv_fs_req_cleanup(req);
        iov = uv_buf_init(buffer, sizeof(buffer));
        uv_fs_read(uv_default_loop(), &read_req, source_fd,
                   &iov, 1, -1, on_read);
    }
    else {
        fprintf(stderr, "error opening file: %s\n", uv_strerror((int)req->result));
        uv_fs_req_cleanup(req);
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <path>\n", argv[0]);
        return 1;
    }

    uv_fs_open(uv_default_loop(), &open_req, argv[1], O_RDONLY, 0, on_open);
    uv_run(uv_default_loop(), UV_RUN_DEFAULT);

    uv_fs_req_cleanup(&open_req);
    uv_fs_req_cleanup(&read_req);
    uv_fs_req_cleanup(&write_req);
    uv_fs_req_cleanup(&close_req);
    return 0;
}
