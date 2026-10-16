#include "util.hpp"

#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

volatile std::sig_atomic_t g_interrupted = 0;

double now_secs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ')
        ++a;
    while (b > a && (unsigned char)s[b - 1] <= ' ')
        --b;
    return s.substr(a, b - a);
}

bool starts_with(const std::string& s, const char* p) {
    size_t n = strlen(p);
    return s.size() >= n && s.compare(0, n, p) == 0;
}

bool ends_with(const std::string& s, const char* p) {
    size_t n = strlen(p);
    return s.size() >= n && s.compare(s.size() - n, n, p) == 0;
}

std::string lower(std::string s) {
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

std::vector<std::string> split_args(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    bool in_word = false;
    char quote = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (quote) {
            if (c == quote)
                quote = 0;
            else if (c == '\\' && quote == '"' && i + 1 < s.size())
                cur += s[++i];
            else
                cur += c;
        } else if (c == '"' || c == '\'') {
            quote = c;
            in_word = true;
        } else if (c == ' ' || c == '\t' || c == '\n') {
            if (in_word) out.push_back(cur);
            cur.clear();
            in_word = false;
        } else {
            if (c == '\\' && i + 1 < s.size()) c = s[++i];
            cur += c;
            in_word = true;
        }
    }
    if (in_word) out.push_back(cur);
    return out;
}

std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> out;
    size_t a = 0;
    while (a < s.size()) {
        size_t nl = s.find('\n', a);
        if (nl == std::string::npos) nl = s.size();
        out.push_back(s.substr(a, nl - a));
        a = nl + 1;
    }
    return out;
}

size_t count_lines(const std::string& s) {
    if (s.empty()) return 0;
    size_t n = 0;
    for (char c : s)
        n += c == '\n';
    return n + (s.back() != '\n');
}

std::string fmt(const char* f, ...) {
    char small[256];
    va_list ap;
    va_start(ap, f);
    int n = vsnprintf(small, sizeof small, f, ap);
    va_end(ap);
    if (n < 0) return {};
    if ((size_t)n < sizeof small) return std::string(small, (size_t)n);
    std::string out((size_t)n + 1, '\0');
    va_start(ap, f);
    vsnprintf(&out[0], out.size(), f, ap);
    va_end(ap);
    out.resize((size_t)n);
    return out;
}

std::string human_bytes(double b) {
    const char* u[] = {"B", "KB", "MB", "GB", "TB"};
    int i = 0;
    while (b >= 1000 && i < 4)
        b /= 1000, ++i;
    return i == 0 ? fmt("%.0f %s", b, u[i]) : fmt("%.1f %s", b, u[i]);
}

std::string plural(size_t n, const char* one, const char* many) {
    return fmt("%zu %s", n, n == 1 ? one : many ? many : (std::string(one) + "s").c_str());
}

std::string human_tokens(long long n) {
    if (n < 1000) return fmt("%lld", n);
    if (n < 100000) return fmt("%.1fk", (double)n / 1000);
    return fmt("%lldk", n / 1000);
}

std::string human_ctx(long long n) {
    return n >= 1024 && n % 1024 == 0 ? fmt("%lldk", n / 1024) : human_tokens(n);
}

std::string home_dir() {
    const char* h = getenv("HOME");
    if (h && *h) return h;
    passwd* pw = getpwuid(getuid());
    return pw && pw->pw_dir ? pw->pw_dir : "/";
}

std::string expand_home(const std::string& p) {
    if (p == "~") return home_dir();
    if (starts_with(p, "~/")) return home_dir() + p.substr(1);
    return p;
}

std::string cwd() {
    char buf[4096];
    return getcwd(buf, sizeof buf) ? buf : ".";
}

bool is_file(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool is_dir(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool read_file(const std::string& path, std::string& out, std::string& err) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        err = path + ": " + strerror(errno);
        return false;
    }
    out.clear();
    char buf[65536];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n > 0)
            out.append(buf, (size_t)n);
        else if (n == 0)
            break;
        else if (errno != EINTR) {
            err = path + ": " + strerror(errno);
            close(fd);
            return false;
        }
    }
    close(fd);
    return true;
}

bool make_dirs(const std::string& dir) {
    if (dir.empty() || is_dir(dir)) return true;
    size_t slash = dir.find_last_of('/');
    if (slash != std::string::npos && slash > 0 && !make_dirs(dir.substr(0, slash))) return false;
    return mkdir(dir.c_str(), 0755) == 0 || errno == EEXIST;
}

bool write_file(const std::string& path, const std::string& data, std::string& err) {
    size_t slash = path.find_last_of('/');
    if (slash != std::string::npos && slash > 0 && !make_dirs(path.substr(0, slash))) {
        err = path + ": cannot create parent directory: " + strerror(errno);
        return false;
    }
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        err = path + ": " + strerror(errno);
        return false;
    }
    const char* p = data.data();
    size_t left = data.size();
    while (left) {
        ssize_t n = write(fd, p, left);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            err = path + ": " + strerror(errno);
            close(fd);
            return false;
        }
        p += n;
        left -= (size_t)n;
    }
    return close(fd) == 0 || (err = path + ": " + strerror(errno), false);
}

size_t utf8_len(const unsigned char* p, size_t n) {
    if (!n) return 0;
    unsigned char c = p[0];
    if (c < 0x80) return 1;
    auto cont = [&](size_t i) { return i < n && (p[i] & 0xC0) == 0x80; };
    if (c >= 0xC2 && c <= 0xDF) return cont(1) ? 2 : 0;
    if (c >= 0xE0 && c <= 0xEF) {
        if (n < 2) return 0;
        if (c == 0xE0 && (p[1] < 0xA0 || p[1] > 0xBF)) return 0;
        if (c == 0xED && (p[1] < 0x80 || p[1] > 0x9F)) return 0;
        return cont(1) && cont(2) ? 3 : 0;
    }
    if (c >= 0xF0 && c <= 0xF4) {
        if (n < 2) return 0;
        if (c == 0xF0 && (p[1] < 0x90 || p[1] > 0xBF)) return 0;
        if (c == 0xF4 && (p[1] < 0x80 || p[1] > 0x8F)) return 0;
        return cont(1) && cont(2) && cont(3) ? 4 : 0;
    }
    return 0;
}

size_t utf8_floor(const std::string& s, size_t max) {
    if (max >= s.size()) return s.size();
    while (max > 0 && ((unsigned char)s[max] & 0xC0) == 0x80)
        --max;
    return max;
}

std::string utf8_clip(const std::string& s, size_t max) {
    if (s.size() <= max) return s;
    return s.substr(0, utf8_floor(s, max)) + "…";
}

std::string utf8_clean(const std::string& s) {
    const unsigned char* p = (const unsigned char*)s.data();
    size_t n = s.size(), i = 0;
    // fast path: already valid
    while (i < n) {
        size_t l = utf8_len(p + i, n - i);
        if (!l) break;
        i += l;
    }
    if (i == n) return s;
    std::string out(s, 0, i);
    while (i < n) {
        size_t l = utf8_len(p + i, n - i);
        if (l)
            out.append(s, i, l), i += l;
        else
            out += "\xEF\xBF\xBD", ++i;
    }
    return out;
}

void utf8_append(std::string& out, unsigned cp) {
    if (cp < 0x80)
        out += (char)cp;
    else if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

std::string clean_terminal_output(const std::string& s) {
    std::string noesc;
    noesc.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c != '\x1b') {
            noesc += c;
            continue;
        }
        if (i + 1 >= s.size()) break;
        char k = s[i + 1];
        if (k == '[') {  // CSI: params then a final byte 0x40..0x7E
            size_t j = i + 2;
            while (j < s.size() && !((unsigned char)s[j] >= 0x40 && (unsigned char)s[j] <= 0x7E))
                ++j;
            i = j;
        } else if (k == ']') {  // OSC: until BEL or ESC backslash
            size_t j = i + 2;
            while (j < s.size() && s[j] != '\a' && !(s[j] == '\x1b' && j + 1 < s.size() && s[j + 1] == '\\'))
                ++j;
            i = (j < s.size() && s[j] == '\x1b') ? j + 1 : j;
        } else {
            i += 1;
        }
    }
    // "10%\r20%\r100%\n" -> "100%\n": keep what a terminal would finally show
    std::string out;
    out.reserve(noesc.size());
    size_t a = 0;
    while (a <= noesc.size()) {
        size_t nl = noesc.find('\n', a);
        bool last = nl == std::string::npos;
        if (last) nl = noesc.size();
        size_t end = nl;
        while (end > a && noesc[end - 1] == '\r')
            --end;
        size_t cr = noesc.rfind('\r', end ? end - 1 : 0);
        size_t start = (cr != std::string::npos && cr >= a && cr < end) ? cr + 1 : a;
        out.append(noesc, start, end - start);
        if (last) break;
        out += '\n';
        a = nl + 1;
    }
    return out;
}

std::string head_tail(const std::string& s, size_t max) {
    if (s.size() <= max) return s;
    size_t head = utf8_floor(s, max / 4);
    size_t nl = s.rfind('\n', head);
    if (nl != std::string::npos && nl > head / 2) head = nl + 1;
    size_t tail_start = s.size() - (max - head);
    size_t nl2 = s.find('\n', tail_start);
    if (nl2 != std::string::npos && nl2 < tail_start + (max - head) / 2) tail_start = nl2 + 1;
    while (tail_start < s.size() && ((unsigned char)s[tail_start] & 0xC0) == 0x80)
        ++tail_start;
    size_t dropped = tail_start - head;
    return s.substr(0, head) +
           fmt("\n[... %zu bytes / %zu lines omitted ...]\n", dropped, count_lines(s.substr(head, dropped))) +
           s.substr(tail_start);
}

std::string find_in_path(const std::string& exe) {
    if (exe.find('/') != std::string::npos) return access(exe.c_str(), X_OK) == 0 ? exe : "";
    const char* path = getenv("PATH");
    std::string p = path ? path : "/usr/bin:/bin";
    size_t a = 0;
    while (a <= p.size()) {
        size_t c = p.find(':', a);
        if (c == std::string::npos) c = p.size();
        std::string dir = p.substr(a, c - a);
        if (dir.empty()) dir = ".";
        std::string full = dir + "/" + exe;
        if (is_file(full) && access(full.c_str(), X_OK) == 0) return full;
        a = c + 1;
    }
    return "";
}

int run_attached(const std::vector<std::string>& argv) {
    if (argv.empty()) return 127;
    std::vector<char*> av;
    for (auto& a : argv)
        av.push_back(const_cast<char*>(a.c_str()));
    av.push_back(nullptr);
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) return 127;
    if (pid == 0) {
        signal(SIGPIPE, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        execvp(av[0], av.data());
        _exit(127);
    }
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
    }
    g_interrupted = 0;  // a ^C meant for the child is not ours
    if (WIFEXITED(st)) return WEXITSTATUS(st);
    return 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
}
