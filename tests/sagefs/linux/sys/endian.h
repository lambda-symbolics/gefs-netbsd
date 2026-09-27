/* NetBSD's <sys/endian.h> decoders, for building sagefs on Linux */
#ifndef SAGEFS_SYS_ENDIAN_H
#define SAGEFS_SYS_ENDIAN_H
#include <endian.h>
#include <stdint.h>
#include <string.h>
static inline uint16_t be16dec(const void *p) { uint16_t v; memcpy(&v, p, 2); return be16toh(v); }
static inline uint32_t be32dec(const void *p) { uint32_t v; memcpy(&v, p, 4); return be32toh(v); }
static inline uint64_t be64dec(const void *p) { uint64_t v; memcpy(&v, p, 8); return be64toh(v); }
static inline uint16_t le16dec(const void *p) { uint16_t v; memcpy(&v, p, 2); return le16toh(v); }
static inline uint32_t le32dec(const void *p) { uint32_t v; memcpy(&v, p, 4); return le32toh(v); }
static inline uint64_t le64dec(const void *p) { uint64_t v; memcpy(&v, p, 8); return le64toh(v); }
static inline void be16enc(void *p, uint16_t v) { v = htobe16(v); memcpy(p, &v, 2); }
static inline void be32enc(void *p, uint32_t v) { v = htobe32(v); memcpy(p, &v, 4); }
static inline void be64enc(void *p, uint64_t v) { v = htobe64(v); memcpy(p, &v, 8); }
#endif
