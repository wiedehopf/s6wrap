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
#include <math.h>
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
#include <sys/time.h>
#include <stdarg.h>


extern const char *const sys_siglist[];



#define NOTUSED(V) ((void) V)

#define MAXEVENTS 32

#define MAX_LINESIZE (16 * 1024)

static char *programName = "s6wrap";

static int sigchldEventfd = -1;
static int childPid = -1;
static int epfd = -1;

static char errBuf[MAX_LINESIZE + 1];
static int errBytes = 0;
static char outBuf[MAX_LINESIZE + 1];
static int outBytes = 0;

static char *prependString = NULL;
static int enableTimestamps = 0;
static int enableExecPrint = 1;

static int debug = 0;

static int ignoreStderr = 0;
static int ignoreStdout = 0;

static int quiet = 0;

static char** eargv = NULL;
static int eargc = 0;



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


int64_t microtime(void) {
    struct timeval tv;
    int64_t mst;

    gettimeofday(&tv, NULL);
    mst = ((int64_t) tv.tv_sec) * 1000LL * 1000LL;
    mst += tv.tv_usec;
    return mst;
}

int64_t mstime(void) {
    struct timeval tv;
    int64_t mst;

    gettimeofday(&tv, NULL);
    mst = ((int64_t) tv.tv_sec)*1000;
    mst += tv.tv_usec / 1000;
    return mst;
}
static void printStart(FILE *stream) {
    if (enableTimestamps) {
        char timebuf[128];
        char timebuf2[128];
        time_t now;
        struct tm local;

        now = time(NULL);
        localtime_r(&now, &local);
        strftime(timebuf, 128, "%Y-%m-%d %T", &local);
        timebuf[127] = 0;
        if (0) {
            strftime(timebuf2, 128, "%Z", &local);
            timebuf2[127] = 0;
            fprintf(stream, "[%s.%03d %s]", timebuf, (int) (mstime() % 1000), timebuf2);
        } else {
            // don't print time zone
            fprintf(stream, "[%s.%03d]", timebuf, (int) (mstime() % 1000));
        }
    }
    if (prependString) {
        fprintf(stream, "%s", prependString);
    }
    if (enableTimestamps || prependString) {
        fprintf(stream, " ");
    }
}

void errorPrint(const char *format, ...) __attribute__ ((format(printf, 1, 2)));
void errorPrint(const char *format, ...) {
    char msg[1024];
    va_list ap;

    printStart(outStream);

    va_start(ap, format);
    vsnprintf(msg, 1024, format, ap);
    va_end(ap);
    msg[1023] = 0;

    fprintf(outStream, "[%s] %s", programName, msg);
}

static void printExecLine() {
    if (!eargv) {
        return;
    }
    for (int k = 0;; k++) {
        if (!eargv[k]) {
            break;
        }
        if (k != 0) {
            fprintf(outStream, " ");
        }
        fprintf(outStream, "%s", eargv[k]);
    }
    fprintf(outStream, "\n");
}

static void _sigaction_range(struct sigaction *sa, int first, int last) {
    int sig;
    for (sig = first; sig <= last; ++sig) {
        if (sigaction(sig, sa, NULL)) {
            /* SIGKILL/SIGSTOP trigger EINVAL.  Ignore */
            if (errno != EINVAL) {
                fprintf(stderr, "sigaction(%s[%i]) failed: %s\n", strsignal(sig), sig, strerror(errno));
            }
        }
    }
}

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
static void sigforwardHandler(int sig) {
    if (debug) { fprintf(stderr, "forwarding signal %d to child. (%s)\n", sig, strsignal(sig)); }
    kill(childPid, sig);
}
static void sigchldHandler(int sig) {
    NOTUSED(sig);
    signalEventfd(sigchldEventfd);
}
static void usage(int argc, char** argv) {
    FILE *fStream = outStream;
    fprintf(fStream, "%s incorrect usage, invoked by: ", programName);
    for (int k = 0; k < argc; k++) {
        fprintf(fStream, "%s", argv[k]);
        if (k < argc - 1) {
            fprintf(fStream, " ");
        }
    }
    fprintf(fStream, "\n");

    fprintf(fStream, "%s usage hint: %s [--output=<stdout|stderr> (default:stdout)] [--quiet] [--ignore=<stdout|stderr>] [--prepend=<identifier>] [--timestamps] --args <COMMAND --ARG1 --ARG2 VALUE2 [...] (don't quote the entire thing)>\n", programName, programName);
    fprintf(fStream, "by default stderr is merged with stdout line by line and the result written to --output\n");
    exit(EXIT_FAILURE);
}
static int isExited(int pid, int *exitStatus) {
    int wstatus;
    int res = waitpid(pid, &wstatus, WNOHANG);
    if (res != pid) {
        if (res == 0) {
            // WNOHANG: child state unchanged
            return 0;
        }
        if (res == -1) {
            fprintf(outStream, "%s: waitpid error: %s", programName, strerror(errno));
        }
        if (debug) { fprintf(outStream, "%s: waitpid weird: res %d pid %d", programName, res, pid); }
        return 0;
    }
    int exited = WIFEXITED(wstatus);
    int signaled = WIFSIGNALED(wstatus);
    if (exited || signaled) {
        int termSig = WTERMSIG(wstatus);
        *exitStatus = WEXITSTATUS(wstatus);
        if (debug) { fprintf(outStream, "child %d exited: %d child signaled %d\n", pid, exited, signaled); }
        if (signaled) {
            char *strsig = strsignal(termSig);
            if (termSig == SIGILL || termSig == SIGSEGV || termSig == SIGFPE || termSig == SIGABRT || termSig == SIGBUS) {
                errorPrint("!!! CAUTION !!! Wrapped program terminated by signal: %d (%s)\n", termSig, strsig);
                errorPrint("Command line for terminated program was: ");
                printExecLine();
            } else if (!quiet) {
                errorPrint("Wrapped program terminated by signal: %d (%s)\n", termSig, strsig);
            }
        } else if (exited && !quiet) {
            errorPrint("Wrapped program exited with status: %d (%s)\n", *exitStatus, strerror(*exitStatus));
        }
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

    // close write sides of the pipes
    close(pipe_out[1]);
    close(pipe_err[1]);

    // set all signal handlers to the sigforwardHandler
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigfillset(&sa.sa_mask);
    sa.sa_handler = sigforwardHandler;
    _sigaction_range(&sa, 1, 31);

    // set sigchld to our handler
    signal(SIGCHLD, sigchldHandler);


    // set pipe outputs nonblock
    setNonblock(pipe_out[0]);
    setNonblock(pipe_err[0]);
}
static void cleanup() {
    if (prependString) {
        free(prependString);
    }

    //epoll_ctl(epfd, EPOLL_CTL_DEL, pipe_out[0], NULL);
    //epoll_ctl(epfd, EPOLL_CTL_DEL, pipe_err[0], NULL);
    //epoll_ctl(epfd, EPOLL_CTL_DEL, sigchldEventfd, NULL);

    close(epfd);

    // close read sides of the pipes
    close(pipe_out[0]);
    close(pipe_err[0]);



}

static int fdIgnored(int fd) {
    return ((fd == pipe_out[0] && ignoreStdout) || (fd == pipe_err[0] && ignoreStderr));
}
static int outputLine(FILE *stream, char *buf, int bytes) {
    if (bytes <= 0) {
        return 0;
    }
    printStart(stream);

    int res = fwrite(buf, 1, bytes, stream);
    return res;
}
static void processChildOutput(int fd, char *buf, int *bufBytes) {
    int totalBytesWritten = 0;
    while (1) {
        int toRead = MAX_LINESIZE - *bufBytes;
        char *target = buf + *bufBytes;
        int res = read(fd, target, toRead);
        if (debug) {
            if (fd == pipe_out[0]) {
                fprintf(outStream, "pipe_out[0]: read res: %d\n", res);
            } else if (fd == pipe_err[0]) {
                fprintf(outStream, "pipe_err[0]: read res: %d\n", res);
            }
        }
        if (res <= 0) {
            if (res < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
            } else if (res == 0) {
                // EOF

                if (debug) {
                    if (fd == pipe_out[0]) {
                        fprintf(outStream, "pipe_out[0]: read got EOF\n");
                    } else if (fd == pipe_err[0]) {
                        fprintf(outStream, "pipe_err[0]: read got EOF\n");
                    }
                }

                if (!fdIgnored(fd)) {
                    totalBytesWritten += outputLine(outStream, buf, *bufBytes);
                }
                *bufBytes = 0;
                // remove from epoll
                //fprintf(outStream, "*bufBytes: %d\n", *bufBytes);
                epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
            }
            break;
        }

        if (!fdIgnored(fd)) {
            *bufBytes += res;

            char *start = buf;
            char *newline;
            int bytesProcessed = 0;
            while ((newline = memchr(start, '\n', *bufBytes))) {
                int bytes = newline + 1 - start;
                totalBytesWritten += outputLine(outStream, start, bytes);
                if (debug) { fprintf(outStream, "processed %4d bytes\n", bytes); }
                bytesProcessed += bytes;
                *bufBytes -= bytes;
                start += bytes;
            }
            if (*bufBytes > 0 && bytesProcessed > 0) {
                memmove(buf, buf + bytesProcessed, *bufBytes);
            }
        }
    }
    if (totalBytesWritten > 0) {
        fflush(outStream);
        //fprintf(outStream, "flushed!\n");
    }
}
static int mainLoop() {
    struct epoll_event events[MAXEVENTS];

    int exitStatus = EXIT_FAILURE;

    int childExited = 0;
    // wait on data produced by the child
    while (!childExited) {
        int count = epoll_wait(epfd, events, MAXEVENTS, 1 * 1000);
        if (count == 0) {
            if (isExited(childPid, &exitStatus)) {
                fprintf(outStream, "%s: BAD: checking status: chld exited without SIGCHLD being triggered?? this shouldn't be possible!\n", programName);
                childExited = 1;
            }
        }
        for (int k = 0; k < count; k++) {
            struct epoll_event event = events[k];
            if (event.data.ptr == &sigchldEventfd) {
                // signal
                resetEventfd(sigchldEventfd);
                if (isExited(childPid, &exitStatus)) {
                    //fprintf(outStream, "sigchld / chld exited\n");
                    childExited = 1;
                } else {
                    fprintf(outStream, "sigchld no exit\n");
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
                    fprintf(outStream, "%s: ERROR: epoll_wait returned an unexpected file descriptor: %d\n", programName, fd);
                    exit(EXIT_FAILURE);
                }
                processChildOutput(fd, buf, bufBytes);
            }
        }
    }

    return exitStatus;
}
//fprintf(outStream, "%s:%d\n", __FILE__, __LINE__);
int main(int argc, char* argv[]) {
    errBuf[MAX_LINESIZE] = '\0';
    outBuf[MAX_LINESIZE] = '\0';

    outStream = stdout; // by default output to stdout

    for (int k = 1; k < argc; k++) {
        char *arg = argv[k];
        // only -- arguments are allowed
        if (arg[0] != '-' || arg[1] != '-') {
            usage(argc, argv);
        }
        arg += 2; // skip --
        if (byteMatchStrict(arg, "args")) {
            // parameter parsing done.
            k++;
            eargv = &argv[k];
            eargc = argc - k;
            if (eargc <= 0) {
                usage(argc, argv);
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
        if (byteMatchStart(arg, "ignore=")) {
            char *value = arg + litLen("ignore=");
            if (byteMatchStrict(value, "stderr")) {
                ignoreStderr = 1;
            } else if (byteMatchStrict(value, "stdout")) {
                ignoreStdout = 1;
            }
            continue;
        }
        if (byteMatchStart(arg, "prepend=")) {
            char *value = arg + litLen("prepend=");
            if (prependString) { free(prependString); }
            prependString = malloc(2+1+strlen(value));
            sprintf(prependString, "[%s]", value);
            continue;
        }
        if (byteMatchStrict(arg, "timestamps")) {
            enableTimestamps = 1;
            continue;
        }
        if (byteMatchStrict(arg, "quiet")) {
            quiet = 1;
            continue;
        }
    }

    if (!eargv) {
        usage(argc, argv);
    }

    // block signals
    sigset_t mask;
    sigfillset(&mask);
    sigprocmask(SIG_SETMASK, &mask, NULL);

    int flags = O_DIRECT;
    flags = 0;
    if (pipe2(pipe_out, flags) || pipe2(pipe_err, flags)) {
        fprintf(outStream, "%s could not create pipes: %s\n", programName, strerror(errno));
        exit(EXIT_FAILURE);
    }

    if (enableExecPrint && !quiet) {
        errorPrint("executing: ");
        printExecLine();
    }

    //setvbuf(stdout, NULL, _IOLBF, MAX_LINESIZE);
    //setvbuf(stderr, NULL, _IOLBF, MAX_LINESIZE);

    int pid = fork();
    if (pid == -1) {
        fprintf(outStream, "%s: error on fork: %s\n", programName, strerror(errno));
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

        execvp(eargv[0], eargv);
        exit(EXIT_FAILURE);   // exec never returns
    }

    // PARENT:
    childPid = pid;
    if (debug) { fprintf(outStream, "childPid: %d\n", childPid); }

    setup();

    // unblock signals after doing the setup
    sigemptyset(&mask);
    sigprocmask(SIG_SETMASK, &mask, NULL);

    // call the main Loop doing epoll_wait
    int exitStatus = mainLoop();

    cleanup();

    if (debug) { errorPrint("exiting with status: %d\n", exitStatus); }
    exit(exitStatus);
}
