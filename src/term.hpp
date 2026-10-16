// Terminal helpers: colours, a spinner and a small line editor (no readline).
#pragma once
#include <termios.h>
#include <functional>
#include <string>
#include <utility>
#include <vector>

extern bool g_color_out, g_color_err;  // decided once at startup (isatty + NO_COLOR)

// ANSI codes, or "" when colour is off for that stream.
const char* co(const char* code);  // stdout
const char* ce(const char* code);  // stderr

namespace ansi {
constexpr const char* reset = "\x1b[0m";
constexpr const char* bold = "\x1b[1m";
constexpr const char* dim = "\x1b[2m";
constexpr const char* italic = "\x1b[3m";
constexpr const char* red = "\x1b[31m";
constexpr const char* green = "\x1b[32m";
constexpr const char* yellow = "\x1b[33m";
constexpr const char* blue = "\x1b[34m";
constexpr const char* magenta = "\x1b[35m";
constexpr const char* cyan = "\x1b[36m";
constexpr const char* gray = "\x1b[90m";
constexpr const char* code = "\x1b[38;5;180m";  // fenced code in replies
}  // namespace ansi

int term_cols();
size_t display_width(const std::string& s);  // columns, ignoring ANSI escapes

// One-line activity indicator on stderr ("⠋ thinking 1.2s").
class Spinner {
public:
    void start(const std::string& label);
    void label(const std::string& label) { label_ = label; }
    void tick();
    void stop();
    bool active() const { return active_; }

private:
    bool active_ = false;
    std::string label_;
    double t0_ = 0, last_ = 0;
    int frame_ = 0;
};

class LineEditor {
public:
    enum Result { Line, Eof, Cancel };
    using Completer = std::function<void(const std::string& buf, size_t& word_start, std::vector<std::string>& out)>;

    using MenuItems = std::vector<std::pair<std::string, std::string>>;  // name, description
    // Live list under the prompt while the line is a bare "/word".
    std::function<void(const std::string& word, MenuItems& out)> menu;

    explicit LineEditor(int fd) : fd_(fd) {}
    // `prompt` may contain ANSI codes; `prompt_width` is its visible width.
    Result read(const std::string& prompt, size_t prompt_width, std::string& out);

    void history_load(const std::string& path);
    void history_add(const std::string& line);
    Completer completer;

private:
    int fd_;
    termios saved_{};
    std::string buf_;
    size_t pos_ = 0, scroll_ = 0;
    std::vector<std::string> hist_;
    std::string hist_path_;
    bool listed_ = false;
    MenuItems items_;
    std::string items_for_;
    size_t sel_ = 0, drawn_rows_ = 0;

    bool raw(bool on);
    int getc(int timeout_ms);
    void refresh(const std::string& prompt, size_t pw);
    void insert(const std::string& s);
    void complete(const std::string& prompt, size_t pw);
    size_t prev_char(size_t p) const;
    size_t next_char(size_t p) const;
    size_t prev_word(size_t p) const;
    size_t next_word(size_t p) const;
    void update_menu();
    void put(const std::string& s);
};
