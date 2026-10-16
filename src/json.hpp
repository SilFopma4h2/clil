// Minimal JSON value: just enough for the OpenAI / LM Studio wire format.
#pragma once
#include <cstddef>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

class Json {
public:
    enum Type : unsigned char { Null, Bool, Num, Str, Arr, Obj };
    using Member = std::pair<std::string, Json>;

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool v) : t_(Bool), b_(v) {}
    template <class T,
              typename std::enable_if<std::is_arithmetic<T>::value && !std::is_same<T, bool>::value, int>::type = 0>
    Json(T v) : t_(Num), n_((double)v) {}
    Json(const char* v) : t_(Str), s_(v) {}
    Json(std::string v) : t_(Str), s_(std::move(v)) {}

    static Json array() {
        Json j;
        j.t_ = Arr;
        return j;
    }
    static Json object() {
        Json j;
        j.t_ = Obj;
        return j;
    }
    static bool parse(const std::string& text, Json& out, std::string* err = nullptr);

    Type type() const { return t_; }
    bool is_null() const { return t_ == Null; }
    bool is_str() const { return t_ == Str; }
    bool is_num() const { return t_ == Num; }
    bool is_bool() const { return t_ == Bool; }
    bool is_arr() const { return t_ == Arr; }
    bool is_obj() const { return t_ == Obj; }

    // Lookups never fail: a missing key or index yields a shared null value.
    const Json& operator[](const char* key) const;
    const Json& operator[](const std::string& key) const { return (*this)[key.c_str()]; }
    const Json& operator[](size_t i) const;
    const Json& operator[](int i) const { return (*this)[(size_t)i]; }
    bool has(const char* key) const;
    size_t size() const { return t_ == Arr ? a_.size() : t_ == Obj ? o_.size() : 0; }

    const std::string& str() const;  // empty unless this is a string
    std::string str(const char* def) const { return t_ == Str ? s_ : def; }
    double num(double def = 0) const { return t_ == Num ? n_ : def; }
    long long integer(long long def = 0) const { return t_ == Num ? (long long)n_ : def; }
    bool boolean(bool def = false) const { return t_ == Bool ? b_ : def; }

    Json& set(const std::string& key, Json v);  // insert or replace; returns *this
    Json& push(Json v);                         // append to array; returns *this
    Json* find(const char* key);
    bool erase(const char* key);
    std::vector<Json>& items() { return a_; }
    const std::vector<Json>& items() const { return a_; }
    std::vector<Member>& members() { return o_; }
    const std::vector<Member>& members() const { return o_; }
    std::string& mut_str() { return s_; }

    std::string dump() const;
    void dump_to(std::string& out) const;

private:
    Type t_ = Null;
    bool b_ = false;
    double n_ = 0;
    std::string s_;
    std::vector<Json> a_;
    std::vector<Member> o_;
};

// Append s to out as a quoted JSON string (invalid UTF-8 becomes U+FFFD).
void json_quote(std::string& out, const std::string& s);
