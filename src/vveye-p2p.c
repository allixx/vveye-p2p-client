#include "libt2u.h"

#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef RTLD_DEEPBIND
#define RTLD_DEEPBIND 0
#endif

#define DEFAULT_SERVER_PORT 8000
#define DEFAULT_STATUS_INTERVAL_MS 1000
#define DEFAULT_TIMEOUT_SEC 60

static volatile sig_atomic_t stop_requested;
static volatile sig_atomic_t signal_received;

static void on_signal(int signo)
{
    signal_received = signo;
    stop_requested = 1;
}

static void sleep_ms(unsigned int milliseconds)
{
    struct timespec request;
    struct timespec remaining;

    request.tv_sec = milliseconds / 1000U;
    request.tv_nsec = (long)(milliseconds % 1000U) * 1000000L;
    while (!stop_requested && nanosleep(&request, &remaining) != 0 &&
           errno == EINTR) {
        request = remaining;
    }
}

typedef struct {
    const char *uid;
    const char *password;
    const char *remote_ip;
    const char *server;
    const char *server_key;
    const char *library_path;
    const char *compat_path;
    unsigned short server_port;
    unsigned short remote_port;
    unsigned short local_port;
    unsigned short range_min;
    unsigned short range_max;
    unsigned int timeout_sec;
    unsigned int interval_ms;
    bool skip_query;
    bool verbose;
    bool no_compat;
} options_t;

typedef struct {
    void *handle;
    void (*init)(const char *, unsigned short, const char *);
    void (*set_port_range)(unsigned short, unsigned short);
    int (*status)(void);
    int (*query)(const char *);
    int (*add_port)(const char *, unsigned short, unsigned short);
    int (*add_port_ex)(const char *, const char *, unsigned short, unsigned short);
    int (*add_port_v3)(const char *, const char *, const char *, unsigned short,
                       unsigned short);
    void (*del_port)(unsigned short);
    int (*port_status)(unsigned short, T2uNetStat *);
    void (*exit)(void);
} api_t;

static void usage(FILE *stream, const char *program)
{
    fprintf(stream,
        "Usage: %s --server HOST --uid UID --remote-port PORT [options]\n"
        "\n"
        "Create a VVEye TCP mapping.  The library opens the local listener.\n"
        "\n"
        "Required:\n"
        "  --uid UID                Remote device UID\n"
        "  --remote-port PORT       Destination TCP port on the device\n"
        "  --server HOST            Relay/signalling server\n"
        "\n"
        "Mapping options:\n"
        "  --local-port PORT        Local listener (0 means choose one)\n"
        "  --server-port PORT       Relay server port (default: %u)\n"
        "  --server-key KEY         Optional server key\n"
        "  --password PASSWORD      Device P2P password; uses the v3 API\n"
        "  --remote-ip ADDRESS      Device-side destination (v3/ex API)\n"
        "  --port-range MIN-MAX     UDP port range used by the SDK\n"
        "  --skip-query             Do not query device status before mapping\n"
        "\n"
        "Runtime/options:\n"
        "  --timeout SECONDS        Initial connection timeout (default: %u)\n"
        "  --interval MILLISECONDS  Status polling interval (default: %u)\n"
        "  --library PATH           libt2u.so path\n"
        "  --compat PATH            Bionic compatibility shim (liblog.so)\n"
        "  --no-compat              Do not load the compatibility shim\n"
        "  --verbose                Print status and traffic stats once per minute\n"
        "  --help                   Show this help\n"
        "\n"
        "The password can also be supplied as VV_P2P_PASSWORD.\n",
        program, DEFAULT_SERVER_PORT, DEFAULT_TIMEOUT_SEC,
        DEFAULT_STATUS_INTERVAL_MS);
}

static bool parse_unsigned(const char *text, unsigned long min,
                           unsigned long max, unsigned long *value)
{
    char *end = NULL;
    unsigned long parsed;

    if (text == NULL || *text == '\0' || text[0] == '-')
        return false;
    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed < min ||
        parsed > max)
        return false;
    *value = parsed;
    return true;
}

static bool parse_port(const char *text, unsigned short *port,
                       bool allow_zero)
{
    unsigned long value;
    unsigned long min = allow_zero ? 0 : 1;
    if (!parse_unsigned(text, min, 65535, &value))
        return false;
    *port = (unsigned short)value;
    return true;
}

static bool parse_range(const char *text, unsigned short *min_port,
                        unsigned short *max_port)
{
    char *copy;
    char *dash;
    bool result;

    if (text == NULL)
        return false;
    copy = strdup(text);
    if (copy == NULL)
        return false;
    dash = strchr(copy, '-');
    if (dash == NULL) {
        free(copy);
        return false;
    }
    *dash = '\0';
    result = parse_port(copy, min_port, false) &&
             parse_port(dash + 1, max_port, false) &&
             *min_port <= *max_port;
    free(copy);
    return result;
}

static bool take_value(int argc, char **argv, int *index, const char **value,
                       const char *option)
{
    if (*index + 1 >= argc) {
        fprintf(stderr, "error: %s requires a value\n", option);
        return false;
    }
    *value = argv[++*index];
    return true;
}

static bool parse_options(int argc, char **argv, options_t *options)
{
    memset(options, 0, sizeof(*options));
    options->server_port = DEFAULT_SERVER_PORT;
    options->local_port = 0;
    options->timeout_sec = DEFAULT_TIMEOUT_SEC;
    options->interval_ms = DEFAULT_STATUS_INTERVAL_MS;
    options->password = getenv("VV_P2P_PASSWORD");
    options->server_key = getenv("VV_P2P_SERVER_KEY");
    if (options->server_key == NULL)
        options->server_key = "";

    for (int i = 1; i < argc; ++i) {
        const char *value;
        unsigned long number;

        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(stdout, argv[0]);
            exit(0);
        } else if (strcmp(argv[i], "--verbose") == 0) {
            options->verbose = true;
        } else if (strcmp(argv[i], "--skip-query") == 0) {
            options->skip_query = true;
        } else if (strcmp(argv[i], "--no-compat") == 0) {
            options->no_compat = true;
        } else if (strcmp(argv[i], "--uid") == 0) {
            if (!take_value(argc, argv, &i, &options->uid, "--uid"))
                return false;
        } else if (strcmp(argv[i], "--remote-port") == 0) {
            if (!take_value(argc, argv, &i, &value, "--remote-port"))
                return false;
            if (!parse_port(value, &options->remote_port, false)) {
                fprintf(stderr, "error: invalid remote port: %s\n", value);
                return false;
            }
        } else if (strcmp(argv[i], "--local-port") == 0) {
            if (!take_value(argc, argv, &i, &value, "--local-port"))
                return false;
            if (!parse_port(value, &options->local_port, true)) {
                fprintf(stderr, "error: invalid local port: %s\n", value);
                return false;
            }
        } else if (strcmp(argv[i], "--server") == 0) {
            if (!take_value(argc, argv, &i, &options->server, "--server"))
                return false;
        } else if (strcmp(argv[i], "--server-port") == 0) {
            if (!take_value(argc, argv, &i, &value, "--server-port"))
                return false;
            if (!parse_port(value, &options->server_port, false)) {
                fprintf(stderr, "error: invalid server port: %s\n", value);
                return false;
            }
        } else if (strcmp(argv[i], "--server-key") == 0) {
            if (!take_value(argc, argv, &i, &options->server_key,
                            "--server-key"))
                return false;
        } else if (strcmp(argv[i], "--password") == 0) {
            if (!take_value(argc, argv, &i, &options->password,
                            "--password"))
                return false;
        } else if (strcmp(argv[i], "--remote-ip") == 0) {
            if (!take_value(argc, argv, &i, &options->remote_ip,
                            "--remote-ip"))
                return false;
        } else if (strcmp(argv[i], "--port-range") == 0) {
            if (!take_value(argc, argv, &i, &value, "--port-range"))
                return false;
            if (!parse_range(value, &options->range_min,
                             &options->range_max)) {
                fprintf(stderr, "error: invalid port range\n");
                return false;
            }
        } else if (strcmp(argv[i], "--timeout") == 0) {
            if (!take_value(argc, argv, &i, &value, "--timeout") ||
                !parse_unsigned(value, 1, UINT_MAX, &number)) {
                fprintf(stderr, "error: invalid timeout\n");
                return false;
            }
            options->timeout_sec = (unsigned int)number;
        } else if (strcmp(argv[i], "--interval") == 0) {
            if (!take_value(argc, argv, &i, &value, "--interval") ||
                !parse_unsigned(value, 50, 3600000, &number)) {
                fprintf(stderr, "error: invalid polling interval\n");
                return false;
            }
            options->interval_ms = (unsigned int)number;
        } else if (strcmp(argv[i], "--library") == 0) {
            if (!take_value(argc, argv, &i, &options->library_path,
                            "--library"))
                return false;
        } else if (strcmp(argv[i], "--compat") == 0) {
            if (!take_value(argc, argv, &i, &options->compat_path,
                            "--compat"))
                return false;
        } else {
            fprintf(stderr, "error: unknown option: %s\n", argv[i]);
            usage(stderr, argv[0]);
            return false;
        }
    }

    if (options->uid == NULL || options->uid[0] == '\0') {
        fprintf(stderr, "error: --uid is required\n");
        return false;
    }
    if (options->remote_port == 0) {
        fprintf(stderr, "error: --remote-port is required\n");
        return false;
    }
    if (options->server == NULL || options->server[0] == '\0') {
        fprintf(stderr, "error: --server is required\n");
        return false;
    }
    if (options->remote_ip == NULL && options->password != NULL)
        options->remote_ip = "127.0.0.1";
    return true;
}

static bool path_is_file(const char *path)
{
    return path != NULL && access(path, R_OK) == 0;
}

static char *join_path(const char *left, const char *right)
{
    size_t left_len;
    size_t right_len;
    char *result;

    if (left == NULL || right == NULL)
        return NULL;
    left_len = strlen(left);
    right_len = strlen(right);
    result = malloc(left_len + right_len + 2);
    if (result == NULL)
        return NULL;
    memcpy(result, left, left_len);
    result[left_len] = '/';
    memcpy(result + left_len + 1, right, right_len + 1);
    return result;
}

static char *executable_dir(void)
{
    char buffer[PATH_MAX];
    ssize_t length = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    char *slash;

    if (length <= 0 || (size_t)length >= sizeof(buffer))
        return strdup(".");
    buffer[length] = '\0';
    slash = strrchr(buffer, '/');
    if (slash == NULL)
        return strdup(".");
    *slash = '\0';
    return strdup(buffer[0] == '\0' ? "/" : buffer);
}

static char *choose_path(const char *explicit_path, const char *env_name,
                         const char *base, const char *name)
{
    char *candidate;
    const char *env_path;

    if (explicit_path != NULL)
        return strdup(explicit_path);
    env_path = getenv(env_name);
    if (env_path != NULL && *env_path != '\0')
        return strdup(env_path);

    candidate = join_path(base, name);
    if (path_is_file(candidate))
        return candidate;
    free(candidate);

    candidate = join_path(base, "vendor");
    if (candidate != NULL) {
        char *nested = join_path(candidate, name);
        free(candidate);
        if (path_is_file(nested))
            return nested;
        free(nested);
    }

    candidate = join_path(base, "..");
    if (candidate != NULL) {
        char *parent_vendor = join_path(candidate, "vendor");
        free(candidate);
        if (parent_vendor != NULL) {
            char *nested = join_path(parent_vendor, name);
            free(parent_vendor);
            if (path_is_file(nested))
                return nested;
            free(nested);
        }
    }

    return strdup(name);
}

static bool add_library_directory_to_search_path(const char *path)
{
    char buffer[PATH_MAX];
    const char *old = getenv("LD_LIBRARY_PATH");
    const char *slash = strrchr(path, '/');
    size_t directory_length;
    int written;

    if (slash == NULL)
        return true;
    directory_length = (size_t)(slash - path);
    if (directory_length == 0)
        directory_length = 1;
    if (directory_length >= sizeof(buffer))
        return false;
    memcpy(buffer, path, directory_length);
    buffer[directory_length] = '\0';
    written = snprintf(buffer + directory_length,
                       sizeof(buffer) - directory_length, ":%s",
                       old != NULL ? old : "");
    if (written < 0 || (size_t)written >= sizeof(buffer) - directory_length)
        return false;
    return setenv("LD_LIBRARY_PATH", buffer, 1) == 0;
}

#define LOAD_REQUIRED(api, field, symbol)                                      \
    do {                                                                       \
        *(void **)(&(api)->field) = dlsym((api)->handle, (symbol));             \
        if ((api)->field == NULL) {                                            \
            fprintf(stderr, "error: libt2u is missing %s: %s\n",             \
                    (symbol), dlerror());                                     \
            return false;                                                      \
        }                                                                      \
    } while (0)

static bool load_api(api_t *api, const char *library_path)
{
    const char *error;
    int flags = RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND;

    memset(api, 0, sizeof(*api));
    api->handle = dlopen(library_path, flags);
    if (api->handle == NULL) {
        error = dlerror();
        fprintf(stderr, "error: cannot load %s: %s\n", library_path,
                error != NULL ? error : "unknown loader error");
        return false;
    }

    LOAD_REQUIRED(api, init, "t2u_init");
    LOAD_REQUIRED(api, set_port_range, "t2u_set_port_range");
    LOAD_REQUIRED(api, status, "t2u_status");
    LOAD_REQUIRED(api, query, "t2u_query");
    LOAD_REQUIRED(api, add_port, "t2u_add_port");
    LOAD_REQUIRED(api, add_port_ex, "t2u_add_port_ex");
    LOAD_REQUIRED(api, add_port_v3, "t2u_add_port_v3");
    LOAD_REQUIRED(api, del_port, "t2u_del_port");
    LOAD_REQUIRED(api, port_status, "t2u_port_status");
    LOAD_REQUIRED(api, exit, "t2u_exit");
    return true;
}

static void close_api(api_t *api)
{
    if (api->handle != NULL)
        dlclose(api->handle);
    memset(api, 0, sizeof(*api));
}

static bool wait_for_server(const api_t *api, const options_t *options)
{
    unsigned long elapsed_ms = 0;
    int last_status = INT_MIN;

    while (!stop_requested) {
        int status = api->status();
        if (options->verbose || status != last_status) {
            fprintf(stderr, "server status: %d%s\n", status,
                    status == 1 ? " (connected)" : "");
            last_status = status;
        }
        if (status == 1)
            return true;
        if (status == -2) {
            fprintf(stderr, "error: relay server rejected the server key\n");
            return false;
        }
        if (elapsed_ms >= (unsigned long)options->timeout_sec * 1000UL) {
            fprintf(stderr, "error: timed out waiting for relay server\n");
            return false;
        }
        sleep_ms(options->interval_ms);
        elapsed_ms += options->interval_ms;
    }
    return false;
}

static const char *mapping_status_text(int status)
{
    switch (status) {
    case 1: return "connected";
    case 0: return "connecting";
    case -1: return "unknown device or mapping";
    case -2: return "P2P connection failed";
    case -3: return "P2P connection interrupted";
    case -4: return "device offline";
    case -5: return "device password rejected";
    default: return "unknown status";
    }
}

static void print_mapping_status(int status, unsigned short local_port,
                                 const T2uNetStat *network)
{
    if (status == 1 && network != NULL) {
        const char *proxy_text = network->proxy == 0 ? "no" : "yes";
        fprintf(stderr,
                "mapping %u: connected; peer=%.*s:%d proxy=%s (%d) "
                "lost=%.2f%% bandwidth=%d KB/s\n",
                local_port, (int)sizeof(network->ip), network->ip,
                network->port, proxy_text, network->proxy,
                network->lost_rate, network->bandwidth);
    } else {
        fprintf(stderr, "mapping %u: %s (%d)\n", local_port,
                mapping_status_text(status), status);
    }
}

int main(int argc, char **argv)
{
    options_t options;
    api_t api;
    char *exe_dir = NULL;
    char *library_path = NULL;
    char *compat_path = NULL;
    void *compat_handle = NULL;
    unsigned short mapped_port = 0;
    int mapping_result = 0;
    int last_mapping_status = INT_MIN;
    unsigned long elapsed_ms = 0;
    unsigned long connected_elapsed_ms = 0;
    unsigned long next_stats_ms = 60000;
    int exit_code = 1;
    struct sigaction action;

    if (!parse_options(argc, argv, &options)) {
        usage(stderr, argv[0]);
        return 1;
    }

    memset(&action, 0, sizeof(action));
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) != 0 ||
        sigaction(SIGTERM, &action, NULL) != 0) {
        perror("sigaction");
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);

    exe_dir = executable_dir();
    library_path = choose_path(options.library_path, "VV_P2P_LIBRARY",
                               exe_dir, "libt2u.so");
    compat_path = choose_path(options.compat_path, "VV_P2P_COMPAT",
                              exe_dir, "liblog.so");
    if (library_path == NULL || compat_path == NULL) {
        fprintf(stderr, "error: out of memory resolving library paths\n");
        goto cleanup;
    }

    add_library_directory_to_search_path(library_path);
    if (!options.no_compat) {
        add_library_directory_to_search_path(compat_path);
        compat_handle = dlopen(compat_path, RTLD_NOW | RTLD_GLOBAL);
        if (compat_handle == NULL && options.verbose) {
            fprintf(stderr, "compatibility shim not loaded from %s: %s\n",
                    compat_path, dlerror());
        }
    }

    if (!load_api(&api, library_path))
        goto cleanup;

    fprintf(stderr, "initializing VVEye client: server=%s:%u\n",
            options.server, options.server_port);
    api.init(options.server, options.server_port, options.server_key);
    if (options.range_min != 0)
        api.set_port_range(options.range_min, options.range_max);

    if (!wait_for_server(&api, &options))
        goto shutdown;

    if (!options.skip_query) {
        int online = api.query(options.uid);
        fprintf(stderr, "device query: %d%s\n", online,
                online == 1 ? " (online)" : "");
        if (online != 1) {
            fprintf(stderr, "error: device is not reported online\n");
            goto shutdown;
        }
    }

    if (options.password != NULL && options.password[0] != '\0') {
        const char *remote_ip = options.remote_ip != NULL
                                    ? options.remote_ip : "127.0.0.1";
        mapping_result = api.add_port_v3(
            options.uid, options.password, remote_ip, options.remote_port,
            options.local_port);
    } else if (options.remote_ip != NULL) {
        mapping_result = api.add_port_ex(
            options.uid, options.remote_ip, options.remote_port,
            options.local_port);
    } else {
        mapping_result = api.add_port(
            options.uid, options.remote_port, options.local_port);
    }

    if (mapping_result <= 0) {
        fprintf(stderr, "error: cannot create local mapping (return=%d)\n",
                mapping_result);
        goto shutdown;
    }
    mapped_port = (unsigned short)mapping_result;

    fprintf(stderr, "mapping created: 127.0.0.1:%u -> %s:%u\n", mapped_port,
            options.uid, options.remote_port);

    while (!stop_requested) {
        T2uNetStat network;
        int status;
        memset(&network, 0, sizeof(network));
        status = api.port_status(mapped_port, &network);
        if (options.verbose || status != last_mapping_status) {
            print_mapping_status(status, mapped_port, &network);
            last_mapping_status = status;
        }
        if (status == 1)
            break;
        if (elapsed_ms >= (unsigned long)options.timeout_sec * 1000UL) {
            fprintf(stderr, "error: timed out waiting for mapping\n");
            goto shutdown;
        }
        sleep_ms(options.interval_ms);
        elapsed_ms += options.interval_ms;
    }

    if (stop_requested)
        goto shutdown;

    fprintf(stderr, "press Ctrl-C to stop\n");
    while (!stop_requested) {
        T2uNetStat network;
        int status;
        memset(&network, 0, sizeof(network));
        status = api.port_status(mapped_port, &network);
        if (status != 1) {
            print_mapping_status(status, mapped_port, &network);
            fprintf(stderr, "error: mapping disconnected\n");
            goto shutdown;
        }
        if (options.verbose && connected_elapsed_ms >= next_stats_ms) {
            print_mapping_status(status, mapped_port, &network);
            next_stats_ms += 60000;
        }
        sleep_ms(options.interval_ms);
        connected_elapsed_ms += options.interval_ms;
    }

    exit_code = 0;

shutdown:
    if (stop_requested) {
        fprintf(stderr, "received %s; closing mapping and VVEye session\n",
                signal_received == SIGINT ? "SIGINT" : "SIGTERM");
    }
    if (mapped_port != 0)
        api.del_port(mapped_port);
    api.exit();
    close_api(&api);

cleanup:
    if (compat_handle != NULL)
        dlclose(compat_handle);
    free(compat_path);
    free(library_path);
    free(exe_dir);
    return exit_code;
}
