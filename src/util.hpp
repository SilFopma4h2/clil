// Small helpers shared by every module: strings, files, UTF-8, processes.
#pragma once
#include <csignal>
#include <cstddef>
#include <string>
#include <vector>

// Set by the SIGINT handler; long-running loops poll it and bail out.
extern volatile std::sig_atomic_t g_interrupted;

double now_secs();
std::string trim(const std::string& s);
bool starts_with(const std::string& s, const char* prefix);
bool ends_with(const std::string& s, const char* suffix);
std::string lower(std::string s);
std::vector<std::string> split_args(const std::string& s);  // shell-like, honours quotes
std::vector<std::string> split_lines(const std::string& s);
size_t count_lines(const std::string& s);

std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
std::string human_bytes(double bytes);
std::string plural(size_t n, const char* one, const char* many = nullptr);  // "1 line", "2 lines"
std::string human_tokens(long long n);                                      // 1234 -> "1.2k"
std::string human_ctx(long long n);                                         // 4096 -> "4k"

std::string home_dir();
std::string expand_home(const std::string& path);
std::string cwd();
bool is_file(const std::string& path);
bool is_dir(const std::string& path);
bool read_file(const std::string& path, std::string& out, std::string& err);
bool write_file(const std::string& path, const std::string& data, std::string& err);
bool make_dirs(const std::string& dir);  // mkdir -p

// UTF-8: length of a valid sequence at p (0 if invalid), safe truncation, cleanup.
size_t utf8_len(const unsigned char* p, size_t n);
size_t utf8_floor(const std::string& s, size_t max_bytes);
std::string utf8_clip(const std::string& s, size_t max_bytes);  // adds "…" when clipped
std::string utf8_clean(const std::string& s);                   // invalid bytes -> U+FFFD
void utf8_append(std::string& out, unsigned cp);

// Strip ANSI escapes and collapse carriage-return progress lines (saves tokens).
std::string clean_terminal_output(const std::string& s);

// Keep head and tail of an oversized text, marking what was dropped.
std::string head_tail(const std::string& s, size_t max_bytes);

// Run argv with the terminal attached (for `lms`, `!cmd`). Returns exit code, 127 if not found.
int run_attached(const std::vector<std::string>& argv);
std::string find_in_path(const std::string& exe);
