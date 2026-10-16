#include "term.hpp"
#include "util.hpp"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>

bool g_color_out = false, g_color_err = false;

const char* co(const char* code) {
    return g_color_out ? code : "";
}
const char* ce(const char* code) {
    return g_color_err ? code : "";
}

int term_cols() {
    winsize ws;
    for (int fd : {2, 1, 0})
        if (ioctl(fd, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) return ws.ws_col;
    return 80;
}

static unsigned decode_cp(const std::string& s, size_t i, size_t len) {
    const unsigned char* p = (const unsigned char*)s.data() + i;
    if (len == 1) return p[0];
    if (len == 2) return ((p[0] & 0x1Fu) << 6) | (p[1] & 0x3Fu);
    if (len == 3) return ((p[0] & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu);
    return ((p[0] & 0x07u) << 18) | ((p[1] & 0x3Fu) << 12) | ((p[2] & 0x3Fu) << 6) | (p[3] & 0x3Fu);
}

static int cp_width(unsigned cp) {
    if ((cp >= 0x300 && cp <= 0x36F) || cp == 0x200B || cp == 0x200D || (cp >= 0xFE00 && cp <= 0xFE0F)) return 0;
    if ((cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF) || (cp >= 0xAC00 && cp <= 0xD7A3) ||
        (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFE30 && cp <= 0xFE4F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
        (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x1F300 && cp <= 0x1F64F) || (cp >= 0x1F900 && cp <= 0x1F9FF) ||
        (cp >= 0x20000 && cp <= 0x3FFFD))
        return 2;
    return 1;
}

size_t display_width(const std::string& s) {
    size_t w = 0;
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '\x1b' && i + 1 < s.size() && s[i + 1] == '[') {
            i += 2;
            while (i < s.size() && !((unsigned char)s[i] >= 0x40 && (unsigned char)s[i] <= 0x7E))
                ++i;
            ++i;
            continue;
        }
        size_t l = utf8_len((const unsigned char*)s.data() + i, s.size() - i);
        if (!l) l = 1;
        w += (size_t)cp_width(decode_cp(s, i, l));
        i += l;
    }
    return w;
}

// ---------------------------------------------------------------- spinner

void Spinner::start(const std::string& label) {
    active_ = isatty(2);
    label_ = label;
    t0_ = now_secs();
    last_ = 0;
    tick();
}

void Spinner::tick() {
    static const char* frames[] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
    if (!active_) return;
    double t = now_secs();
    if (t - last_ < 0.08) return;
    last_ = t;
    fprintf(stderr, "\r\x1b[K%s%s%s %s%s %.1fs%s", ce(ansi::cyan), frames[frame_++ % 10], ce(ansi::reset),
            ce(ansi::dim), label_.c_str(), t - t0_, ce(ansi::reset));
    fflush(stderr);
}

void Spinner::stop() {
    if (!active_) return;
    fputs("\r\x1b[K", stderr);
    fflush(stderr);
    active_ = false;
}

// ---------------------------------------------------------------- line editor

void LineEditor::put(const std::string& s) {
    size_t off = 0;
    while (off < s.size()) {
        ssize_t n = write(fd_, s.data() + off, s.size() - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return;
        off += (size_t)n;
    }
}

bool LineEditor::raw(bool on) {
    if (!on) return tcsetattr(fd_, TCSANOW, &saved_) == 0;
    if (!isatty(fd_) || tcgetattr(fd_, &saved_) < 0) return false;
    termios t = saved_;
    t.c_iflag &= ~(tcflag_t)(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    t.c_cflag |= CS8;
    t.c_lflag &= ~(tcflag_t)(ECHO | ICANON | IEXTEN | ISIG);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    return tcsetattr(fd_, TCSANOW, &t) == 0;
}

int LineEditor::getc(int timeout_ms) {
    // select(), not poll(): macOS poll() does not support terminal devices.
    for (;;) {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(fd_, &set);
        timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        int r = select(fd_ + 1, &set, nullptr, nullptr, timeout_ms < 0 ? nullptr : &tv);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        unsigned char c;
        ssize_t n = ::read(fd_, &c, 1);
        if (n < 0 && errno == EINTR) continue;
        return n == 1 ? c : -1;
    }
}

size_t LineEditor::prev_char(size_t p) const {
    if (!p) return 0;
    --p;
    while (p > 0 && ((unsigned char)buf_[p] & 0xC0) == 0x80)
        --p;
    return p;
}

size_t LineEditor::next_char(size_t p) const {
    if (p >= buf_.size()) return buf_.size();
    ++p;
    while (p < buf_.size() && ((unsigned char)buf_[p] & 0xC0) == 0x80)
        ++p;
    return p;
}

static bool is_space(char c) {
    return c == ' ' || c == '\n' || c == '\t';
}

size_t LineEditor::prev_word(size_t p) const {
    while (p > 0 && is_space(buf_[p - 1]))
        --p;
    while (p > 0 && !is_space(buf_[p - 1]))
        --p;
    return p;
}

size_t LineEditor::next_word(size_t p) const {
    while (p < buf_.size() && is_space(buf_[p]))
        ++p;
    while (p < buf_.size() && !is_space(buf_[p]))
        ++p;
    return p;
}

void LineEditor::insert(const std::string& s) {
    buf_.insert(pos_, s);
    pos_ += s.size();
}

void LineEditor::refresh(const std::string& prompt, size_t pw) {
    struct Cell {
        size_t at;
        std::string text;
        size_t w;
    };
    std::vector<Cell> cells;
    const unsigned char* p = (const unsigned char*)buf_.data();
    for (size_t i = 0; i < buf_.size();) {
        unsigned char c = p[i];
        if (c == '\n')
            cells.push_back({i, std::string(ce(ansi::dim)) + "↵" + ce(ansi::reset), 1}), ++i;
        else if (c == '\t')
            cells.push_back({i, " ", 1}), ++i;
        else if (c < 32 || c == 127)
            cells.push_back({i, std::string("^") + (char)(c ^ 0x40), 2}), ++i;
        else {
            size_t l = utf8_len(p + i, buf_.size() - i);
            if (!l)
                cells.push_back({i, "?", 1}), ++i;
            else
                cells.push_back({i, buf_.substr(i, l), (size_t)cp_width(decode_cp(buf_, i, l))}), i += l;
        }
    }
    int cols = term_cols();
    size_t avail = (size_t)cols > pw + 2 ? (size_t)cols - pw - 1 : 1;
    size_t cur = 0;
    for (auto& c : cells) {
        if (c.at >= pos_) break;
        cur += c.w;
    }
    if (cur < scroll_) scroll_ = cur;
    if (cur > scroll_ + avail) scroll_ = cur - avail;
    std::string out = "\r" + prompt;
    size_t col = 0;
    for (auto& c : cells) {
        if (col >= scroll_ && col + c.w <= scroll_ + avail) out += c.text;
        col += c.w;
    }
    out += "\x1b[0K";
    // menu rows below the prompt (window of 8 around the selection); blank leftovers from the last draw
    size_t first = sel_ >= 8 ? sel_ - 7 : 0, rows = std::min<size_t>(items_.size() - std::min(first, items_.size()), 8);
    size_t nw = 0;
    for (auto& it : items_)
        nw = std::max(nw, display_width(it.first));
    for (size_t i = 0; i < std::max(rows, drawn_rows_); ++i) {
        out += "\r\n\x1b[0K";
        if (i >= rows) continue;
        const auto& it = items_[first + i];
        bool on = first + i == sel_;
        size_t room = (size_t)cols > nw + 6 ? (size_t)cols - nw - 6 : 1;
        out += std::string("  ") + ce(on ? ansi::cyan : ansi::gray) + (on ? ce(ansi::bold) : "") + it.first +
               ce(ansi::reset) + std::string(nw - display_width(it.first) + 2, ' ') + ce(ansi::dim) +
               utf8_clip(it.second, room) + ce(ansi::reset);
    }
    if (std::max(rows, drawn_rows_)) out += fmt("\x1b[%zuA", std::max(rows, drawn_rows_));
    drawn_rows_ = rows;
    out += "\r";
    size_t target = pw + (cur - scroll_);
    if (target) out += fmt("\x1b[%zuC", target);
    put(out);
}

void LineEditor::complete(const std::string& prompt, size_t pw) {
    if (!completer) return;
    size_t start = pos_;
    std::vector<std::string> cands;
    completer(buf_.substr(0, pos_), start, cands);
    if (cands.empty() || start > pos_) return;
    std::string word = buf_.substr(start, pos_ - start);
    std::string pre;
    if (cands.size() == 1) {
        pre = cands[0];
        if (pre.back() != '/') pre += ' ';
    } else {
        pre = cands[0];
        for (auto& c : cands) {
            size_t i = 0;
            while (i < pre.size() && i < c.size() && pre[i] == c[i])
                ++i;
            pre.resize(i);
        }
    }
    if (cands.size() == 1 || (pre.size() > word.size() && starts_with(pre, word.c_str()))) {
        buf_.replace(start, pos_ - start, pre);
        pos_ = start + pre.size();
        return;
    }
    if (listed_) return;
    listed_ = true;
    size_t width = 0;
    for (auto& c : cands)
        width = std::max(width, display_width(c));
    width += 2;
    size_t per_row = std::max<size_t>(1, (size_t)term_cols() / width);
    std::string out = "\r\n";
    for (size_t i = 0; i < cands.size(); ++i) {
        out += cands[i];
        if ((i + 1) % per_row == 0 || i + 1 == cands.size())
            out += "\r\n";
        else
            out += std::string(width - display_width(cands[i]), ' ');
    }
    put(out);
    refresh(prompt, pw);
}

void LineEditor::update_menu() {
    items_.clear();
    if (menu && !buf_.empty() && buf_[0] == '/' && buf_.find_first_of(" \t\n") == std::string::npos) menu(buf_, items_);
    if (buf_ != items_for_) {  // new text: highlight an exact match ("/model" before "/models"), else the first
        sel_ = 0;
        for (size_t i = 0; i < items_.size(); ++i)
            if (items_[i].first == buf_) sel_ = i;
    }
    items_for_ = buf_;
    if (sel_ >= items_.size()) sel_ = 0;
}

LineEditor::Result LineEditor::read(const std::string& prompt, size_t pw, std::string& out) {
    out.clear();
    if (!raw(true)) {  // not a terminal: plain line read
        char ch;
        ssize_t n;
        while ((n = ::read(fd_, &ch, 1)) == 1 && ch != '\n')
            out += ch;
        return n <= 0 && out.empty() ? Eof : Line;
    }
    buf_.clear();
    pos_ = scroll_ = 0;
    listed_ = false;
    items_.clear();
    items_for_.clear();
    sel_ = drawn_rows_ = 0;
    size_t hidx = hist_.size();
    std::string saved;
    auto hist_go = [&](int dir) {
        if ((dir < 0 && hidx == 0) || (dir > 0 && hidx >= hist_.size())) return;
        if (hidx == hist_.size()) saved = buf_;
        hidx = (size_t)((long)hidx + dir);
        buf_ = hidx == hist_.size() ? saved : hist_[hidx];
        pos_ = buf_.size();
    };

    put("\x1b[?2004h");  // bracketed paste: pasted newlines don't submit
    refresh(prompt, pw);
    Result res = Line;
    for (;;) {
        int c = getc(-1);
        if (c < 0) {
            res = buf_.empty() ? Eof : Line;
            break;
        }
        if (c != 9) listed_ = false;
        if (c == 13) {  // Enter; a trailing backslash continues on a new line
            if (!items_.empty()) {  // run the highlighted menu entry
                buf_ = items_[sel_].first;
                break;
            }
            if (pos_ == buf_.size() && !buf_.empty() && buf_.back() == '\\')
                buf_.back() = '\n';
            else
                break;
        } else if (c == 10)
            insert("\n");  // Ctrl-J
        else if (c == 3) {
            res = Cancel;
            break;
        } else if (c == 4) {
            if (buf_.empty()) {
                res = Eof;
                break;
            }
            buf_.erase(pos_, next_char(pos_) - pos_);
        } else if (c == 1)
            pos_ = 0;
        else if (c == 5)
            pos_ = buf_.size();
        else if (c == 2)
            pos_ = prev_char(pos_);
        else if (c == 6)
            pos_ = next_char(pos_);
        else if (c == 8 || c == 127) {
            size_t p = prev_char(pos_);
            buf_.erase(p, pos_ - p);
            pos_ = p;
        } else if (c == 11)
            buf_.erase(pos_);
        else if (c == 21)
            buf_.erase(0, pos_), pos_ = 0;
        else if (c == 23) {
            size_t p = prev_word(pos_);
            buf_.erase(p, pos_ - p);
            pos_ = p;
        } else if (c == 12)
            put("\x1b[H\x1b[2J");
        else if (c == 9 && !items_.empty()) {
            buf_ = items_[sel_].first + " ";
            pos_ = buf_.size();
        } else if (c == 9)
            complete(prompt, pw);
        else if (c == 16)
            hist_go(-1);
        else if (c == 14)
            hist_go(1);
        else if (c == 27) {
            int a = getc(50);
            if (a == '[' || a == 'O') {
                std::string params;
                int f;
                while ((f = getc(50)) >= 0 && !(f >= 0x40 && f <= 0x7E))
                    params += (char)f;
                bool mod = params.find(';') != std::string::npos;  // ctrl/alt + arrow
                if (f == 'A' && !items_.empty())
                    sel_ = (sel_ + items_.size() - 1) % items_.size();
                else if (f == 'B' && !items_.empty())
                    sel_ = (sel_ + 1) % items_.size();
                else if (f == 'A')
                    hist_go(-1);
                else if (f == 'B')
                    hist_go(1);
                else if (f == 'C')
                    pos_ = mod ? next_word(pos_) : next_char(pos_);
                else if (f == 'D')
                    pos_ = mod ? prev_word(pos_) : prev_char(pos_);
                else if (f == 'H')
                    pos_ = 0;
                else if (f == 'F')
                    pos_ = buf_.size();
                else if (f == '~' && (params == "1" || params == "7"))
                    pos_ = 0;
                else if (f == '~' && (params == "4" || params == "8"))
                    pos_ = buf_.size();
                else if (f == '~' && params == "3")
                    buf_.erase(pos_, next_char(pos_) - pos_);
                else if (f == '~' && params == "200") {
                    std::string paste;
                    int d;
                    while ((d = getc(500)) >= 0) {
                        paste += (char)d;
                        if (ends_with(paste, "\x1b[201~")) {
                            paste.resize(paste.size() - 6);
                            break;
                        }
                    }
                    std::string norm;
                    for (size_t i = 0; i < paste.size(); ++i) {
                        if (paste[i] == '\r') {
                            norm += '\n';
                            if (i + 1 < paste.size() && paste[i + 1] == '\n') ++i;
                        } else
                            norm += paste[i];
                    }
                    insert(norm);
                }
            } else if (a == 'b')
                pos_ = prev_word(pos_);
            else if (a == 'f')
                pos_ = next_word(pos_);
            else if (a == 'd')
                buf_.erase(pos_, next_word(pos_) - pos_);
            else if (a == 127 || a == 8) {
                size_t p = prev_word(pos_);
                buf_.erase(p, pos_ - p);
                pos_ = p;
            } else if (a == 13)
                insert("\n");  // Alt/Option-Enter
        } else if (c >= 32) {
            std::string s(1, (char)c);
            int more = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : 0;
            for (int i = 0; i < more; ++i) {
                int d = getc(100);
                if (d < 0) break;
                s += (char)d;
            }
            insert(s);
        }
        update_menu();
        refresh(prompt, pw);
    }
    pos_ = buf_.size();
    items_.clear();  // erases the menu rows
    refresh(prompt, pw);
    put("\x1b[?2004l\r\n");
    raw(false);
    out = buf_;
    return res;
}

void LineEditor::history_load(const std::string& path) {
    hist_path_ = path;
    std::string data, err;
    if (!read_file(path, data, err)) return;
    for (auto& l : split_lines(data)) {
        if (l.empty()) continue;
        for (auto& ch : l)
            if (ch == '\x1f') ch = '\n';
        hist_.push_back(l);
    }
    if (hist_.size() > 1000) {  // keep the file small
        hist_.erase(hist_.begin(), hist_.end() - 500);
        std::string all;
        for (auto h : hist_) {
            for (auto& ch : h)
                if (ch == '\n') ch = '\x1f';
            all += h + "\n";
        }
        write_file(path, all, err);
    }
}

void LineEditor::history_add(const std::string& line) {
    if (trim(line).empty() || (!hist_.empty() && hist_.back() == line)) return;
    hist_.push_back(line);
    if (hist_path_.empty()) return;
    std::string l = line;
    for (auto& ch : l)
        if (ch == '\n') ch = '\x1f';
    l += '\n';
    int fd = open(hist_path_.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0) return;
    ssize_t n = write(fd, l.data(), l.size());
    (void)n;
    close(fd);
}
