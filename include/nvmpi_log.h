/*
 * nvmpi_log.h — structured logging macros for libnvmpi.
 *
 * Provides NVMPI_LOG(level, fmt, ...) and NVMPI_LOG_SUB(level, sub, fmt, ...)
 * for consistent, filterable log output across all libnvmpi source files.
 * Pure fprintf(stderr) — no C++ iostream, no external dependencies.
 *
 * Compile-time threshold: define NVMPI_LOG_LEVEL before including this header
 * to suppress messages below the chosen severity. Default: NVMPI_LOG_WARN.
 *
 * Output format:
 *   NVMPI_LOG:     [libnvmpi][E]: message
 *   NVMPI_LOG_SUB: [libnvmpi][jpeg][E]: message
 */
#ifndef __NVMPI_LOG_H__
#define __NVMPI_LOG_H__

#include <stdio.h>

/* Severity levels — lower value = higher severity. */
#define NVMPI_LOG_ERROR 0
#define NVMPI_LOG_WARN  1
#define NVMPI_LOG_INFO  2
#define NVMPI_LOG_DEBUG 3

/* Default threshold: compile with -DNVMPI_LOG_LEVEL=NVMPI_LOG_DEBUG
 * to enable all messages, or =NVMPI_LOG_ERROR for errors only. */
#ifndef NVMPI_LOG_LEVEL
#define NVMPI_LOG_LEVEL NVMPI_LOG_WARN
#endif

/* Short severity tags indexed by level. */
#define _NVMPI_LOG_TAG_0 "E"
#define _NVMPI_LOG_TAG_1 "W"
#define _NVMPI_LOG_TAG_2 "I"
#define _NVMPI_LOG_TAG_3 "D"

/* Indirect expansion so the numeric level resolves to the tag. */
#define _NVMPI_LOG_TAG_EXPAND(lvl) _NVMPI_LOG_TAG_##lvl
#define _NVMPI_LOG_TAG(lvl)        _NVMPI_LOG_TAG_EXPAND(lvl)

/*
 * NVMPI_LOG(level, fmt, ...)
 *   level: one of NVMPI_LOG_ERROR / _WARN / _INFO / _DEBUG
 *   fmt:   printf format string
 *
 * Example: NVMPI_LOG(NVMPI_LOG_ERROR, "alloc failed: size=%zu", sz);
 *   Output: [libnvmpi][E]: alloc failed: size=4096
 */
#define NVMPI_LOG(level, fmt, ...)                                       \
    do {                                                                 \
        if ((level) <= NVMPI_LOG_LEVEL) {                                \
            fprintf(stderr, "[libnvmpi][%s]: " fmt "\n",                 \
                    _NVMPI_LOG_TAG(level), ##__VA_ARGS__);               \
        }                                                                \
    } while (0)

/*
 * NVMPI_LOG_SUB(level, sub, fmt, ...)
 *   sub: subsystem string, e.g. "jpeg", "jpegenc"
 *
 * Example: NVMPI_LOG_SUB(NVMPI_LOG_ERROR, "jpeg", "decode failed fd=%d", fd);
 *   Output: [libnvmpi][jpeg][E]: decode failed fd=7
 */
#define NVMPI_LOG_SUB(level, sub, fmt, ...)                              \
    do {                                                                 \
        if ((level) <= NVMPI_LOG_LEVEL) {                                \
            fprintf(stderr, "[libnvmpi][%s][%s]: " fmt "\n",             \
                    (sub), _NVMPI_LOG_TAG(level), ##__VA_ARGS__);        \
        }                                                                \
    } while (0)

#endif /* __NVMPI_LOG_H__ */
