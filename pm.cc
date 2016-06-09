#include <iostream>
#include <sstream>
#include <stdexcept>
#include <stdlib.h>
#include <string>
#include <unistd.h>

class PMException: public std::runtime_error {
public:
    PMException(std::string const &message): std::runtime_error(message) {}
};

std::string run_man(const std::string &filename);
std::string to_html(const std::string &man_string);
std::string write_to_tempfile(const std::string &str);

int main(int argc, const char *argv[]) {
    try {
        if (argc == 1) {
            throw PMException("No man page provided.");
        } else if (argc > 2) {
            std::cerr << "Warning: Extraneous arguments ignored." << std::endl;
        }
        std::string tempfile = write_to_tempfile(to_html(run_man(argv[1])));
        std::cout << tempfile << std::endl;
    } catch (PMException &e) {
        std::cerr << "Error: " << e.what() << std::endl;
        exit(1);
    }
}

// TODO: Customizable COLUMNS
std::string run_man(const std::string &filename) {
    char *path; // path won't be freed; it's okay
    if ((path = realpath(filename.c_str(), NULL)) == NULL) {
        throw PMException("Cannot resolve " + filename + ".");
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
    console.log('refresh')
    document.getElementById('manpage').innerHTML = JSON.parse(e.data).content
  })
  source.addEventListener('bye', function (e) {
    console.log('bye')
    source.close()
  })
})()
</script>
</body>
</html>
)_";

    return hs;
}

std::string write_to_tempfile(const std::string &str) {
    char name[] = "/tmp/pm-XXXXXX.html";
    int fd = mkstemps(name, 5);
    if (fd == -1) {
        throw PMException("Failed to create temp file.");
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
    return std::string(name);
}

// Local Variables:
// c-basic-offset: 4
// End:
