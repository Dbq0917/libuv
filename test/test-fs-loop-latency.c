#include "uv.h"
#include "task.h"

#include <stdlib.h>
#include <string.h>

#define TOTAL_BYTES (8 * 1024 * 1024)
#define CHUNK_BYTES (64 * 1024)
#define WRITER_DELAY_MS 50

#ifndef _WIN32

/* A pipe lets the writer hold the read in the thread pool deterministically.
 * A page-cached regular file can complete before the timer sample is useful. */
static uv_loop_t loop;
static uv_timer_t timer;
static uv_fs_t read_req;
static uv_fs_t close_req;
static uv_thread_t writer_thread;
static uv_buf_t read_iov;
static char read_buffer[CHUNK_BYTES];
static uv_file pipe_fds[2];
static size_t total_read;
static size_t total_written;
static int writer_error;
static int read_error;
static int timer_ticks;
static uint64_t timer_last;
static uint64_t timer_max_gap;
static int baseline_handles;
static int finishing;

static void count_handle(uv_handle_t* handle, void* arg) {
  (void) handle;
  (*(int*) arg)++;
}

static int handle_count(void) {
  int count;

  count = 0;
  uv_walk(&loop, count_handle, &count);
  return count;
}

static void finish_read(void) {
  if (finishing)
    return;

  finishing = 1;
  ASSERT_EQ(baseline_handles, handle_count());

  uv_timer_stop(&timer);
  uv_close((uv_handle_t*) &timer, NULL);

  if (pipe_fds[0] != -1) {
    ASSERT_OK(uv_fs_close(&loop, &close_req, pipe_fds[0], NULL));
    pipe_fds[0] = -1;
  }
}

static void read_cb(uv_fs_t* req) {
  ssize_t nread;

  nread = req->result;
  uv_fs_req_cleanup(req);

  if (nread < 0) {
    read_error = nread;
    finish_read();
    return;
  }

  if (nread == 0) {
    if (total_read != TOTAL_BYTES)
      read_error = UV_EOF;
    finish_read();
    return;
  }

  total_read += nread;
  if (total_read == TOTAL_BYTES) {
    finish_read();
    return;
  }

  read_iov = uv_buf_init(read_buffer, sizeof(read_buffer));
  ASSERT_OK(uv_fs_read(&loop,
                       &read_req,
                       pipe_fds[0],
                       &read_iov,
                       1,
                       -1,
                       read_cb));
}

static void timer_cb(uv_timer_t* handle) {
  uint64_t now;
  uint64_t gap;

  (void) handle;
  now = uv_hrtime();
  gap = now - timer_last;
  timer_last = now;
  timer_ticks++;

  if (gap > timer_max_gap)
    timer_max_gap = gap;
}

static void writer_cb(void* arg) {
  uv_fs_t req;
  uv_buf_t buf;
  char* data;
  size_t offset;
  size_t n;
  int r;

  (void) arg;
  data = malloc(CHUNK_BYTES);
  if (data == NULL) {
    writer_error = UV_ENOMEM;
    return;
  }

  memset(data, 'x', CHUNK_BYTES);
  offset = 0;

  uv_sleep(WRITER_DELAY_MS);

  while (offset < TOTAL_BYTES) {
    n = TOTAL_BYTES - offset;
    if (n > CHUNK_BYTES)
      n = CHUNK_BYTES;

    buf = uv_buf_init(data, n);
    r = uv_fs_write(NULL, &req, pipe_fds[1], &buf, 1, -1, NULL);
    uv_fs_req_cleanup(&req);

    if (r < 0) {
      writer_error = r;
      break;
    }

    offset += r;
    total_written = offset;
  }

  ASSERT_OK(uv_fs_close(NULL, &req, pipe_fds[1], NULL));
  uv_fs_req_cleanup(&req);
  pipe_fds[1] = -1;
  free(data);
}

#endif  /* !_WIN32 */


TEST_IMPL(fs_read_large_does_not_block_loop) {
#ifdef _WIN32
  RETURN_SKIP("Test requires a blocking Unix pipe");
#else
  ASSERT_OK(uv_loop_init(&loop));
  ASSERT_OK(uv_pipe(pipe_fds, 0, 0));
  ASSERT_OK(uv_timer_init(&loop, &timer));

  baseline_handles = handle_count();
  timer_last = uv_hrtime();

  ASSERT_OK(uv_thread_create(&writer_thread, writer_cb, NULL));
  ASSERT_OK(uv_timer_start(&timer, timer_cb, 1, 1));

  read_iov = uv_buf_init(read_buffer, sizeof(read_buffer));
  ASSERT_OK(uv_fs_read(&loop,
                       &read_req,
                       pipe_fds[0],
                       &read_iov,
                       1,
                       -1,
                       read_cb));

  ASSERT_OK(uv_run(&loop, UV_RUN_DEFAULT));
  ASSERT_OK(uv_thread_join(&writer_thread));

  ASSERT_OK(writer_error);
  ASSERT_OK(read_error);
  ASSERT_EQ(total_read, TOTAL_BYTES);
  ASSERT_EQ(total_written, TOTAL_BYTES);

  /* A synchronous read on the loop thread would hold it for the writer's
   * delay and leave the repeating timer with almost no ticks. */
  ASSERT_GE(timer_ticks, 20);
  ASSERT_LT(timer_max_gap, 100 * 1000 * 1000);

  ASSERT_OK(uv_loop_close(&loop));
  return 0;
#endif
}
