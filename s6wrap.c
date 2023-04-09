/*
MIT License

Copyright (c) 2023 wiedehopf

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <sys/wait.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>




#define NOTUSED(V) ((void) V)

#define MAXEVENTS 32

#define MAX_LINESIZE (16 * 1024)

static char* programName = "s6wrap";

static int sigchldEventfd = -1;
static int childPid = -1;
static int epfd = -1;

static char errBuf[MAX_LINESIZE];
static int errBytes = 0;
static char outBuf[MAX_LINESIZE];
static int outBytes = 0;

char *prependString = NULL;
int enableTimestamps = 0;


// where this program should output to
FILE* outStream = NULL;

// the parent will never read from stdin, thus the child will get all input from stdin as both share the same stdin
static int pipe_out[2]; // pipe from child stdout
static int pipe_err[2]; // pipe from child stderr

// pipes: read from fd [0], write to fd [1]

#define litLen(literal) (sizeof(literal) - 1)
// return true for byte match between string and string literal. string IS allowed to be longer than literal
#define byteMatchStart(s1, literal) (memcmp(s1, literal, litLen(literal)) == 0)
// return true for byte match between string and string literal. string IS NOT allowed to be longer than literal
#define byteMatchStrict(s1, literal) (memcmp(s1, literal, sizeof(literal)) == 0)


static void signalEventfd(int fd) {
    uint64_t one = 1;
    ssize_t res = write(fd, &one, sizeof(one));
    NOTUSED(res);
}
static void resetEventfd(int fd) {
    uint64_t one = 1;
    ssize_t res = read(fd, &one, sizeof(one));
    NOTUSED(res);
}
// handler for SIGTERM / SIGINT / SIGQUIT / SIGHUP
static void sigvarHandler(int sig) {
    fprintf(stderr, "forwarding signal to child.\n");
    kill(childPid, sig);
}
static void sigchldHandler(int sig) {
    NOTUSED(sig);
    signalEventfd(sigchldEventfd);
}
static void usage(int argc, char* argv[], FILE* fStream) {
    fprintf(fStream, "%s incorrect usage, invoked by: ", programName);
    for (int k = 0; k < argc; k++) {
        fprintf(fStream, "%s", argv[k]);
        if (k < argc - 1) {
            fprintf(fStream, " ");
        }
    }
    fprintf(fStream, "\n");

    fprintf(fStream, "%s usage hint: %s [--output=<stdout|stderr> (default:stdout)] [--ignore=<stdout|stderr>] [--prepend=<identifier>] [--timestamps] --args <COMMAND --ARG1 --ARG2 VALUE2 [...] (don't quote the entire thing)>\n", programName, programName);
    fprintf(fStream, "by default stderr is merged with stdout line by line and the result written to --output\n");
    exit(EXIT_FAILURE);
}
static int isExited(int pid, int *exitStatus) {
    int wstatus;
    waitpid(pid, &wstatus, WNOHANG);
    if (WIFEXITED(wstatus)) {
        *exitStatus = WEXITSTATUS(wstatus);
        return 1;
    }
    return 0;
}
static void setNonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}
static void setup() {

    {
        // make epfd
        epfd = epoll_create(32); // argument positive, ignored
        if (epfd == -1) {
            perror("FATAL: epoll_create() failed:");
            exit(EXIT_FAILURE);
        }
    }

    {
        // make eventfd to signal main process from sigchld handler and register it with epfd
        sigchldEventfd = eventfd(0, EFD_NONBLOCK);

        struct epoll_event epollEvent = { .events = EPOLLIN, .data = { .ptr = &sigchldEventfd }};
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, sigchldEventfd, &epollEvent)) {
            perror("epoll_ctl fail:");
            exit(EXIT_FAILURE);
        }
    }

    {
        // register pipe output sides with epfd
        struct epoll_event epollEvent;
        epollEvent = (struct epoll_event) { .events =  EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLHUP, .data = { .fd = pipe_out[0] }};
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, pipe_out[0], &epollEvent)) {
            perror("epoll_ctl fail:");
        }
        epollEvent = (struct epoll_event) { .events =  EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLHUP, .data = { .fd = pipe_err[0] }};
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, pipe_err[0], &epollEvent)) {
            perror("epoll_ctl fail:");
        }
    }


    // set up signal handlers
    signal(SIGCHLD, sigchldHandler);
    signal(SIGTERM, sigvarHandler);
    signal(SIGINT, sigvarHandler);
    signal(SIGQUIT, sigvarHandler);
    signal(SIGHUP, sigvarHandler);

    // close unused write sides of the pipes
    close(pipe_out[1]);
    close(pipe_err[1]);

    // set pipe outputs nonblock
    setNonblock(pipe_out[0]);
    setNonblock(pipe_err[0]);
}
static void cleanup() {
    if (prependString) {
        free(prependString);
    }

    epoll_ctl(epfd, EPOLL_CTL_DEL, pipe_out[0], NULL);
    epoll_ctl(epfd, EPOLL_CTL_DEL, pipe_err[0], NULL);
    epoll_ctl(epfd, EPOLL_CTL_DEL, sigchldEventfd, NULL);

    close(epfd);

    // close read sides of the pipes
    close(pipe_out[0]);
    close(pipe_err[0]);

}
static void outputBuffer(char *buf, int bytes) {
    if (bytes <= 0) {
        return;
    }
    int res = fwrite(buf, 1, bytes, outStream);
    NOTUSED(res);
}
static void processChildOutput(int fd, char *buf, int *bufBytes) {
    while (1) {
        int toRead = MAX_LINESIZE - *bufBytes;
        char *target = buf + *bufBytes;
        int res = read(fd, target, toRead);
        if (res == -1) {
            if (errno == EINTR) {
                continue;
            } else {
                break;
            }
        }
        if (res == 0) {
            // EOF
            outputBuffer(buf, *bufBytes);
            bufBytes = 0;
        }
        *bufBytes += res;
        if (*bufBytes >= 1 && buf[*bufBytes - 1] == '\n') {
            outputBuffer(buf, *bufBytes);
            *bufBytes = 0;
        }
        char *lastNewline = memrchr(buf, '\n', *bufBytes);
        if (lastNewline) {
            int bytes = lastNewline + 1 - buf;
            outputBuffer(buf, bytes);
            *bufBytes -= bytes;
            if (*bufBytes > 0) {
                memmove(buf, lastNewline + 1, *bufBytes);
            }
        }
    }
}
static int mainLoop() {

    outStream = stdout; // by default output to stdout

    struct epoll_event events[MAXEVENTS];

    int exitStatus = EXIT_FAILURE;

    int childExited = 0;
    // wait on data produced by the child
    while (!childExited) {
        int count = epoll_wait(epfd, events, MAXEVENTS, 10 * 1000);
        if (count == 0) {
            if (isExited(childPid, &exitStatus)) {
                fprintf(stderr, "checking status: chld exited\n");
                childExited = 1;
            }
        }
        for (int k = 0; k < count; k++) {
            struct epoll_event event = events[k];
            if (event.data.ptr == &sigchldEventfd) {
                // signal
                resetEventfd(sigchldEventfd);
                if (isExited(childPid, &exitStatus)) {
                    fprintf(stderr, "sigchld / chld exited\n");
                    childExited = 1;
                } else {
                    fprintf(stderr, "sigchld no exit\n");
                }
                continue;
            } else {
                // actual fd
                int fd = event.data.fd;

                int *bufBytes = NULL;
                char *buf = NULL;

                if (fd == pipe_out[0]) {
                    buf = outBuf;
                    bufBytes = &outBytes;
                } else if (fd == pipe_err[0]) {
                    buf = errBuf;
                    bufBytes = &errBytes;
                } else {
                    fprintf(stderr, "%s: ERROR: epoll_wait returned an unexpected file descriptor: %d\n", programName, fd);
                    exit(EXIT_FAILURE);
                }
                processChildOutput(fd, buf, bufBytes);
            }
        }
    }

    return exitStatus;
}
int main(int argc, char* argv[]) {
    // set linebuffering for stdout / stderr
    setvbuf(stdout, NULL, _IOLBF, MAX_LINESIZE);
    setvbuf(stderr, NULL, _IOLBF, MAX_LINESIZE);

    char** eargs = NULL;
    int eargc = 0;

    for (int k = 0; k < argc; k++) {
        char *arg = argv[k];
        // only -- arguments are allowed
        if (arg[0] != '-' || arg[1] != '-') {
            usage(argc, argv, outStream);
        }
        arg += 2; // skip --
        if (byteMatchStrict(arg, "args")) {
            // parameter parsing done.
            k++;
            eargs = &argv[k];
            eargc = argc - k;
            if (eargc <= 0) {
                usage(argc, argv, outStream);
            }
            break;
        }
        if (byteMatchStart(arg, "output=")) {
            char *value = arg + litLen("output=");
            if (byteMatchStrict(value, "stderr")) {
                outStream = stderr;
            } else if (byteMatchStrict(value, "stdout")) {
                outStream = stdout;
            }
            continue;
        }
        if (byteMatchStart(arg, "prepend=")) {
        char *value = arg + litLen("prepend=");
            prependString = strdup(value);
            continue;
        }
        if (byteMatchStrict(arg, "timestamps")) {
            enableTimestamps = 1;
            continue;
        }
    }

    sigset_t mask;
    sigfillset(&mask);
    sigprocmask(SIG_SETMASK, &mask, NULL);

    int pid = fork();
    if (pid == -1) {
        fprintf(outStream, "%s: error on fork: %s\n", programName, strerror(errno));
        exit(EXIT_FAILURE);
    }

    int flags = 0;
    if (pipe2(pipe_out, flags) || pipe2(pipe_err, flags)) {
        fprintf(outStream, "%s could not create pipes: %s\n", programName, strerror(errno));
        exit(EXIT_FAILURE);
    }

    if (pid == 0) {
        // CHILD:
        // make stdout / stderr write TO pipe
        dup2(pipe_out[1], STDOUT_FILENO);
        dup2(pipe_err[1], STDERR_FILENO);

        // close unneeded fds
        close(pipe_out[0]);
        close(pipe_err[0]);
        close(pipe_out[1]);
        close(pipe_err[1]);

        // unblock signals
        sigemptyset(&mask);
        sigprocmask(SIG_SETMASK, &mask, NULL);

        execvp(eargs[0], eargs);
        exit(EXIT_FAILURE);   // exec never returns
    }

    // PARENT:
    childPid = pid;

    setup();

    // unblock signals after doing the setup
    sigemptyset(&mask);
    sigprocmask(SIG_SETMASK, &mask, NULL);

    // call the main Loop doing epoll_wait
    int exitStatus =  mainLoop();

    cleanup();

    fprintf(outStream, "exiting with %d\n", exitStatus);
    exit(exitStatus);
}
