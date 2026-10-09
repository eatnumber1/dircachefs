/*
 * An LD_PRELOAD library for the musl programs of the xfstests guest (written
 * for this repository; step 17.1). xfstests' tests and its _require_* checks
 * match glibc's error messages ("Operation not supported", "Too many levels
 * of symbolic links", ...), and the guest's tools (xfs_io, bash, GNU
 * coreutils, from Alpine) are linked against musl, whose messages differ for
 * a dozen errors. This library answers strerror(), strerror_r() and perror() with
 * glibc's text for those errors and passes every other error on to musl's
 * own function, so that a test sees what it would on a glibc system.
 *
 * It needs only dlsym(), write() and __errno_location() of the C library: it
 * is built freestanding
 * (-nostdlib), so it loads into any process, musl or glibc.
 *
 * The list is the errors for which musl's text (src/errno/__strerror.h)
 * differs from glibc's (sysdeps/gnu/errlist.c); EOPNOTSUPP is ENOTSUP's
 * number twice over in musl's table.
 */

#include <stddef.h>

extern void* dlsym(void* handle, const char* name);
extern int* __errno_location(void);
extern long write(int fd, const void* buf, unsigned long len);

#define RTLD_NEXT ((void*)-1)

#define EXDEV 18
#define ENOMEM 12
#define EMFILE 24
#define ENOTTY 25
#define EDOM 33
#define ERANGE 34
#define ENAMETOOLONG 36
#define ELOOP 40
#define EILSEQ 84
#define EOPNOTSUPP 95
#define EDQUOT 122

static const char* glibc_text(int e) {
  switch (e) {
    case ENOMEM:
      return "Cannot allocate memory";
    case EXDEV:
      return "Invalid cross-device link";
    case EMFILE:
      return "Too many open files";
    case ENOTTY:
      return "Inappropriate ioctl for device";
    case EDOM:
      return "Numerical argument out of domain";
    case ERANGE:
      return "Numerical result out of range";
    case ENAMETOOLONG:
      return "File name too long";
    case ELOOP:
      return "Too many levels of symbolic links";
    case EILSEQ:
      return "Invalid or incomplete multibyte or wide character";
    case EOPNOTSUPP:
      return "Operation not supported";
    case EDQUOT:
      return "Disk quota exceeded";
    default:
      return NULL;
  }
}

static void* next(const char* name) { return dlsym(RTLD_NEXT, name); }

char* strerror(int e) {
  const char* text = glibc_text(e);
  if (text != NULL) return (char*)text;
  char* (*original)(int) = (char* (*)(int))next("strerror");
  return original(e);
}

char* strerror_l(int e, void* locale) {
  const char* text = glibc_text(e);
  if (text != NULL) return (char*)text;
  char* (*original)(int, void*) = (char* (*)(int, void*))next("strerror_l");
  return original(e, locale);
}

static int copy_text(const char* text, char* buf, size_t len) {
  size_t i = 0;
  if (len == 0) return 34; /* ERANGE */
  while (text[i] != '\0' && i + 1 < len) {
    buf[i] = text[i];
    i++;
  }
  buf[i] = '\0';
  return text[i] == '\0' ? 0 : 34;
}

int strerror_r(int e, char* buf, size_t len) {
  const char* text = glibc_text(e);
  if (text != NULL) return copy_text(text, buf, len);
  int (*original)(int, char*, size_t) = (int (*)(int, char*, size_t))next("strerror_r");
  return original(e, buf, len);
}

int __xpg_strerror_r(int e, char* buf, size_t len) {
  return strerror_r(e, buf, len);
}

/* perror() names the text itself, and musl's own perror() would print musl's. */
void perror(const char* prefix) {
  const char* text = strerror(*__errno_location());
  char line[512];
  size_t n = 0;
  if (prefix != NULL && prefix[0] != '\0') {
    for (size_t i = 0; prefix[i] != '\0' && n < 400; i++) line[n++] = prefix[i];
    line[n++] = ':';
    line[n++] = ' ';
  }
  for (size_t i = 0; text[i] != '\0' && n < sizeof(line) - 1; i++) line[n++] = text[i];
  line[n++] = '\n';
  write(2, line, n);
}
