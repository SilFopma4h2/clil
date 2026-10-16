#include "json.hpp"
#include "util.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static const Json kNull;
static const std::string kEmpty;

const Json& Json::operator[](const char* key) const {
    if (t_ == Obj)
        for (auto& m : o_)
            if (m.first == key) return m.second;
    return kNull;
}

const Json& Json::operator[](size_t i) const {
    return t_ == Arr && i < a_.size() ? a_[i] : kNull;
}

bool Json::has(const char* key) const {
    if (t_ == Obj)
        for (auto& m : o_)
            if (m.first == key) return true;
    return false;
}

const std::string& Json::str() const {
    return t_ == Str ? s_ : kEmpty;
}

Json& Json::set(const std::string& key, Json v) {
    if (t_ != Obj) *this = object();
    for (auto& m : o_)
        if (m.first == key) {
            m.second = std::move(v);
            return *this;
        }
    o_.emplace_back(key, std::move(v));
    return *this;
}

Json& Json::push(Json v) {
    if (t_ != Arr) *this = array();
    a_.push_back(std::move(v));
    return *this;
}

Json* Json::find(const char* key) {
    if (t_ == Obj)
        for (auto& m : o_)
            if (m.first == key) return &m.second;
    return nullptr;
}

bool Json::erase(const char* key) {
    for (size_t i = 0; t_ == Obj && i < o_.size(); ++i)
        if (o_[i].first == key) {
            o_.erase(o_.begin() + (long)i);
            return true;
        }
    return false;
}

// ---------------------------------------------------------------- parsing

namespace {
struct Parser {
    const char* p;
    const char* end;
    std::string err;
    int depth = 0;

    bool fail(const char* m) {
        if (err.empty()) err = m;
        return false;
    }
    void ws() {
        while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t'))
            ++p;
    }
    bool lit(const char* w) {
        size_t n = strlen(w);
        if ((size_t)(end - p) < n || memcmp(p, w, n) != 0) return fail("invalid literal");
        p += n;
        return true;
    }
    bool hex4(unsigned& v) {
        if (end - p < 4) return false;
        v = 0;
        for (int i = 0; i < 4; ++i) {
            char c = *p++;
            v <<= 4;
            if (c >= '0' && c <= '9')
                v |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f')
                v |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                v |= (unsigned)(c - 'A' + 10);
            else
                return false;
        }
        return true;
    }
    bool string(std::string& out) {
        ++p;  // opening quote
        while (p < end) {
            const char* run = p;
            while (p < end && *p != '"' && *p != '\\')
                ++p;
            out.append(run, (size_t)(p - run));
            if (p >= end) break;
            if (*p++ == '"') return true;
            if (p >= end) break;
            char e = *p++;
            switch (e) {
                case '"':
                    out += '"';
                    break;
                case '\\':
                    out += '\\';
                    break;
                case '/':
                    out += '/';
                    break;
                case 'b':
                    out += '\b';
                    break;
                case 'f':
                    out += '\f';
                    break;
                case 'n':
                    out += '\n';
                    break;
                case 'r':
                    out += '\r';
                    break;
                case 't':
                    out += '\t';
                    break;
                case 'u': {
                    unsigned cp;
                    if (!hex4(cp)) return fail("bad \\u escape");
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        unsigned lo = 0;
                        if (end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                            p += 2;
                            if (!hex4(lo)) return fail("bad \\u escape");
                            if (lo >= 0xDC00 && lo <= 0xDFFF)
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            else {
                                utf8_append(out, 0xFFFD);
                                cp = (lo >= 0xD800 && lo <= 0xDFFF) ? 0xFFFD : lo;
                            }
                        } else
                            cp = 0xFFFD;
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF)
                        cp = 0xFFFD;
                    utf8_append(out, cp);
                    break;
                }
                default:
                    return fail("bad escape");
            }
        }
        return fail("unterminated string");
    }
    bool number(Json& out) {
        const char* s = p;
        if (p < end && *p == '-') ++p;
        while (p < end && ((*p >= '0' && *p <= '9') || *p == '.' || *p == 'e' || *p == 'E' || *p == '+' || *p == '-'))
            ++p;
        std::string tmp(s, (size_t)(p - s));
        char* e = nullptr;
        double v = strtod(tmp.c_str(), &e);
        if (tmp.empty() || *e) return fail("bad number");
        out = Json(v);
        return true;
    }
    bool value(Json& out) {
        ws();
        if (p >= end) return fail("unexpected end of input");
        if (++depth > 256) return fail("nesting too deep");
        bool ok;
        switch (*p) {
            case '{':
                ok = object(out);
                break;
            case '[':
                ok = array(out);
                break;
            case '"':
                out = Json(std::string());
                ok = string(out.mut_str());
                break;
            case 't':
                ok = lit("true");
                if (ok) out = Json(true);
                break;
            case 'f':
                ok = lit("false");
                if (ok) out = Json(false);
                break;
            case 'n':
                ok = lit("null");
                if (ok) out = Json();
                break;
            default:
                ok = number(out);
        }
        --depth;
        return ok;
    }
    bool array(Json& out) {
        ++p;
        out = Json::array();
        ws();
        if (p < end && *p == ']') return ++p, true;
        for (;;) {
            Json v;
            if (!value(v)) return false;
            out.items().push_back(std::move(v));
            ws();
            if (p < end && *p == ',') {
                ++p;
                continue;
            }
            if (p < end && *p == ']') return ++p, true;
            return fail("expected ',' or ']'");
        }
    }
    bool object(Json& out) {
        ++p;
        out = Json::object();
        ws();
        if (p < end && *p == '}') return ++p, true;
        for (;;) {
            ws();
            if (p >= end || *p != '"') return fail("expected object key");
            std::string k;
            if (!string(k)) return false;
            ws();
            if (p >= end || *p != ':') return fail("expected ':'");
            ++p;
            Json v;
            if (!value(v)) return false;
            out.members().emplace_back(std::move(k), std::move(v));
            ws();
            if (p < end && *p == ',') {
                ++p;
                continue;
            }
            if (p < end && *p == '}') return ++p, true;
            return fail("expected ',' or '}'");
        }
    }
};
}  // namespace

bool Json::parse(const std::string& text, Json& out, std::string* err) {
    Parser ps{text.data(), text.data() + text.size(), {}, 0};
    Json v;
    bool ok = ps.value(v);
    if (ok) {
        ps.ws();
        if (ps.p != ps.end) ok = ps.fail("trailing characters");
    }
    if (!ok) {
        if (err) *err = ps.err + fmt(" at offset %zu", (size_t)(ps.p - text.data()));
        return false;
    }
    out = std::move(v);
    return true;
}

// ---------------------------------------------------------------- output

void json_quote(std::string& out, const std::string& s) {
    out += '"';
    const unsigned char* p = (const unsigned char*)s.data();
    size_t n = s.size();
    for (size_t i = 0; i < n;) {
        unsigned char c = p[i];
        if (c >= 0x80) {
            size_t l = utf8_len(p + i, n - i);
            if (l)
                out.append((const char*)p + i, l), i += l;
            else
                out += "\xEF\xBF\xBD", ++i;
            continue;
        }
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            default:
                if (c < 0x20 || c == 0x7f)
                    out += fmt("\\u%04x", c);
                else
                    out += (char)c;
        }
        ++i;
    }
    out += '"';
}

void Json::dump_to(std::string& out) const {
    switch (t_) {
        case Null:
            out += "null";
            break;
        case Bool:
            out += b_ ? "true" : "false";
            break;
        case Num:
            if (!std::isfinite(n_))
                out += "null";
            else if (std::fabs(n_) < 1e15 && n_ == std::floor(n_))
                out += fmt("%lld", (long long)n_);
            else
                out += fmt("%.15g", n_);
            break;
        case Str:
            json_quote(out, s_);
            break;
        case Arr:
            out += '[';
            for (size_t i = 0; i < a_.size(); ++i) {
                if (i) out += ',';
                a_[i].dump_to(out);
            }
            out += ']';
            break;
        case Obj:
            out += '{';
            for (size_t i = 0; i < o_.size(); ++i) {
                if (i) out += ',';
                json_quote(out, o_[i].first);
                out += ':';
                o_[i].second.dump_to(out);
            }
            out += '}';
            break;
    }
}

std::string Json::dump() const {
    std::string s;
    dump_to(s);
    return s;
}
