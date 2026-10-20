/*
 * Compatibility shim for the x86_64 Android build of VVEye's libt2u.
 *
 * That SDK was built against Bionic.  Its addrinfo layout is:
 *   ... ai_canonname, ai_addr, ai_next
 * whereas glibc uses:
 *   ... ai_addr, ai_canonname, ai_next
 *
 * The shim also supplies the old Bionic symbols required by the library.
 * It is intentionally a separate shared object: libt2u.so has a NEEDED
 * entry for liblog.so.
 */
#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <netdb.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/socket.h>

/* Referenced by Android's stack protector ABI.  Seed it from the kernel's
 * AT_RANDOM bytes so the canary is not a fixed, predictable value. */
uintptr_t __stack_chk_guard = UINT64_C(0x6a09e667f3bcc909);

__attribute__((constructor))
static void seed_stack_guard(void)
{
    const void *random = (const void *)getauxval(AT_RANDOM);

    if (random != NULL)
        memcpy(&__stack_chk_guard, random, sizeof(__stack_chk_guard));
}

/* Bionic exports __errno as a function; glibc uses __errno_location. */
int *__errno(void)
{
    return __errno_location();
}

struct android_addrinfo {
    int ai_flags;
    int ai_family;
    int ai_socktype;
    int ai_protocol;
    socklen_t ai_addrlen;
    char *ai_canonname;
    struct sockaddr *ai_addr;
    struct android_addrinfo *ai_next;
};

_Static_assert(offsetof(struct android_addrinfo, ai_addr) == 32,
               "unexpected Android addrinfo layout");
_Static_assert(offsetof(struct android_addrinfo, ai_next) == 40,
               "unexpected Android addrinfo layout");

static int (*real_getaddrinfo)(const char *, const char *,
                               const struct addrinfo *, struct addrinfo **);
static void (*real_freeaddrinfo)(struct addrinfo *);

static void resolve_netdb(void)
{
    if (real_getaddrinfo == NULL) {
        *(void **)(&real_getaddrinfo) = dlsym(RTLD_NEXT, "getaddrinfo");
    }
    if (real_freeaddrinfo == NULL) {
        *(void **)(&real_freeaddrinfo) = dlsym(RTLD_NEXT, "freeaddrinfo");
    }
}

int getaddrinfo(const char *node, const char *service,
                const struct addrinfo *android_hints,
                struct addrinfo **android_result)
{
    struct addrinfo glibc_hints;
    const struct addrinfo *hints = NULL;
    struct addrinfo *glibc_result = NULL;
    struct android_addrinfo *head = NULL;
    struct android_addrinfo **tail = &head;
    int result;

    resolve_netdb();
    if (real_getaddrinfo == NULL || real_freeaddrinfo == NULL) {
        if (android_result != NULL)
            *android_result = NULL;
        return EAI_SYSTEM;
    }

    /* Translate the input hints too.  The current SDK only supplies family
     * and socktype, but doing this avoids depending on that implementation
     * detail. */
    if (android_hints != NULL) {
        memset(&glibc_hints, 0, sizeof(glibc_hints));
        glibc_hints.ai_flags = android_hints->ai_flags;
        glibc_hints.ai_family = android_hints->ai_family;
        glibc_hints.ai_socktype = android_hints->ai_socktype;
        glibc_hints.ai_protocol = android_hints->ai_protocol;
        glibc_hints.ai_addrlen = android_hints->ai_addrlen;
        glibc_hints.ai_canonname = android_hints->ai_canonname;
        glibc_hints.ai_addr = android_hints->ai_addr;
        glibc_hints.ai_next = (struct addrinfo *)android_hints->ai_next;
        hints = &glibc_hints;
    }

    result = real_getaddrinfo(node, service, hints, &glibc_result);
    if (result != 0) {
        if (android_result != NULL)
            *android_result = NULL;
        return result;
    }

    for (struct addrinfo *entry = glibc_result; entry != NULL;
         entry = entry->ai_next) {
        struct android_addrinfo *copy = calloc(1, sizeof(*copy));
        if (copy == NULL) {
            real_freeaddrinfo(glibc_result);
            while (head != NULL) {
                struct android_addrinfo *next = head->ai_next;
                free(head->ai_canonname);
                free(head->ai_addr);
                free(head);
                head = next;
            }
            if (android_result != NULL)
                *android_result = NULL;
            return EAI_MEMORY;
        }

        copy->ai_flags = entry->ai_flags;
        copy->ai_family = entry->ai_family;
        copy->ai_socktype = entry->ai_socktype;
        copy->ai_protocol = entry->ai_protocol;
        copy->ai_addrlen = entry->ai_addrlen;
        if (entry->ai_canonname != NULL)
            copy->ai_canonname = strdup(entry->ai_canonname);
        if (entry->ai_addr != NULL && entry->ai_addrlen != 0) {
            copy->ai_addr = malloc(entry->ai_addrlen);
            if (copy->ai_addr != NULL)
                memcpy(copy->ai_addr, entry->ai_addr, entry->ai_addrlen);
        }

        *tail = copy;
        tail = &copy->ai_next;
    }

    real_freeaddrinfo(glibc_result);
    if (android_result != NULL)
        *android_result = (struct addrinfo *)head;
    return 0;
}

void freeaddrinfo(struct addrinfo *android_result)
{
    struct android_addrinfo *entry =
        (struct android_addrinfo *)android_result;

    while (entry != NULL) {
        struct android_addrinfo *next = entry->ai_next;
        free(entry->ai_canonname);
        free(entry->ai_addr);
        free(entry);
        entry = next;
    }
}

/* The Android library does not need logging for operation.  Keep a useful
 * opt-in diagnostic without making normal CLI output noisy. */
int __android_log_print(int priority, const char *tag, const char *format, ...)
{
    (void)priority;
    (void)tag;
    (void)format;
    return 0;
}
