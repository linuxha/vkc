#ifndef KCTOOL_H
#define KCTOOL_H

#include <stddef.h>
#include <stdint.h>

extern int verbose;

void die(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
void info(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Read a whole file ("-" = stdin).  Caller frees. */
uint8_t *read_file(const char *path, size_t *len);
/* Write a whole file ("-" = stdout). */
void write_file(const char *path, const uint8_t *buf, size_t len);

int cmd_serve(int argc, char **argv);
int cmd_client(const char *verb, int argc, char **argv);

#endif
