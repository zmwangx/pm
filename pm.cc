#include <condition_variable>
#include <fcntl.h>
#include <iostream>
#include <libgen.h>
#include <mutex>
#include <signal.h>
#include <stdexcept>
#include <stdlib.h>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <time.h>
#include <unistd.h>

std::thread server_controller_thread;
pid_t server_pid = 0;
bool server_not_running = true;
bool shutting_down = false;
std::mutex mtx;
std::condition_variable_any cv;

class PMException: public std::runtime_error {
public:
    PMException(std::string const &message): std::runtime_error(message) {}
};

void log(const char *msg);
std::string run_man(const std::string &manfile);
std::string to_html(const std::string &man_string);
int get_tempfile(std::string &tempfile);
void write_to_file(const std::string &str, const std::string &filepath);
void start_server(const std::string &progpath, const std::string &tempfile);
int get_mtime(const std::string &filepath, timespec &mtime);
void watch_for_changes(const std::string &manfile, const std::string &tempfile,
                       const timespec &initial_mtime);
void sigchld_listener(int sig);
void sigint_term_listener(int sig);
bool operator <(const timespec& lhs, const timespec& rhs);

int main(int argc, const char *argv[]) {
    try {
        if (argc == 1) {
            throw PMException("No man page provided.");
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
        std::cerr << "Error: " << e.what() << std::endl;
        exit(1);
    }
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
    char *execpath = strdup(progpath.c_str()); // won't be freed
    std::string server_bin = dirname(execpath);
    server_bin += "/../libexec/pm/server.py";
    const char *const argv[] = {server_bin.c_str(), tempfile.c_str(), NULL};

    while (1) {
        // Infinite loop to restart server if it ever crashes until
        // shutdown signal is received
        cv.wait(mtx, []() { return server_not_running || shutting_down; });
        if (server_not_running) {
            log("Starting server...");
            server_not_running = false;
            cv.notify_all();
            mtx.unlock();
            server_pid = fork();
            if (server_pid == 0) {
                if (execvp(argv[0], const_cast<char *const *>(argv)) == -1 &&
                    errno == ENOENT) {
                    throw PMException("server.py not found.");
                } else {
                    throw PMException("Unknown error occurred when calling server.py.");
                }
            }
        } else if (shutting_down) {
            mtx.unlock();
            // Wait for the server to gracefully shutdown itself for at most
            // five seconds before force killing
            for (int t = 0; t < 5; ++t) {
                if (waitpid(server_pid, NULL, WNOHANG) == -1 && errno == ECHILD) {
                    exit(0);
                }
                return;
            }
            log("Server not responding, force shutting down...");
            kill(server_pid, SIGKILL);
            return;
        }
    }
}

int get_mtime(const std::string &filepath, timespec &mtime) {
    struct stat st;
    const char *path = filepath.c_str();
    if (stat(path, &st) == -1) {
        return -1;
    }
    mtime = st.st_mtimespec;
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
    if (waitpid(server_pid, NULL, WNOHANG) == server_pid) {
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
