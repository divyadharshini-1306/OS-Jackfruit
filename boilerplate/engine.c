/*

* engine.c - Supervised Multi-Container Runtime (User Space)

*

* Intentionally partial starter:

*   - command-line shape is defined

*   - key runtime data structures are defined

*   - bounded-buffer skeleton is defined

*   - supervisor / client split is outlined

*

* Students are expected to design:

*   - the control-plane IPC implementation

*   - container lifecycle and metadata synchronization

*   - clone + namespace setup for each container

*   - producer/consumer behavior for log buffering

*   - signal handling and graceful shutdown

*/

 

#define _GNU_SOURCE

#include <errno.h>

#include <fcntl.h>

#include <limits.h>

#include <pthread.h>

#include <sched.h>

#include <signal.h>

#include <stdio.h>

#include <stdlib.h>

#include <string.h>

#include <sys/ioctl.h>

#include <sys/mount.h>

#include <sys/socket.h>

#include <sys/stat.h>

#include <sys/types.h>

#include <sys/un.h>

#include <sys/wait.h>

#include <time.h>

#include <unistd.h>

 

#include "monitor_ioctl.h"

 

#define STACK_SIZE (1024 * 1024)

#define CONTAINER_ID_LEN 32

#define CONTROL_PATH "/tmp/mini_runtime.sock"

#define LOG_DIR "logs"

#define CONTROL_MESSAGE_LEN 256

#define CHILD_COMMAND_LEN 256

#define LOG_CHUNK_SIZE 4096

#define LOG_BUFFER_CAPACITY 16

#define DEFAULT_SOFT_LIMIT (40UL << 20)

#define DEFAULT_HARD_LIMIT (64UL << 20)

 

typedef enum {

    CMD_SUPERVISOR = 0,

    CMD_START,

    CMD_RUN,

    CMD_PS,

    CMD_LOGS,

    CMD_STOP

} command_kind_t;

 

typedef enum {

    CONTAINER_STARTING = 0,

    CONTAINER_RUNNING,

    CONTAINER_STOPPED,

    CONTAINER_KILLED,

    CONTAINER_EXITED

} container_state_t;

 

typedef struct container_record {

    char id[CONTAINER_ID_LEN];

    pid_t host_pid;

    time_t started_at;

    container_state_t state;

    unsigned long soft_limit_bytes;

    unsigned long hard_limit_bytes;

    int exit_code;

    int exit_signal;

    char log_path[PATH_MAX];

    int log_read_fd;

    void *stack;                 // ADD THIS LINE

    int waiting_client_fd;       // ADD THIS LINE

    struct container_record *next;

} container_record_t;

 

typedef struct {

    char container_id[CONTAINER_ID_LEN];

    size_t length;

    char data[LOG_CHUNK_SIZE];

} log_item_t;

 

typedef struct {

    log_item_t items[LOG_BUFFER_CAPACITY];

    size_t head;

    size_t tail;

    size_t count;

    int shutting_down;

    pthread_mutex_t mutex;

    pthread_cond_t not_empty;

    pthread_cond_t not_full;

} bounded_buffer_t;

 

typedef struct {

    command_kind_t kind;

    char container_id[CONTAINER_ID_LEN];

    char rootfs[PATH_MAX];

    char command[CHILD_COMMAND_LEN];

    unsigned long soft_limit_bytes;

    unsigned long hard_limit_bytes;

    int nice_value;

} control_request_t;

 

typedef struct {

    int status;

    char message[CONTROL_MESSAGE_LEN];

} control_response_t;

 

typedef struct {

    char id[CONTAINER_ID_LEN];

    char rootfs[PATH_MAX];

    char command[CHILD_COMMAND_LEN];

    int nice_value;

    int log_write_fd;

} child_config_t;

 

typedef struct {

    int server_fd;

    int monitor_fd;

    int should_stop;

    pthread_t logger_thread;

    bounded_buffer_t log_buffer;

    pthread_mutex_t metadata_lock;

    container_record_t *containers;

} supervisor_ctx_t;

 

static supervisor_ctx_t *g_ctx = NULL;

 

static void signal_handler(int signum)

{

    if (!g_ctx)

        return;

 

    if (signum == SIGINT || signum == SIGTERM) {

        printf("\nSupervisor caught signal %d, initiating shutdown.\n", signum);

        g_ctx->should_stop = 1;

    } else if (signum == SIGCHLD) {

        int status;

        pid_t pid;

 

        while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {

            pthread_mutex_lock(&g_ctx->metadata_lock);

 

            for (container_record_t *c = g_ctx->containers; c != NULL; c = c->next) {

                if (c->host_pid == pid) {

                    // Logic for a normally exited container

                    if (WIFEXITED(status)) {

                        c->state = CONTAINER_EXITED;

                        c->exit_code = WEXITSTATUS(status);

                        printf("Container '%s' (PID %d) exited with code %d.\n", c->id, pid, c->exit_code);

 

                        // If a 'run' client was waiting, notify it.

                        if (c->waiting_client_fd >= 0) {

                            control_response_t final_resp;

                            memset(&final_resp, 0, sizeof(final_resp));

                            final_resp.status = c->exit_code;

                            snprintf(final_resp.message, sizeof(final_resp.message), "Container exited with code %d.", c->exit_code);

                            write(c->waiting_client_fd, &final_resp, sizeof(final_resp));

                            close(c->waiting_client_fd);

                            c->waiting_client_fd = -1;

                        }

                    }

                    // Logic for a container killed by a signal

                    else if (WIFSIGNALED(status)) {

                        c->state = CONTAINER_KILLED;

                        c->exit_signal = WTERMSIG(status);

                        printf("Container '%s' (PID %d) was killed by signal %d.\n", c->id, pid, c->exit_signal);

 

                        // If a 'run' client was waiting, notify it.

                        if (c->waiting_client_fd >= 0) {

                            control_response_t final_resp;

                            memset(&final_resp, 0, sizeof(final_resp));

                            final_resp.status = 128 + c->exit_signal;

                            snprintf(final_resp.message, sizeof(final_resp.message), "Container killed by signal %d.", c->exit_signal);

                            write(c->waiting_client_fd, &final_resp, sizeof(final_resp));

                            close(c->waiting_client_fd);

                            c->waiting_client_fd = -1;

                        }

                    }

 

                    // General resource cleanup

                    unregister_from_monitor(g_ctx->monitor_fd, c->id, c->host_pid);

                    if (c->stack) {

                        free(c->stack);

                        c->stack = NULL;

                    }

 

                    break; // Found and processed the record, exit the loop.

                }

            }

            pthread_mutex_unlock(&g_ctx->metadata_lock);

        }

    }

}

 

static void usage(const char *prog)

{

    fprintf(stderr,

            "Usage:\n"

            "  %s supervisor <base-rootfs>\n"

            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"

            "  %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"

            "  %s ps\n"

            "  %s logs <id>\n"

            "  %s stop <id>\n",

            prog, prog, prog, prog, prog, prog);

}

 

static int parse_mib_flag(const char *flag,

                          const char *value,

                          unsigned long *target_bytes)

{

    char *end = NULL;

    unsigned long mib;

 

    errno = 0;

    mib = strtoul(value, &end, 10);

    if (errno != 0 || end == value || *end != '\0') {

        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);

        return -1;

    }

 

    if (mib > ULONG_MAX / (1UL << 20)) {

        fprintf(stderr, "Value for %s is too large: %s\n", flag, value);

        return -1;

    }

 

    *target_bytes = mib * (1UL << 20);

    return 0;

}

 

static int parse_optional_flags(control_request_t *req,

                                int argc,

                                char *argv[],

                                int start_index)

{

    int i;

 

    for (i = start_index; i < argc; i += 2) {

        char *end = NULL;

        long nice_value;

 

        if (i + 1 >= argc) {

            fprintf(stderr, "Missing value for option: %s\n", argv[i]);

            return -1;

        }

 

        if (strcmp(argv[i], "--soft-mib") == 0) {

            if (parse_mib_flag("--soft-mib", argv[i + 1], &req->soft_limit_bytes) != 0)

                return -1;

            continue;

        }

 

        if (strcmp(argv[i], "--hard-mib") == 0) {

            if (parse_mib_flag("--hard-mib", argv[i + 1], &req->hard_limit_bytes) != 0)

                return -1;

            continue;

        }

 

        if (strcmp(argv[i], "--nice") == 0) {

            errno = 0;

            nice_value = strtol(argv[i + 1], &end, 10);

            if (errno != 0 || end == argv[i + 1] || *end != '\0' ||

                nice_value < -20 || nice_value > 19) {

                fprintf(stderr,

                        "Invalid value for --nice (expected -20..19): %s\n",

                        argv[i + 1]);

                return -1;

            }

            req->nice_value = (int)nice_value;

            continue;

        }

 

        fprintf(stderr, "Unknown option: %s\n", argv[i]);

        return -1;

    }

 

    if (req->soft_limit_bytes > req->hard_limit_bytes) {

        fprintf(stderr, "Invalid limits: soft limit cannot exceed hard limit\n");

        return -1;

    }

 

    return 0;

}

 

static const char *state_to_string(container_state_t state)

{

    switch (state) {

    case CONTAINER_STARTING:

        return "starting";

    case CONTAINER_RUNNING:

        return "running";

    case CONTAINER_STOPPED:

        return "stopped";

    case CONTAINER_KILLED:

        return "killed";

    case CONTAINER_EXITED:

        return "exited";

    default:

        return "unknown";

    }

}

 

static int bounded_buffer_init(bounded_buffer_t *buffer)

{

    int rc;

 

    memset(buffer, 0, sizeof(*buffer));

 

    rc = pthread_mutex_init(&buffer->mutex, NULL);

    if (rc != 0)

        return rc;

 

    rc = pthread_cond_init(&buffer->not_empty, NULL);

    if (rc != 0) {

        pthread_mutex_destroy(&buffer->mutex);

        return rc;

    }

 

    rc = pthread_cond_init(&buffer->not_full, NULL);

    if (rc != 0) {

        pthread_cond_destroy(&buffer->not_empty);

        pthread_mutex_destroy(&buffer->mutex);

        return rc;

    }

 

    return 0;

}

 

static void bounded_buffer_destroy(bounded_buffer_t *buffer)

{

    pthread_cond_destroy(&buffer->not_full);

    pthread_cond_destroy(&buffer->not_empty);

    pthread_mutex_destroy(&buffer->mutex);

}

 

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buffer)

{

    pthread_mutex_lock(&buffer->mutex);

    buffer->shutting_down = 1;

    pthread_cond_broadcast(&buffer->not_empty);

    pthread_cond_broadcast(&buffer->not_full);

    pthread_mutex_unlock(&buffer->mutex);

}

 

/*

* TODO:

* Implement producer-side insertion into the bounded buffer.

*

* Requirements:

*   - block or fail according to your chosen policy when the buffer is full

*   - wake consumers correctly

*   - stop cleanly if shutdown begins

*/

int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)

{

    pthread_mutex_lock(&buffer->mutex);

 

    while (buffer->count >= LOG_BUFFER_CAPACITY && !buffer->shutting_down) {

        pthread_cond_wait(&buffer->not_full, &buffer->mutex);

    }

 

    if (buffer->shutting_down) {

        pthread_mutex_unlock(&buffer->mutex);

        return -1; /* Indicate shutdown */

    }

 

    buffer->items[buffer->head] = *item;

    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;

    buffer->count++;

 

    pthread_cond_signal(&buffer->not_empty);

    pthread_mutex_unlock(&buffer->mutex);

 

    return 0;

}

 

/*

* TODO:

* Implement consumer-side removal from the bounded buffer.

*

* Requirements:

*   - wait correctly while the buffer is empty

*   - return a useful status when shutdown is in progress

*   - avoid races with producers and shutdown

*/

int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)

{

    pthread_mutex_lock(&buffer->mutex);

 

    while (buffer->count == 0 && !buffer->shutting_down) {

        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);

    }

 

    if (buffer->count == 0 && buffer->shutting_down) {

        pthread_mutex_unlock(&buffer->mutex);

        return -1; /* Indicate shutdown and buffer is empty */

    }

 

    *item = buffer->items[buffer->tail];

    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;

    buffer->count--;

 

    pthread_cond_signal(&buffer->not_full);

    pthread_mutex_unlock(&buffer->mutex);

 

    return 0;

}

 

/*

* TODO:

* Implement the logging consumer thread.

*

* Suggested responsibilities:

*   - remove log chunks from the bounded buffer

*   - route each chunk to the correct per-container log file

*   - exit cleanly when shutdown begins and pending work is drained

*/

void *logging_thread(void *arg)

{

    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;

    log_item_t item;

    FILE *log_files[128] = {0}; /* Simple map of FD to FILE* */

    char log_path[PATH_MAX];

 

    mkdir(LOG_DIR, 0755);

 

    while (1) {

        if (bounded_buffer_pop(&ctx->log_buffer, &item) != 0) {

            /* Buffer is empty and shutdown has begun. */

            break;

        }

 

        /* Find the container record to get the log file path. */

        /* This is a simplified approach; a hash map would be better for many containers. */

        pthread_mutex_lock(&ctx->metadata_lock);

        container_record_t *current = ctx->containers;

        while(current) {

            if (strcmp(current->id, item.container_id) == 0) {

                strncpy(log_path, current->log_path, sizeof(log_path) -1);

                break;

            }

            current = current->next;

        }

        pthread_mutex_unlock(&ctx->metadata_lock);

 

        if (!current) continue; /* Container might have been removed. */

 

        /* Open the log file on first write for this container. */

        int log_fd = -1;

        for (size_t i = 0; i < sizeof(log_files)/sizeof(log_files[0]); ++i) {

             if (log_files[i] && fileno(log_files[i]) != -1 && strcmp(current->log_path, log_path) == 0) {

                 log_fd = i;

                 break;

             }

        }

 

        if (log_fd == -1) {

             FILE* file = fopen(log_path, "a");

             if (file) {

                 for (size_t i = 0; i < sizeof(log_files)/sizeof(log_files[0]); ++i) {

                     if (!log_files[i]) {

                         log_files[i] = file;

                         log_fd = i;

                         break;

                     }

                 }

             }

        }

 

        if (log_fd != -1) {

            fwrite(item.data, 1, item.length, log_files[log_fd]);

            fflush(log_files[log_fd]);

        }

    }

 

    /* Clean up: close all open log files. */

    for (size_t i = 0; i < sizeof(log_files)/sizeof(log_files[0]); ++i) {

        if (log_files[i]) {

            fclose(log_files[i]);

        }

    }

 

    printf("Logging thread has shut down.\n");

    return NULL;

}

 

/*

* TODO:

* Implement the clone child entrypoint.

*

* Required outcomes:

*   - isolated PID / UTS / mount context

*   - chroot or pivot_root into rootfs

*   - working /proc inside container

*   - stdout / stderr redirected to the supervisor logging path

*   - configured command executed inside the container

*/

int child_fn(void *arg)

{

    child_config_t *config = (child_config_t *)arg;

 

    /* 1. Redirect stdout and stderr to the logging pipe */

    /*    The other end of this pipe is read by the supervisor. */

    if (dup2(config->log_write_fd, STDOUT_FILENO) < 0 ||

        dup2(config->log_write_fd, STDERR_FILENO) < 0) {

        perror("child: failed to dup2 log pipe");

        return 1;

    }

    /* We no longer need the original fd after dup2'ing it */

    close(config->log_write_fd);

 

    /* 2. Set a new hostname for the container (UTS namespace) */

    if (sethostname(config->id, strlen(config->id)) < 0) {

        perror("child: sethostname failed");

        return 1;

    }

 

    /* 3. Change root to the container's rootfs (Mount namespace) */

    if (chroot(config->rootfs) < 0) {

        perror("child: chroot failed");

        return 1;

    }

    /* Move to the new root directory */

    if (chdir("/") < 0) {

        perror("child: chdir to / failed");

        return 1;

    }

 

    /* 4. Mount a new /proc filesystem */

    if (mount("proc", "/proc", "proc", 0, NULL) < 0) {

        perror("child: failed to mount /proc");

        /* This is often non-fatal, but tools like 'ps' won't work */

    }

 

    /* 5. Set process priority if a nice value was specified */

    if (config->nice_value != 0) {

        if (nice(config->nice_value) < 0) {

            perror("child: failed to set nice value");

            /* Non-fatal, continue execution */

        }

    }

    /* 6. Execute the user's command via a shell.

          This robustly handles complex commands with quotes and spaces. */

    execl("/bin/sh", "sh", "-c", config->command, (char *) NULL);

 

    /* If execl returns, it means an error occurred. */

    fprintf(stderr, "child: execl failed for command '%s'\n", config->command);

    perror("execl");

    return 1;

}

 

int register_with_monitor(int monitor_fd,

                          const char *container_id,

                          pid_t host_pid,

                          unsigned long soft_limit_bytes,

                          unsigned long hard_limit_bytes)

{

    struct monitor_request req;

 

    memset(&req, 0, sizeof(req));

    req.pid = host_pid;

    req.soft_limit_bytes = soft_limit_bytes;

    req.hard_limit_bytes = hard_limit_bytes;

    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

 

    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)

        return -1;

 

    return 0;

}

 

int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid)

{

    struct monitor_request req;

 

    memset(&req, 0, sizeof(req));

    req.pid = host_pid;

    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

 

    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)

        return -1;

 

    return 0;

}

 

/*

* TODO:

* Implement the long-running supervisor process.

*

* Suggested responsibilities:

*   - create and bind the control-plane IPC endpoint

*   - initialize shared metadata and the bounded buffer

*   - start the logging thread

*   - accept control requests and update container state

*   - reap children and respond to signals

*/

static int handle_start_request(supervisor_ctx_t *ctx, const control_request_t *req, control_response_t *resp, int waiting_fd)

{

    char *stack;

    char *stack_top;

    pid_t child_pid;

    child_config_t child_config;

    int log_pipe[2];

    container_record_t *record;

 

    /* TODO: Add check to prevent duplicate container IDs. */

 

    /* 1. Create a pipe for the child's logs. */

    if (pipe(log_pipe) < 0) {

        perror("pipe");

        snprintf(resp->message, sizeof(resp->message), "Failed to create log pipe.");

        return -1;

    }

 

    /* 2. Allocate memory for the child's stack. */

    stack = malloc(STACK_SIZE);

    if (!stack) {

        perror("malloc stack");

        snprintf(resp->message, sizeof(resp->message), "Failed to allocate stack.");

        close(log_pipe[0]);

        close(log_pipe[1]);

        return -1;

    }

    stack_top = stack + STACK_SIZE; /* Stack grows downwards */

 

    /* 3. Configure the child process. */

    memset(&child_config, 0, sizeof(child_config));

    strncpy(child_config.id, req->container_id, sizeof(child_config.id) - 1);

    strncpy(child_config.rootfs, req->rootfs, sizeof(child_config.rootfs) - 1);

    strncpy(child_config.command, req->command, sizeof(child_config.command) - 1);

    child_config.nice_value = req->nice_value;

    child_config.log_write_fd = log_pipe[1]; /* Pass the write end to the child */

 

    /* 4. Create the child process using clone(). */

    const int flags = CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | SIGCHLD;

    child_pid = clone(child_fn, stack_top, flags, &child_config);

 

    /* --- This is the Supervisor (Parent) Path --- */

 

    /* Always close the write end of the pipe in the parent. */

    close(log_pipe[1]);

 

    if (child_pid < 0) {

        perror("clone");

        snprintf(resp->message, sizeof(resp->message), "Failed to clone child process.");

        free(stack);

        close(log_pipe[0]); /* Also close the read end on failure. */

        return -1;

    }

 

    printf("Supervisor created container '%s' with host PID %d\n", req->container_id, child_pid);

 

    /* 5. Create and store metadata for the new container. */

    record = malloc(sizeof(*record));

    if (!record) {

        kill(child_pid, SIGKILL);

        snprintf(resp->message, sizeof(resp->message), "Failed to allocate record.");

        free(stack); // Use the variable, not the record field yet

        close(log_pipe[0]);

        return -1;

    }

    memset(record, 0, sizeof(*record));

    // Initialize new fields

    record->stack = stack;               // STORE THE STACK POINTER

    record->waiting_client_fd = -1;      // Default to no waiting client

 

    // At the end of the function, before returning:

    record->log_read_fd = log_pipe[0];

    record->waiting_client_fd = waiting_fd; // ASSIGN THE WAITING CLIENT FD

 

    snprintf(record->log_path, sizeof(record->log_path), "%s/%s.log", LOG_DIR, record->id);

 

    strncpy(record->id, req->container_id, sizeof(record->id) - 1);

    record->host_pid = child_pid;

    record->started_at = time(NULL);

    record->state = CONTAINER_RUNNING;

    record->soft_limit_bytes = req->soft_limit_bytes;

    record->hard_limit_bytes = req->hard_limit_bytes;

    record->log_read_fd = log_pipe[0]; /* Store the read end of the pipe */

 

    /* Construct the log file path and store it */

    snprintf(record->log_path, sizeof(record->log_path), "%s/%s.log", LOG_DIR, record->id);

 

    pthread_mutex_lock(&ctx->metadata_lock);

    record->next = ctx->containers;

    ctx->containers = record;

    pthread_mutex_unlock(&ctx->metadata_lock);

 

    /* 6. Register the container with the kernel monitor. */

    if (register_with_monitor(ctx->monitor_fd, record->id, record->host_pid,

                              record->soft_limit_bytes, record->hard_limit_bytes) < 0) {

        perror("register_with_monitor");

        /* Non-fatal, but log it. The container will run without memory limits. */

    }

 

    snprintf(resp->message, sizeof(resp->message), "Successfully started container '%s'", req->container_id);

    /* Note: 'stack' is intentionally not freed here. The child process owns it. */

    /*       It will be freed when the process exits. We are "leaking" it on purpose. */

    return 0;

}

 

static void handle_logs_request(supervisor_ctx_t *ctx, const control_request_t *req, int client_fd)

{

    char log_path[PATH_MAX] = {0};

    int found = 0;

 

    // Find the container record to get its log path.

    pthread_mutex_lock(&ctx->metadata_lock);

    for (container_record_t *c = ctx->containers; c != NULL; c = c->next) {

        if (strcmp(c->id, req->container_id) == 0) {

            strncpy(log_path, c->log_path, sizeof(log_path) - 1);

            found = 1;

            break;

        }

    }

    pthread_mutex_unlock(&ctx->metadata_lock);

 

    if (!found) {

        char msg[128];

        snprintf(msg, sizeof(msg), "Error: Container '%s' not found.\n", req->container_id);

        write(client_fd, msg, strlen(msg));

        close(client_fd);

        return;

    }

 

    FILE *log_file = fopen(log_path, "r");

    if (!log_file) {

        char msg[128];

        snprintf(msg, sizeof(msg), "Error: Could not open log file for '%s'.\n", req->container_id);

        write(client_fd, msg, strlen(msg));

        close(client_fd);

        return;

    }

 

    // Stream the file contents to the client.

    char buffer[4096];

    size_t bytes_read;

    while ((bytes_read = fread(buffer, 1, sizeof(buffer), log_file)) > 0) {

        if (write(client_fd, buffer, bytes_read) < 0) {

            // Client disconnected, stop sending.

            break;

        }

    }

 

    fclose(log_file);

    close(client_fd);

}

 

static int handle_stop_request(supervisor_ctx_t *ctx, const control_request_t *req, control_response_t *resp)

{

    container_record_t *record = NULL;

    int found = 0;

 

    pthread_mutex_lock(&ctx->metadata_lock);

    for (record = ctx->containers; record != NULL; record = record->next) {

        if (strcmp(record->id, req->container_id) == 0) {

            found = 1;

            break;

        }

    }

 

    if (!found) {

        pthread_mutex_unlock(&ctx->metadata_lock);

        snprintf(resp->message, sizeof(resp->message), "Container '%s' not found.", req->container_id);

        return -1;

    }

 

    // Send the termination signal

    if (kill(record->host_pid, SIGTERM) < 0) {

        pthread_mutex_unlock(&ctx->metadata_lock);

        perror("kill");

        snprintf(resp->message, sizeof(resp->message), "Failed to send SIGTERM to container '%s'.", req->container_id);

        return -1;

    }

 

    record->state = CONTAINER_STOPPED; // Update state

    pthread_mutex_unlock(&ctx->metadata_lock);

 

    snprintf(resp->message, sizeof(resp->message), "Sent SIGTERM to container '%s'.", req->container_id);

    printf("Stopping container '%s' (PID %d)\n", record->id, record->host_pid);

 

    return 0;

}

 

static int run_supervisor(const char *rootfs)

{

    supervisor_ctx_t ctx;

    struct sockaddr_un addr;

    int rc = 1; /* Default to error exit code */

 

    (void)rootfs; /* Not used in this step, but will be later */

 

    memset(&ctx, 0, sizeof(ctx));

    ctx.server_fd = -1;

    ctx.monitor_fd = -1;

    g_ctx = &ctx; /* Set global context for signal handler */

 

    if (pthread_mutex_init(&ctx.metadata_lock, NULL) != 0 ||

        bounded_buffer_init(&ctx.log_buffer) != 0) {

        perror("init sync primitives");

        return 1;

    }

 

    /* 1) Open /dev/container_monitor */

    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);

    if (ctx.monitor_fd < 0) {

        perror("open /dev/container_monitor");

        goto cleanup;

    }

 

    /* 2) Create the control socket */

    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);

    if (ctx.server_fd < 0) {

        perror("socket");

        goto cleanup;

    }

 

    memset(&addr, 0, sizeof(addr));

    addr.sun_family = AF_UNIX;

    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

 

    /* Remove old socket file if it exists */

    unlink(CONTROL_PATH);

 

    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {

        perror("bind");

        goto cleanup;

    }

 

    if (listen(ctx.server_fd, 5) < 0) {

        perror("listen");

        goto cleanup;

    }

 

    printf("Supervisor listening on %s\n", CONTROL_PATH);

 

    /* 3) Install signal handlers */

    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));

    sa.sa_handler = signal_handler;

    sigaction(SIGINT, &sa, NULL);

    sigaction(SIGTERM, &sa, NULL);

    sigaction(SIGCHLD, &sa, NULL);

    signal(SIGPIPE, SIG_IGN);

 

    /* 4) TODO: Spawn the logger thread (in a later step) */

    if (mkdir(LOG_DIR, 0755) < 0 && errno != EEXIST) {

        perror("mkdir logs");

        goto cleanup;

    }

    if (pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx) != 0) {

        perror("pthread_create for logger");

        goto cleanup;

    }

 

    /* 5) Enter the supervisor event loop */

    while (!ctx.should_stop) {

        fd_set read_fds;

        int max_fd = ctx.server_fd;

 

        FD_ZERO(&read_fds);

        FD_SET(ctx.server_fd, &read_fds);

 

        pthread_mutex_lock(&ctx.metadata_lock);

        container_record_t *current = ctx.containers;

        while (current) {

            if (current->log_read_fd >= 0) {

                FD_SET(current->log_read_fd, &read_fds);

                if (current->log_read_fd > max_fd) {

                    max_fd = current->log_read_fd;

                }

            }

            current = current->next;

        }

        pthread_mutex_unlock(&ctx.metadata_lock);

 

        if (select(max_fd + 1, &read_fds, NULL, NULL, NULL) < 0) {

            if (errno == EINTR) continue;

            perror("select");

            break; /* Exit loop on select error */

        }

 

        /* Check for new client connection */

        if (FD_ISSET(ctx.server_fd, &read_fds)) {

            int client_fd = accept(ctx.server_fd, NULL, NULL);

            if (client_fd >= 0) {

                control_request_t req;

                control_response_t resp;

                memset(&resp, 0, sizeof(resp));

                resp.status = 1; // Default to error

 

                ssize_t bytes_read = read(client_fd, &req, sizeof(req));

                if (bytes_read != sizeof(req)) {

                    snprintf(resp.message, sizeof(resp.message), "Malformed request.");

                    write(client_fd, &resp, sizeof(resp));

                    close(client_fd);

                    continue; // Go to next loop iteration

                }

 

                if (req.kind == CMD_START) {

                    if (handle_start_request(&ctx, &req, &resp, -1) == 0) {

                        resp.status = 0;

                    }

                } else if (req.kind == CMD_RUN) {

                    if (handle_start_request(&ctx, &req, &resp, client_fd) == 0) {

                        // On success, do NOT write or close. The connection is now held

                        // for the SIGCHLD handler to use later. So we just continue.

                        continue;

                    }

                    // On failure, the rest of the code will write the error and close.

                } else if (req.kind == CMD_PS) {

                    // --- THIS IS THE RESTORED PS LOGIC ---

                    char *buffer = malloc(4096);

                    if (buffer) {

                        char *ptr = buffer;

                        size_t remaining = 4096;

                        int written = snprintf(ptr, remaining, "%-12s %-8s %-12s %-s\n", "ID", "PID", "STATE", "STARTED");

                        ptr += written; remaining -= written;

 

                        pthread_mutex_lock(&ctx.metadata_lock);

                        for (container_record_t *c = ctx.containers; c != NULL && remaining > 1; c = c->next) {

                            written = snprintf(ptr, remaining, "%-12s %-8d %-12s %ld\n", c->id, c->host_pid, state_to_string(c->state), c->started_at);

                            ptr += written; remaining -= written;

                        }

                        pthread_mutex_unlock(&ctx.metadata_lock);

                        strncpy(resp.message, buffer, sizeof(resp.message) - 1);

                        free(buffer);

                    }

                    resp.status = 0;

                    // --- END OF RESTORED LOGIC ---

                } else if (req.kind == CMD_STOP) {

                    if (handle_stop_request(&ctx, &req, &resp) == 0) {

                        resp.status = 0;

                    }

                } else if (req.kind == CMD_LOGS) {

                    // This handler will manage the client socket itself.

                    handle_logs_request(&ctx, &req, client_fd);

                    continue; // Skip the default write/close logic.

 

                } else {

                    snprintf(resp.message, sizeof(resp.message), "Command %d not yet implemented.", req.kind);

                }

 

                // Default behavior: write response and close socket

                if (write(client_fd, &resp, sizeof(resp)) != sizeof(resp)) {

                    perror("write response to client");

                }

                //close(client_fd);

 

            }

        }

 

        /* Check for container log data */

        pthread_mutex_lock(&ctx.metadata_lock);

        for (container_record_t *c = ctx.containers; c != NULL; c = c->next) {

            if (c->log_read_fd >= 0 && FD_ISSET(c->log_read_fd, &read_fds)) {

                log_item_t item;

                memset(&item, 0, sizeof(item));

                strncpy(item.container_id, c->id, sizeof(item.container_id) - 1);

 

                item.length = read(c->log_read_fd, item.data, LOG_CHUNK_SIZE);

                if (item.length > 0) {

                    bounded_buffer_push(&ctx.log_buffer, &item);

                } else {

                    /* read() returns 0 on EOF, <0 on error. Pipe is dead. */

                    printf("Container '%s' (PID %d) output pipe closed.\n", c->id, c->host_pid);

                    close(c->log_read_fd);

                    c->log_read_fd = -1; /* Mark as closed */

                }

            }

        }

        pthread_mutex_unlock(&ctx.metadata_lock);

    }

 

    printf("\nSupervisor shutting down.\n");

    rc = 0; /* Success exit code */

 

    /* Wait for the logger thread to finish processing remaining logs */

    bounded_buffer_begin_shutdown(&ctx.log_buffer);

    pthread_join(ctx.logger_thread, NULL);

 

cleanup:

    bounded_buffer_begin_shutdown(&ctx.log_buffer);

    bounded_buffer_destroy(&ctx.log_buffer);

    pthread_mutex_destroy(&ctx.metadata_lock);

 

    if (ctx.server_fd >= 0)

        close(ctx.server_fd);

    if (ctx.monitor_fd >= 0)

        close(ctx.monitor_fd);

 

    unlink(CONTROL_PATH);

    g_ctx = NULL;

    return rc;

}

 

/*

* TODO:

* Implement the client-side control request path.

*

* The CLI commands should use a second IPC mechanism distinct from the

* logging pipe. A UNIX domain socket is the most direct option, but a

* FIFO or shared memory design is also acceptable if justified.

*/

static int send_control_request(const control_request_t *req)

{

    int client_fd;

    struct sockaddr_un addr;

    control_response_t resp;

    ssize_t bytes_written, bytes_read;

 

    client_fd = socket(AF_UNIX, SOCK_STREAM, 0);

    if (client_fd < 0) {

        perror("socket");

        return 1;

    }

 

    memset(&addr, 0, sizeof(addr));

    addr.sun_family = AF_UNIX;

    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

 

    if (connect(client_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {

        perror("connect to supervisor");

        close(client_fd);

        return 1;

    }

 

    bytes_written = write(client_fd, req, sizeof(*req));

    if (bytes_written != sizeof(*req)) {

        perror("write to supervisor");

        close(client_fd);

        return 1;

    }

 

    bytes_read = read(client_fd, &resp, sizeof(resp));

    if (bytes_read < 0) {

        perror("read from supervisor");

        close(client_fd);

        return 1;

    }

    if (bytes_read == 0) {

        fprintf(stderr, "Supervisor closed connection unexpectedly.\n");

        close(client_fd);

        return 1;

    }

 

    printf("%s\n", resp.message);

 

    close(client_fd);

    return resp.status;

}

 

static int cmd_start(int argc, char *argv[])

{

    control_request_t req;

 

    if (argc < 5) {

        fprintf(stderr,

                "Usage: %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",

                argv[0]);

        return 1;

    }

 

    memset(&req, 0, sizeof(req));

    req.kind = CMD_START;

    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);

    strncpy(req.command, argv[4], sizeof(req.command) - 1);

    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;

    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

 

    if (parse_optional_flags(&req, argc, argv, 5) != 0)

        return 1;

 

    return send_control_request(&req);

}

// Add these near the top, with other client code

static char g_run_container_id[CONTAINER_ID_LEN] = {0};

 

static void run_sigint_handler(int signum)

{

    if (signum == SIGINT && strlen(g_run_container_id) > 0) {

        fprintf(stderr, "\nCaught SIGINT, sending stop request for '%s'.\n", g_run_container_id);

 

        control_request_t stop_req;

        memset(&stop_req, 0, sizeof(stop_req));

        stop_req.kind = CMD_STOP;

        strncpy(stop_req.container_id, g_run_container_id, sizeof(stop_req.container_id) - 1);

 

        send_control_request(&stop_req);

    }

}

 

static int cmd_run(int argc, char *argv[])

{

    control_request_t req;

    int client_fd;

    struct sockaddr_un addr;

    control_response_t resp;

    ssize_t bytes_read;

 

    // --- 1. Argument Parsing (same as cmd_start) ---

    if (argc < 5) {

        fprintf(stderr,

                "Usage: %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",

                argv[0]);

        return 1;

    }

    memset(&req, 0, sizeof(req));

    req.kind = CMD_RUN;

    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);

    strncpy(req.command, argv[4], sizeof(req.command) - 1);

    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;

    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)

        return 1;

 

    // --- 2. Set up for signal handling ---

    strncpy(g_run_container_id, req.container_id, sizeof(g_run_container_id) - 1);

    struct sigaction sa;

    sa.sa_handler = run_sigint_handler;

    sigemptyset(&sa.sa_mask);

    sa.sa_flags = 0;

    sigaction(SIGINT, &sa, NULL);

 

    // --- 3. Connect and send request ---

    if ((client_fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {

        perror("socket"); return 1;

    }

    memset(&addr, 0, sizeof(addr));

    addr.sun_family = AF_UNIX;

    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(client_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {

        perror("connect to supervisor"); close(client_fd); return 1;

    }

    if (write(client_fd, &req, sizeof(req)) != sizeof(req)) {

        perror("write to supervisor"); close(client_fd); return 1;

    }

 

    // --- 4. Block waiting for the final response from supervisor ---

    bytes_read = read(client_fd, &resp, sizeof(resp));

    close(client_fd);

    g_run_container_id[0] = '\0'; // Deactivate signal handler logic

 

    if (bytes_read < 0) {

        if (errno == EINTR) {

            fprintf(stderr, "Run interrupted.\n");

            return 130; // Standard exit code for Ctrl+C

        }

        perror("read from supervisor");

        return 1;

    }

    if (bytes_read == 0) {

        fprintf(stderr, "Supervisor closed connection; container may have failed to start.\n");

        return 1;

    }

 

    // --- 5. Got a valid response, print and return its status ---

    printf("%s\n", resp.message);

    return resp.status;

}

 

static int cmd_ps(void)

{

    control_request_t req;

 

    memset(&req, 0, sizeof(req));

    req.kind = CMD_PS;

 

    /*

     * TODO:

     * The supervisor should respond with container metadata.

     * Keep the rendering format simple enough for demos and debugging.

     */

    printf("Expected states include: %s, %s, %s, %s, %s\n",

           state_to_string(CONTAINER_STARTING),

           state_to_string(CONTAINER_RUNNING),

           state_to_string(CONTAINER_STOPPED),

           state_to_string(CONTAINER_KILLED),

           state_to_string(CONTAINER_EXITED));

    return send_control_request(&req);

}

 

static int cmd_logs(int argc, char *argv[])

{

    control_request_t req;

    int client_fd;

    struct sockaddr_un addr;

 

    if (argc < 3) {

        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);

        return 1;

    }

 

    memset(&req, 0, sizeof(req));

    req.kind = CMD_LOGS;

    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

 

    // 1. Connect to the supervisor.

    if ((client_fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {

        perror("socket"); return 1;

    }

    memset(&addr, 0, sizeof(addr));

    addr.sun_family = AF_UNIX;

    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(client_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {

        perror("connect to supervisor"); close(client_fd); return 1;

    }

 

    // 2. Send the logs request.

    if (write(client_fd, &req, sizeof(req)) != sizeof(req)) {

        perror("write to supervisor"); close(client_fd); return 1;

    }

 

    // 3. Read the streaming response until the connection is closed.

    char buffer[4096];

    ssize_t bytes_read;

    while ((bytes_read = read(client_fd, buffer, sizeof(buffer))) > 0) {

        // Write the received chunk directly to standard output.

        fwrite(buffer, 1, bytes_read, stdout);

    }

 

    if (bytes_read < 0) {

        perror("read from supervisor");

    }

 

    close(client_fd);

    return 0;

}

 

static int cmd_stop(int argc, char *argv[])

{

    control_request_t req;

 

    if (argc < 3) {

        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);

        return 1;

    }

 

    memset(&req, 0, sizeof(req));

    req.kind = CMD_STOP;

    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

 

    return send_control_request(&req);

}

 

int main(int argc, char *argv[])

{

    if (argc < 2) {

        usage(argv[0]);

        return 1;

    }

 

    if (strcmp(argv[1], "supervisor") == 0) {

        if (argc < 3) {

            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);

            return 1;

        }

        return run_supervisor(argv[2]);

    }

 

    if (strcmp(argv[1], "start") == 0)

        return cmd_start(argc, argv);

 

    if (strcmp(argv[1], "run") == 0)

        return cmd_run(argc, argv);

 

    if (strcmp(argv[1], "ps") == 0)

        return cmd_ps();

 

    if (strcmp(argv[1], "logs") == 0)

        return cmd_logs(argc, argv);

 

    if (strcmp(argv[1], "stop") == 0)

        return cmd_stop(argc, argv);

 

    usage(argv[0]);

    return 1;

}

 

 

