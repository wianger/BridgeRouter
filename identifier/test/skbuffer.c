#include "util.h"

struct SkbufferLike {
  unsigned len;
  void *data;
};

struct PacketLike {
  unsigned headroom;
  unsigned len;
  void *head;
  void *data;
};

struct SkbufferLike *skbuffer_alloc(unsigned size) {
  struct SkbufferLike *s;

  s = kmalloc(sizeof(*s));
  s->data = kmalloc(size);
  s->len = size;
  return s;
}

void skbuffer_write_good(unsigned size) {
  struct SkbufferLike *s;

  s = skbuffer_alloc(size);
  copy_from_user(s->data, USER_SPACE, size);
}

void skbuffer_read_good(unsigned size) {
  struct SkbufferLike *s;

  s = skbuffer_alloc(size);
  copy_to_user(USER_SPACE, s->data, size);
}

void skbuffer_bad_size(unsigned size) {
  struct SkbufferLike *s;

  s = skbuffer_alloc(size);
  copy_from_user(s->data, USER_SPACE, size + 1);
}

struct PacketLike *packet_alloc(unsigned size) {
  struct PacketLike *p;
  void *buf;

  p = kmalloc(sizeof(*p));
  buf = kmalloc(size);
  p->head = buf;
  p->data = buf;
  p->len = size;
  return p;
}

int packet_copy_from_iter(struct PacketLike *p, unsigned off, void *from,
                          unsigned len) {
  copy_from_user((char *)p->data + off, from, len);
  return 0;
}

int packet_copy_to_iter(struct PacketLike *p, unsigned off, void *to,
                        unsigned len) {
  copy_to_user(to, (char *)p->data + off, len);
  return 0;
}

void packet_write_good(unsigned size) {
  struct PacketLike *p;

  p = packet_alloc(size);
  packet_copy_from_iter(p, 0, USER_SPACE, size);
}

void packet_read_good(unsigned size) {
  struct PacketLike *p;

  p = packet_alloc(size);
  packet_copy_to_iter(p, 0, USER_SPACE, size);
}

void packet_bad_offset(unsigned size) {
  struct PacketLike *p;

  p = packet_alloc(size);
  packet_copy_from_iter(p, 1, USER_SPACE, size);
}
