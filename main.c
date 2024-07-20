#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <stdarg.h>

#define BACKOFF_TIME 2 // seconds

typedef struct {
    pid_t pid;
    char **args;
    int retries;
} Process;

Process *processes = NULL;
int process_count = 0;

void log_message(const char *format, ...) {
    va_list args;
    time_t now = time(NULL);
    char timestamp[20];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", localtime(&now));
    printf("[%s] ", timestamp);
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    printf("\n");
}

void start_process(int index) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork error");
        exit(EXIT_FAILURE);
    }
    if (pid == 0) {
        // Child process
        execvp(processes[index].args[0], processes[index].args);
        perror("exec error");
        exit(EXIT_FAILURE);
    }
    processes[index].pid = pid;
    log_message("Started process %d (%s) with PID %d", index, processes[index].args[0], pid);
    processes[index].retries = 0; // Reset retry count on successful start
}

void restart_process(int index) {
    sleep(BACKOFF_TIME * (processes[index].retries + 1)); // Exponential backoff

    log_message("Restarting process %d (%s), retry %d", index, processes[index].args[0], processes[index].retries + 1);

    processes[index].retries++;
    start_process(index);
}

void handle_sigchld(int sig) {
    (void)sig;  // Silence unused parameter warning
    pid_t pid;
    int status;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for (int i = 0; i < process_count; i++) {
            if (processes[i].pid == pid) {
                if (WIFEXITED(status) || WIFSIGNALED(status)) {
                    log_message("Process %d (%s) with PID %d terminated unexpectedly", i, processes[i].args[0], pid);
                    restart_process(i);
                }
                break;
            }
        }
    }
}

void handle_sigint(int sig) {
    (void)sig;  // Silence unused parameter warning
    log_message("Received SIGINT, terminating all processes");
    for (int i = 0; i < process_count; i++) {
        if (processes[i].pid > 0) {
            kill(processes[i].pid, SIGTERM);
        }
    }
    for (int i = 0; i < process_count; i++) {
        if (processes[i].args) {
            free(processes[i].args);
        }
    }
    free(processes);
    exit(EXIT_SUCCESS);
}

char **parse_command(char *cmd) {
    size_t max_args = 10;
    size_t arg_count = 0;
    char **args = malloc(max_args * sizeof(char *));
    if (args == NULL) {
        perror("malloc error");
        exit(EXIT_FAILURE);
    }
    char *token = strtok(cmd, ";");
    while (token != NULL) {
        if (arg_count >= max_args) {
            max_args *= 2;
            args = realloc(args, max_args * sizeof(char *));
            if (args == NULL) {
                perror("realloc error");
                exit(EXIT_FAILURE);
            }
        }
        args[arg_count++] = token;
        token = strtok(NULL, ";");
    }
    args[arg_count] = NULL;
    return args;
}

void parse_processes(int argc, char *argv[]) {
    processes = malloc((argc - 1) * sizeof(Process));
    if (processes == NULL) {
        perror("malloc error");
        exit(EXIT_FAILURE);
    }
    for (int i = 1; i < argc; i++) {
        processes[process_count].args = parse_command(argv[i]);
        processes[process_count].pid = -1;
        processes[process_count].retries = 0;
        process_count++;
    }
}

void setup_sigaction(int signum, void (*handler)(int)) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    if (sigaction(signum, &sa, NULL) < 0) {
        perror("sigaction error");
        exit(EXIT_FAILURE);
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s \"cmd1;arg1;arg2\" \"cmd2;arg1;arg2\" ...\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    // Parse processes from command-line arguments
    parse_processes(argc, argv);

    // Setup SIGCHLD and SIGINT handlers
    setup_sigaction(SIGCHLD, handle_sigchld);
    signal(SIGINT, handle_sigint);

    // Start all processes
    for (int i = 0; i < process_count; i++) {
        start_process(i);
    }

    // Main loop
    while (1) {
        pause(); // Wait for signals
    }

    return 0;
}
