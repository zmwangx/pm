#include <condition_variable>
#include <fcntl.h>
#include <iostream>
#include <libgen.h>
#include <limits.h>
#include <mutex>
#include <signal.h>
#include <stdexcept>
#include <stdlib.h>
#include <string>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <time.h>
#include <unistd.h>

#include "config.h"

std::thread server_controller_thread;
pid_t server_pid = 0;
bool server_not_running = true;
bool shutting_down = false;
int exit_status = 0;
std::mutex mtx;
std::condition_variable_any cv;

class PMException: public std::runtime_error {
public:
    PMException(std::string const &message): std::runtime_error(message) {}
};

/**
 * Prints help text for the program.
 */
void print_help();

/**
 * Logs a message to stderr.
 *
 * The logging format conforms to the format of logs emitted by the http.server
 * module of Python 3, i.e., a logged message looks like
 *
 *     [14/Jun/2016 06:24:50] Starting server...
 *
 * Note that a newline is printed after the message.
 *
 * @param msg The message to be logged.
 */
void log(const char *msg);

void print_error_and_initiate_shutdown(const char *msg);

/**
 * Calls man(1) on the supplied file and returns the output.
 *
 * man(1) is run with PAGER set to cat(1) and COLUMNS set to 120. Only stdout
 * is captured; warnings emitted by man(1) go straight to stderr.
 *
 * @param manfile Path to the man page file to be rendered with man(1).
 * @returns Output captured on the stdout of man(1).
 * @throws PMException
 *         If the man(1) call fails for whatever reason.
 * @see to_html
 */
std::string run_man(const std::string &manfile);

/**
 * Converts man(1) output to an HTML file with auto-update support.
 *
 * @param man_string man(1) output as returned by run_man.
 * @returns An HTML string ready to be written to file.
 * @see run_man
 * @see write_to_file
 */
std::string to_html(const std::string &man_string);

/**
 * Creates a tempfile with a .html extension.
 *
 * The tempfile is created with mkstemp(3).
 *
 * @param tempfile The string buffer to which the path of the newly created
 * tempfile is written.
 * @returns The return value of mkstemp(3), which is the file descriptor for
 * the tempfile or -1 on error.
 * @see write_to_file
 */
int get_tempfile(std::string &tempfile);

/**
 * Writes string to file.
 *
 * The file is overwritten if it already exists.
 *
 * @param str The string to write.
 * @param filepath The path to the file to write to.
 * @throws PMException
 *         If any of the opening and writing operations fails.
 * @see get_tempfile
 */
void write_to_file(const std::string &str, const std::string &filepath);

/**
 * Starts the HTTP server (../libexec/pm/server.py).
 *
 * This routine is also responsible for restarting the server on crash, or
 * killing the server if the shut down signal is received but the server
 * remains unresponsive for a period of time.
 *
 * Note that if server.py cannot be found, the child process used to spawn the
 * server will exit with status 127. This case should be appropriately handled
 * in the SIGCHLD handler.
 *
 * This routine is blocking, so it should run in a dedicated thread.
 *
 * @param progpath Path to the current program, i.e., argv[0] of main.
 * @param tempfile Path to the tempfile which contains the HTML version of the
 * man page.
 * @see get_tempfile
 * @see write_to_file
 */
void start_server(const std::string &progpath, const std::string &tempfile);

/**
 * Retrieves the mtime of a file as returned by stat(2).
 *
 * @param filepath Path to the file of interest.
 * @param mtime Buffer to which the mtime is written on success.
 * @returns 0 on success, -1 on error (failed stat(2) call).
 * @see watch_for_changes
 */
int get_mtime(const std::string &filepath, timespec &mtime);

/**
 * Watches for changes on the source file.
 *
 * This routine polls for changes on the man page source file, and upon change,
 * it regenerates the HTML version and issues a SIGUSR1 to the server which
 * triggers a server-sent event to update client-side content.
 *
 * @param manfile Path to the man page source file.
 * @param tempfile Path to the generated HTML file.
 * @param initial_mtime Initial mtime of the source file when the HTML file was
 * first generated.
 */
void watch_for_changes(const std::string &manfile, const std::string &tempfile,
                       const timespec &initial_mtime);

/**
 * The SIGCHLD listener (handler).
 *
 * See implementation for details.
 *
 * @param sig The signal number.
 */
void sigchld_listener(int sig);

/**
 * The SIGINT and SIGTERM joint listener (handler).
 *
 * See implementation for details.
 *
 * @param sig The signal number.
 */
void sigint_term_listener(int sig);

/**
 * Comparison operator for timespec.
 *
 * The timespec struct is defined in time.h.
 *
 * @param lhs Left-hand side.
 * @param rhs Right-hand side.
 * @returns True if lhs is strictly earlier than rhs; false otherwise.
 */
bool operator <(const timespec& lhs, const timespec& rhs);

int main(int argc, const char *argv[]) {
    try {
        if (argc == 1 || strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
            print_help();
            exit(1);
        } else if (argc > 2) {
            std::cerr << "Warning: Extraneous arguments ignored." << std::endl;
        }

        std::string manfile = argv[1];
        timespec initial_mtime;
        if (get_mtime(argv[1], initial_mtime) == -1) {
            // std::ostringstream msg;
            // msg << "Failed to stat " << argv[1] << ".";
            throw PMException("Failed to stat " + manfile + ".");
        }

        std::string tempfile;
        int fd = get_tempfile(tempfile);
        if (fd == -1) {
            throw PMException("Failed to create temp file.");
        }
        close(fd);
        write_to_file(to_html(run_man(manfile)), tempfile);

        signal(SIGCHLD, sigchld_listener);
        signal(SIGINT, sigint_term_listener);
        signal(SIGTERM, sigint_term_listener);

        server_controller_thread = std::thread(start_server, argv[0], tempfile);
        watch_for_changes(manfile, tempfile, initial_mtime);
        server_controller_thread.join();
    } catch (PMException &e) {
        print_error_and_initiate_shutdown(e.what());
    }
    exit(exit_status);
}

void print_help() {
    std::string help_text = R"(Preview man page as you edit.

Usage:
    pm [options] manfile

Options:
    -h, --help
        Print help text and exit.
)";
    std::cerr << help_text << std::endl;
}

void log(const char *msg) {
    // Keep logging format inline with server.py
    // Not thread safe
    time_t t = time(0);
    tm *now = localtime(&t);
    char timestr[128];
    strftime(timestr, 127, "%d/%b/%Y %H:%M:%S", now);
    std::cerr << "[" << timestr << "] " << msg << std::endl;
}

void print_error_and_initiate_shutdown(const char *msg) {
    std::cerr << "Error: " << msg << std::endl;
    mtx.lock();
    shutting_down = true;
    exit_status = 1;
    cv.notify_all();
    mtx.unlock();
}

// TODO: Customizable COLUMNS
std::string run_man(const std::string &manfile) {
    char *path; // path won't be freed; it's okay
    if ((path = realpath(manfile.c_str(), NULL)) == NULL) {
        throw PMException("Cannot resolve " + manfile + ".");
    }

    setenv("PAGER", "cat", 1);
    setenv("COLUMNS", "120", 1);

    int fds[2];
    pipe(fds);
    pid_t pid = fork();

    if (pid == 0) {
        dup2(fds[1], STDOUT_FILENO);
        close(fds[0]);
        close(fds[1]);
        const char *argv[3] = {"man", path, NULL};
        if (execvp(argv[0], const_cast<char *const *>(argv)) == -1 &&
            errno == ENOENT) {
            throw PMException("man(1) not found.");
        } else {
            throw PMException("Unknown error occurred when calling man(1).");
        }
    }

    close(fds[1]);

    int status;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || !(WEXITSTATUS(status) == 0)) {
        throw PMException("Call to man(1) failed.");
    }

    char buf[4097];
    std::string str;
    ssize_t size;
    while ((size = read(fds[0], buf, 4096)) != 0) {
        if (size == -1) {
            throw PMException("Failed to read from pipe.");
        }
        buf[size] = '\0';
        str += buf;
    }
    return str;
}

std::string to_html(const std::string &man_string) {
    std::string ms = man_string;
    const size_t n = ms.length();
    ms += "\0\0"; // Append two NULs to the string so that we won't need to
                  // worry about index out of bound

    // The HTML string to be constructed from ms
    std::string hs = R"(<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<title>man page</title>
<style type="text/css">
    body {
        text-align: center;
    }

    #manpage {
        text-align: left;
        display: inline-block;
    }
</style>
</head>
<body>
<pre id="manpage">
)";

    // Initial state
    bool inbold = false;
    bool initalic = false;

    size_t i = 0;
    while (i < n) {
        char ch = ms[i];

        // Shrink consecutive blank lines into one
        if (ch == '\n' && ms[i+1] == '\n') {
            // First close markup tags
            if (inbold) {
                hs += "</b>";
                inbold = false;
            }
            if (initalic) {
                hs += "</u>";
                initalic = false;
            }
            hs += "\n\n";
            i += 2;
            while (ms[i] == '\n') {
                ++i;
            }
            continue;
        }

        bool bold, italic;
        if (ms[i+1] == '\b') {
            // A three-character sequence CH BS CH is the markup for bold CH
            bold = (ch == ms[i+2]);
            // A three-character sequence _ BS CH is the markup for italic CH
            // (underlined in pagers like less(1))
            italic = (ch == '_');
            if (bold && italic) {
                // Encountered _ BS _ which is ambiguous.
                //
                // 1. We assume that bold and italic cannot coexist (which is
                //     at least true for groff_man);
                //
                // 2. When the previous character is in italic, we assume this
                //    is an italic underscore; otherwise we assume it is a bold
                //    underscore. This matches the bahavior of less(1) (version
                //    458) on OS X when it is set as the pager for man(1).
                if (initalic) {
                    bold = false;
                } else {
                    italic = false;
                }
            }
        }  else {
            bold = italic = false;
        }

        if (inbold && !bold) {
            hs += "</b>";
            inbold = false;
        }
        if (initalic && !italic) {
            hs += "</u>";
            initalic = false;
        }
        if (bold) {
            if (!inbold) {
                hs += "<b>";
                inbold = true;
            }
            i += 2;
        }
        if (italic) {
            if (!initalic) {
                hs += "<u>";
                initalic = true;
            }
            ch = ms[i+2];
            i += 2;
        }

        if (ch == '<') {
            hs += "&lt;";
        } else if (ch == '>') {
            hs += "&gt;";
        } else {
            hs += ch;
        }

        ++i;
    }

    hs += R"_(
</pre>
<script>
(function () {
  var source = new EventSource('/events')
  source.addEventListener('update', function (e) {
    document.getElementById('manpage').innerHTML = JSON.parse(e.data).content
  })
  source.addEventListener('bye', function (e) {
    source.close()
  })
})()
</script>
</body>
</html>
)_";

    return hs;
}

int get_tempfile(std::string &tempfile) {
    char name[] = "/tmp/pm-XXXXXX.html";
    int fd = mkstemps(name, 5);
    tempfile = name;
    return fd;
}

void write_to_file(const std::string &str, const std::string &filepath) {
    int fd = open(filepath.c_str(), O_WRONLY);
    if (fd == -1) {
        throw PMException("Failed to open " + str + " for writing.");
    }
    const char *ptr = str.c_str();
    ssize_t bytes_written = 0;
    ssize_t length = str.length();
    while (bytes_written < length) {
        ssize_t size = write(fd, ptr, length - bytes_written);
        if (size == -1) {
            throw PMException("Failed to write to fd " + std::to_string(fd) + ".");
        }
        bytes_written += size;
        ptr += size;
    }
    close(fd);
}

void start_server(const std::string &progpath, const std::string &tempfile) {
    char execpath[PATH_MAX + 1];
    realpath(progpath.c_str(), execpath); // Resolve to absolute path
    std::string server_bin = dirname(execpath);
    server_bin += "/../libexec/pm/server.py";
    const char *const argv[] = {server_bin.c_str(), tempfile.c_str(), NULL};

    while (1) {
        // Infinite loop to restart server if it ever crashes until
        // shutdown signal is received
        cv.wait(mtx, []() { return server_not_running || shutting_down; });
        if (shutting_down) {
            mtx.unlock();
            // Wait for the server to gracefully shutdown itself for at most
            // five seconds before force killing
            for (int t = 0; t < 5; ++t) {
                if (waitpid(server_pid, NULL, WNOHANG) == -1 && errno == ECHILD) {
                    return;
                }
            }
            log("Server not responding, force shutting down...");
            kill(server_pid, SIGKILL);
            return;
        }
        if (server_not_running) {
            log("Starting server...");
            server_not_running = false;
            cv.notify_all();
            mtx.unlock();
            server_pid = fork();
            if (server_pid == 0) {
                if (execv(argv[0], const_cast<char *const *>(argv)) == -1) {
                    const char *msg = (errno == ENOENT) ? "server.py not found." :
                        "Unknown error occurred when calling server.py.";
                    std::cerr << "Error: " << msg << std::endl;

                    // This is a hack that executes `bin/sh -c "exit 127"' so
                    // that the subprocess exits with status 127 without
                    // returning.
                    //
                    // The reason exit(127) doesn't work is that we're within a
                    // thread, so exit leads to the process being terminated
                    // with SIGABRT.
                    const char *const argv2[] = {"/bin/sh", "-c", "exit 127", NULL};
                    execv(argv2[0], const_cast<char *const *>(argv2));
                }
            }
        }
    }
}

int get_mtime(const std::string &filepath, timespec &mtime) {
    struct stat st;
    const char *path = filepath.c_str();
    if (stat(path, &st) == -1) {
        return -1;
    }
#ifdef HAVE_STRUCT_STAT_ST_MTIMESPEC
    mtime = st.st_mtimespec;
#elif HAVE_STRUCT_STAT_ST_MTIM
    mtime = st.st_mtim;
#else
    mtime.tv_sec = st.st_mtime;
    mtime.tv_nsec = 0;
#endif
    return 0;
}

void watch_for_changes(const std::string &manfile, const std::string &tempfile,
                       const timespec &initial_mtime) {
    timespec last_mtime = initial_mtime;
    timespec mtime;
    // Poll for changes
    while (true) {
        if (shutting_down) {
            break;
        }
        if (get_mtime(manfile, mtime) == -1) {
            std::cerr << "Warning: Failed to stat " << manfile << "." << std::endl;
            sleep(2);
        }
        if (last_mtime < mtime) {
            log("Change detected.");
            write_to_file(to_html(run_man(manfile)), tempfile);
            kill(server_pid, SIGUSR1);
            last_mtime = mtime;
        }
        sleep(1);
    }
}

void sigchld_listener(int sig) {
    // Reap server process; WNOHANG to ignore when server is merely stopped
    int status;
    if (waitpid(server_pid, &status, WNOHANG) == server_pid) {
        if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
            // Unrecoverable failure due to failed execvp
            print_error_and_initiate_shutdown("Unrecoverable server failure.");
            return;
        }
        log("Server crashed...");
        mtx.lock();
        server_not_running = true;
        mtx.unlock();
        cv.notify_all();
    }
}

void sigint_term_listener(int sig) {
    mtx.lock();
    shutting_down = true;
    mtx.unlock();
    cv.notify_all();
}

bool operator <(const timespec& lhs, const timespec& rhs) {
    if (lhs.tv_sec == rhs.tv_sec) {
        return lhs.tv_nsec < rhs.tv_nsec;
    } else {
        return lhs.tv_sec < rhs.tv_sec;
    }
}

// Local Variables:
// c-basic-offset: 4
// End:
