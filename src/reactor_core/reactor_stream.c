#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>

#include <dynamic.h>

#include "reactor_user.h"
#include "reactor_desc.h"
#include "reactor_core.h"
#include "reactor_stream.h"

static __thread reactor_stream *current = NULL;

static size_t reactor_stream_desc_write(reactor_stream *stream, void *data, size_t size)
{
  size_t i;
  ssize_t n;

  for (i = 0; i < size; i += n)
    {
      n = reactor_desc_write(&stream->desc, (char *) data + i, size - i);
      if (n == -1)
        {
          stream->flags |= REACTOR_STREAM_FLAGS_BLOCKED;
          break;
        }
    }
  reactor_desc_write_notify(&stream->desc, i != size);
  return i;
}

void reactor_stream_init(reactor_stream *stream, reactor_user_callback *callback, void *state)
{
  *stream = (reactor_stream) {.state = REACTOR_STREAM_CLOSED};
  reactor_user_init(&stream->user, callback, state);
  reactor_desc_init(&stream->desc, reactor_stream_event, stream);
  buffer_init(&stream->input);
  buffer_init(&stream->output);
}

void reactor_stream_open(reactor_stream *stream, int fd)
{
  if (stream->state != REACTOR_STREAM_CLOSED)
    {
      (void) close(fd);
      reactor_stream_error(stream);
      return;
    }

  stream->state = REACTOR_STREAM_OPEN;
  reactor_desc_open(&stream->desc, fd);
}

void reactor_stream_error(reactor_stream *stream)
{
  stream->state = REACTOR_STREAM_INVALID;
  reactor_user_dispatch(&stream->user, REACTOR_STREAM_ERROR, NULL);
}

void reactor_stream_shutdown(reactor_stream *stream)
{
  if (stream->state != REACTOR_STREAM_OPEN &&
      stream->state != REACTOR_STREAM_INVALID)
    return;

  if (stream->state == REACTOR_STREAM_OPEN && buffer_size(&stream->output))
    {
      stream->state = REACTOR_STREAM_LINGER;
      reactor_desc_read_notify(&stream->desc, 0);
      return;
    }

  reactor_stream_close(stream);
}

void reactor_stream_close(reactor_stream *stream)
{
  if (stream->state != REACTOR_STREAM_OPEN &&
      stream->state != REACTOR_STREAM_LINGER &&
      stream->state != REACTOR_STREAM_INVALID)
    return;

  reactor_desc_close(&stream->desc);
}

void reactor_stream_close_final(reactor_stream *stream)
{
  if (current == stream)
    current = NULL;
  buffer_clear(&stream->input);
  buffer_clear(&stream->output);
  stream->state = REACTOR_STREAM_CLOSED;
  reactor_user_dispatch(&stream->user, REACTOR_STREAM_CLOSE, NULL);
}

void reactor_stream_event(void *state, int type, void *data)
{
  reactor_stream *stream = state;

  (void) data;
  switch(type)
    {
    case REACTOR_DESC_ERROR:
      reactor_stream_error(stream);
      break;
    case REACTOR_DESC_READ:
      if (stream->state != REACTOR_STREAM_OPEN)
        break;
      current = stream;
      reactor_stream_read(stream);
      if (current && (stream->flags & REACTOR_STREAM_FLAGS_BLOCKED) == 0)
        reactor_stream_flush(stream);
      break;
    case REACTOR_DESC_WRITE:
      stream->flags &= ~REACTOR_STREAM_FLAGS_BLOCKED;
      current = stream;
      reactor_stream_flush(stream);
      if (current &&
          stream->state == REACTOR_STREAM_OPEN &&
          (stream->flags & REACTOR_STREAM_FLAGS_BLOCKED) == 0)
        reactor_user_dispatch(&stream->user, REACTOR_STREAM_WRITE_AVAILABLE, NULL);
      break;
    case REACTOR_DESC_SHUTDOWN:
      reactor_user_dispatch(&stream->user, REACTOR_STREAM_SHUTDOWN, NULL);
      break;
    case REACTOR_DESC_CLOSE:
      reactor_stream_close_final(stream);
      break;
    }
}

void reactor_stream_read(reactor_stream *stream)
{
  char buffer[REACTOR_STREAM_BLOCK_SIZE];
  reactor_stream_data data;
  ssize_t n;

  n = reactor_desc_read(&stream->desc, buffer, sizeof buffer);
  if (n == -1 && errno != EAGAIN)
    {
      reactor_stream_error(stream);
      return;
    }

  if (n == 0)
    {
      reactor_stream_shutdown(stream);
      return;
    }

  if (n > 0)
    {
      data = (reactor_stream_data) {.base = buffer, .size = n};
      current = stream;
      reactor_user_dispatch(&stream->user, REACTOR_STREAM_READ, &data);
      if (current && data.size)
        buffer_insert(&stream->input, buffer_size(&stream->input), data.base, data.size);
    }
}

void reactor_stream_write(reactor_stream *stream, void *base, size_t size)
{
  int e;

  e = buffer_insert(&stream->output, buffer_size(&stream->output), base, size);
  if (e == -1)
    reactor_stream_error(stream);
}

void reactor_stream_write_direct(reactor_stream *stream, void *base, size_t size)
{
  size_t n;

  if (stream->state != REACTOR_STREAM_OPEN && stream->state != REACTOR_STREAM_LINGER)
    return;

  if (buffer_size(&stream->output))
    {
      reactor_stream_write(stream, base, size);
      return;
    }

  n = reactor_stream_desc_write(stream, base, size);
  if (n < size)
    {
      if (errno != EAGAIN)
        reactor_stream_error(stream);
      else
        reactor_stream_write(stream, (char *) base + n, size - n);
    }
}

void reactor_stream_flush(reactor_stream *stream)
{
  ssize_t n;

  if (stream->state != REACTOR_STREAM_OPEN && stream->state != REACTOR_STREAM_LINGER)
    return;

  n = reactor_stream_desc_write(stream, buffer_data(&stream->output), buffer_size(&stream->output));
  if (n == -1 && errno != EAGAIN)
    {
      reactor_stream_error(stream);
      return;
    }

  buffer_erase(&stream->output, 0, n);
  if (stream->state == REACTOR_STREAM_LINGER && buffer_size(&stream->output) == 0)
    reactor_stream_close(stream);
}
