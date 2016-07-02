#include <condition_variable>
#include <fcntl.h>
#include <iostream>
#include <libgen.h>
#include <limits.h>
#include <mutex>
#include <signal.h>
#include <sstream>
#include <stdexcept>
#include <stdlib.h>
#include <string>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <time.h>
#include <unistd.h>

#ifdef __linux__
#include <pty.h>
#endif

#include "config.h"
#include "config-paths.h"

const timespec TENTH_SECOND{0, 100000000};
const timespec HALF_SECOND{0, 500000000};

std::thread server_controller_thread;
pid_t server_pid = 0;
bool server_not_running = true;
bool shutting_down = false;
int exit_status = 0;
std::mutex mtx;
std::condition_variable_any cv;

// Options
int columns = 120;

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

/**
 * Calculates the path to server.py (../libexec/pm/server.py).
 *
 * This routine does not check the existence of the calculated path.
 *
 * @param argv0 argv[0].
 * @returns The path to server.py.
 */
std::string get_server_path(const char *argv0);

/**
 * Prints error message and initiates shutdown sequence.
 *
 * shutting_down is set to true, exit_status is set to 1, and all watchers of
 * cv are notified.
 *
 * @param (Optional) The error message to be printed; suppressed if
 * NULL. Default is NULL.
 */
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
 * @param filepath Path to the man page source file, used for determining the
 * title of the HTML file.
 * @returns An HTML string ready to be written to file.
 * @see run_man
 * @see write_to_file
 */
std::string to_html(const std::string &man_string, const std::string &filepath);

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
 * Starts the HTTP server.
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
 * @param progpath Path to server.py.
 * @param tempfile Path to the tempfile which contains the HTML version of the
 * man page.
 * @see get_server_path
 * @see get_tempfile
 * @see write_to_file
 */
void start_server(const std::string &server_path, const std::string &tempfile);

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

/**
 * Converts string to integer.
 *
 * Although the return type is int, only unsigned integers are supported.
 *
 * This function does not check for overflow, so use it with sane values.
 *
 * @param s A string supposedly containing an unsigned integer.
 * @returns The converted integer on success, and -1 otherwise.
 */
int string2unsigned(const std::string &s);

int main(int argc, const char *argv[]) {
    // Parse arguments
    std::string manfile;
    try {
        int optind = 1; // option index
        while (optind < argc) {
            std::string opt = argv[optind];
            if (opt == "-h" || opt == "--help") {
                print_help();
                exit(1);
            } else if (opt == "-V" || opt == "--version") {
                std::cerr << "pm " << PM_VERSION << std::endl;
                exit(1);
            } else if (opt == "-w" || opt == "--width" || opt == "--columns") {
                ++optind;
                std::string arg = argv[optind];
                columns = string2unsigned(arg);
                if (columns == -1) {
                    throw PMException("Invalid width " + arg + ".");
                }
            } else if (opt == "--") {
                break;
            } else if (opt.compare(0, 1, "-") == 0) {
                throw PMException("Unknown option " + opt + ".");
            } else {
                break;
            }
            ++optind;
        }
        if (optind >= argc) {
            throw PMException("Missing man page source file.");
        } else if (optind + 1 < argc) {
            std::cerr << "Warning: Extraneous arguments ignored." << std::endl;
        }
        manfile = argv[optind];
    } catch (PMException &e) {
        std::cerr << "Error: " << e.what() << std::endl << std::endl;
        print_help();
        exit(1);
    }
    // Done parsing arguments

    try {
        timespec initial_mtime;
        if (get_mtime(manfile.c_str(), initial_mtime) == -1) {
            throw PMException("Failed to stat " + manfile + ".");
        }

        std::string tempfile;
        int fd = get_tempfile(tempfile);
        if (fd == -1) {
            throw PMException("Failed to create temp file.");
        }
        close(fd);
        write_to_file(to_html(run_man(manfile), manfile), tempfile);

        signal(SIGCHLD, sigchld_listener);
        signal(SIGINT, sigint_term_listener);
        signal(SIGTERM, sigint_term_listener);

        std::string server_path = get_server_path(argv[0]);
        server_controller_thread = std::thread(start_server, server_path, tempfile);
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
    -V, --version
        Print version info and exit.
    -w, --width, --columns=WIDTH
        Width of output, i.e., the COLUMNS environment variable passed to
        man(1).
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

std::string get_server_path(const char *argv0) {
    std::string pm_executable_path;

    if (strchr(argv0, '/') != NULL) {
        // argv0 is a path
        pm_executable_path = argv0;
    } else {
        // Infer from $PATH
        char *paths = strdup(getenv("PATH")); // Duplicate the string so that
                                              // we do not modify it in place
        char *path = strtok(paths, ":");
        while (path != NULL) {
            std::string potential_pm_path = path;
            potential_pm_path += "/pm";
            if (access(potential_pm_path.c_str(), X_OK) == 0) {
                pm_executable_path = potential_pm_path;
            }
            path = strtok(NULL, ":");
        }
    }

    if (pm_executable_path.empty()) {
        // Previous methods failed, use hard-coded path instead
        pm_executable_path = BINDIR;
        pm_executable_path += "/pm";
    }

    char pm_executable_realpath[PATH_MAX + 1];
    realpath(pm_executable_path.c_str(), pm_executable_realpath); // Resolve to absolute path
    std::string server_executable_path = dirname(pm_executable_realpath);
    server_executable_path += "/../libexec/pm/server.py";
    return server_executable_path;
}

void print_error_and_initiate_shutdown(const char *msg=NULL) {
    if (msg) {
        std::cerr << "Error: " << msg << std::endl;
    }
    mtx.lock();
    shutting_down = true;
    exit_status = 1;
    cv.notify_all();
    mtx.unlock();
}

std::string run_man(const std::string &manfile) {
    char *pathstr;
    if ((pathstr = realpath(manfile.c_str(), NULL)) == NULL) {
        throw PMException("Cannot resolve " + manfile + ".");
    }
    std::string path(pathstr);
    free(pathstr);

    setenv("COLUMNS", std::to_string(columns).c_str(), 1);

    int readfd; // The fd to read man(1) output from, which is the master of
                // the pty in the Linux implementation, and the reading end of
                // the pipe in the BSD/generic implementation

#ifdef __linux__
    // Linux implementation based on pseudotty
    int master;
    int slave;
    if (openpty(&master, &slave, NULL, NULL, NULL) == -1) {
        throw PMException("Failed to open pseudotty for man(1).");
    }
    readfd = master;
    // Temporarily redirect stdout to the slave
    int _stdout = dup(STDOUT_FILENO);
    dup2(slave, STDOUT_FILENO);
#else
    // BSD/generic implementation based on pipe
    int fds[2];
    pipe(fds);
    readfd = fds[0];
#endif

    pid_t pid = fork();
    if (pid == 0) {
#ifndef __linux__
        // Set up pipe for BSD/generic
        dup2(fds[1], STDOUT_FILENO);
        close(fds[0]);
        close(fds[1]);
#endif

        const char *argv[] = {"man", "-P", "/bin/cat", path.c_str(), NULL};
        if (execvp(argv[0], const_cast<char *const *>(argv)) == -1 &&
            errno == ENOENT) {
            throw PMException("man(1) not found.");
        } else {
            throw PMException("Unknown error occurred when calling man(1).");
        }
    }

#ifndef __linux__
    close(fds[1]);
#endif

    int status;
    fd_set rfds;
    struct timeval tv{0, 0};
    char buf[4097];
    std::string str;
    ssize_t size;

    while (1) {
        // Read as we wait, so that the child process doesn't hang due to
        // saturating the pty or pipe's buffer.
        if (waitpid(pid, &status, WNOHANG) == pid) {
            // Child process finished
            if (!WIFEXITED(status) || !(WEXITSTATUS(status) == 0)) {
                throw PMException("Call to man(1) failed.");
            }
            break;
        }

        // Read if there's anything to read
        FD_ZERO(&rfds);
        FD_SET(readfd, &rfds);
        if (select(readfd + 1, &rfds, NULL, NULL, &tv)) {
            size = read(readfd, buf, 4096);
            if (size == -1) {
#ifdef __linux__
                throw PMException("Failed to read from pty.");
#else
                throw PMException("Failed to read from pipe.");
#endif
            }
            buf[size] = '\0';
            str += buf;
        }
    }

#ifdef __linux__
    // Flush and restore stdout
    fsync(STDOUT_FILENO);
    dup2(_stdout, STDOUT_FILENO);
#endif

    // Read until there's no more to read, then if Linux, close both ends of
    // the pty.
    while (1) {
        FD_ZERO(&rfds);
        FD_SET(readfd, &rfds);
        if (!select(readfd + 1, &rfds, NULL, NULL, &tv)) {
            // No more data to read
            break;
        }
        size = read(readfd, buf, 4096);
        if (size == 0) {
            break;
        } else if (size == -1) {
#ifdef __linux__
            throw PMException("Failed to read from pty.");
#else
            throw PMException("Failed to read from pipe.");
#endif
        }
        buf[size] = '\0';
        str += buf;
    }

#ifdef __linux__
    close(master);
    close(slave);
#endif

    return str;
}

std::string to_html(const std::string &man_string, const std::string &filepath) {
    char *filepathstr = strdup(filepath.c_str());
    std::string filename = basename(filepathstr);
    free(filepathstr);

    // Encode each character in the filename as entity code for the title
    std::ostringstream oss;
    for (auto c: filename) {
        oss << "&#" << (int) c << ';';
    }
    std::string title = oss.str();
    if (title.empty()) {
        title = "Man page";
    }

    std::string ms = man_string;
    const size_t n = ms.length();
    ms += "\0\0"; // Append two NULs to the string so that we won't need to
                  // worry about index out of bound

    // The HTML string to be constructed from ms
    std::string hs = R"(<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<title>)";
    hs += title;
    hs += R"(</title>
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

void start_server(const std::string &server_path, const std::string &tempfile) {
    const char *const argv[] = {server_path.c_str(), tempfile.c_str(), NULL};

    while (1) {
        // Infinite loop to restart server if it ever crashes until
        // shutdown signal is received
        cv.wait(mtx, []() { return server_not_running || shutting_down; });
        if (shutting_down) {
            mtx.unlock();
            // Wait for the server to gracefully shutdown itself for at most
            // five seconds before force killing
            for (int t = 0; t < 50; ++t) {
                if (waitpid(server_pid, NULL, WNOHANG) == -1 && errno == ECHILD) {
                    return;
                }
                nanosleep(&TENTH_SECOND, NULL);
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
            write_to_file(to_html(run_man(manfile), manfile), tempfile);
            kill(server_pid, SIGUSR1);
            last_mtime = mtime;
        }
        nanosleep(&HALF_SECOND, NULL);
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
        if (!shutting_down) {
            log("Server crashed...");
            mtx.lock();
            server_not_running = true;
            mtx.unlock();
            cv.notify_all();
        }
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

int string2unsigned(const std::string &s) {
    int sum = 0;
    for (char c: s) {
        int ord = (int)c;
        if (ord >= 48 && ord <= 57) {
            sum = sum * 10 + ord - 48;
        } else {
            return -1;
        }
    }
    return sum;
}

// Local Variables:
// c-basic-offset: 4
// End:
