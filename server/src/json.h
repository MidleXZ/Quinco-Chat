// Minimal, dependency-free JSON value type + parser + serializer.
// Quinco Chat - C++ backend
//
// Covers the full JSON grammar the client protocol needs: null, bool,
// number, string, array, object. Strings are UTF-8 byte sequences; \uXXXX
// escapes (including surrogate pairs) are decoded on parse and re-encoded
// on write.
//
// Design goals: no exceptions escaping into socket code, no external
// dependencies, single-pass O(n) parsing, and accessors that never crash
// on a malformed client message.

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace quinco {
namespace json {

class Value;
using Array = std::vector<Value>;
// std::map keeps object keys in a stable, sorted order, which makes both
// round-tripping and diffing deterministic.
using Object = std::map<std::string, Value>;

enum class Type { Null, Bool, Number, String, Array, Object };

class Value {
 public:
  Value() : type_(Type::Null) {}
  Value(std::nullptr_t) : type_(Type::Null) {}
  Value(bool b) : type_(Type::Bool), bool_(b) {}
  Value(int n) : type_(Type::Number), num_(static_cast<double>(n)), isInt_(true), int_(n) {}
  Value(int64_t n) : type_(Type::Number), num_(static_cast<double>(n)), isInt_(true), int_(n) {}
  Value(uint64_t n)
      : type_(Type::Number),
        num_(static_cast<double>(n)),
        isInt_(true),
        int_(static_cast<int64_t>(n)) {}
  Value(double d) : type_(Type::Number), num_(d), isInt_(false) {}
  Value(const char* s) : type_(Type::String), str_(s ? s : "") {}
  Value(const std::string& s) : type_(Type::String), str_(s) {}
  Value(std::string&& s) : type_(Type::String), str_(std::move(s)) {}
  Value(const Array& a) : type_(Type::Array), arr_(a) {}
  Value(Array&& a) : type_(Type::Array), arr_(std::move(a)) {}
  Value(const Object& o) : type_(Type::Object), obj_(o) {}
  Value(Object&& o) : type_(Type::Object), obj_(std::move(o)) {}

  static Value array() { return Value(Array{}); }
  static Value object() { return Value(Object{}); }

  Type type() const { return type_; }
  bool isNull() const { return type_ == Type::Null; }
  bool isBool() const { return type_ == Type::Bool; }
  bool isNumber() const { return type_ == Type::Number; }
  bool isString() const { return type_ == Type::String; }
  bool isArray() const { return type_ == Type::Array; }
  bool isObject() const { return type_ == Type::Object; }

  // Safe accessors. A wrong-type access returns the supplied default, so a
  // malformed client message can never crash a handler.
  bool asBool(bool fallback = false) const {
    return type_ == Type::Bool ? bool_ : fallback;
  }
  double asNumber(double fallback = 0.0) const {
    return type_ == Type::Number ? num_ : fallback;
  }
  int64_t asInt(int64_t fallback = 0) const {
    if (type_ != Type::Number) return fallback;
    if (isInt_) return int_;
    return static_cast<int64_t>(num_);
  }
  const std::string& asString() const {
    static const std::string kEmpty;
    return type_ == Type::String ? str_ : kEmpty;
  }
  std::string asString(const std::string& fallback) const {
    return type_ == Type::String ? str_ : fallback;
  }
  const Array& asArray() const {
    static const Array kEmpty;
    return type_ == Type::Array ? arr_ : kEmpty;
  }
  const Object& asObject() const {
    static const Object kEmpty;
    return type_ == Type::Object ? obj_ : kEmpty;
  }

  Array& arrayItems() { return arr_; }
  Object& objectItems() { return obj_; }
  const Array& arrayItems() const { return arr_; }
  const Object& objectItems() const { return obj_; }

  // Object helpers -------------------------------------------------------
  bool has(const std::string& key) const {
    return type_ == Type::Object && obj_.find(key) != obj_.end();
  }
  const Value& operator[](const std::string& key) const {
    static const Value kNull;
    if (type_ != Type::Object) return kNull;
    auto it = obj_.find(key);
    return it == obj_.end() ? kNull : it->second;
  }
  void set(const std::string& key, Value v) {
    if (type_ != Type::Object) {
      type_ = Type::Object;
      obj_.clear();
    }
    obj_[key] = std::move(v);
  }
  void erase(const std::string& key) {
    if (type_ == Type::Object) obj_.erase(key);
  }

  // Array helpers --------------------------------------------------------
  void push(Value v) {
    if (type_ != Type::Array) {
      type_ = Type::Array;
      arr_.clear();
    }
    arr_.push_back(std::move(v));
  }
  size_t size() const {
    switch (type_) {
      case Type::Array: return arr_.size();
      case Type::Object: return obj_.size();
      case Type::String: return str_.size();
      default: return 0;
    }
  }
  bool empty() const { return size() == 0; }

  std::string dump() const;
  std::string dump(int indent) const;

  // Parse `text`. On success returns true and fills `out`; on failure
  // returns false and puts a short description in `error`.
  static bool parse(const std::string& text, Value& out, std::string& error);

 private:
  void dumpTo(std::string& out, int indent, int depth) const;

  Type type_;
  bool bool_ = false;
  double num_ = 0.0;
  bool isInt_ = false;
  int64_t int_ = 0;
  std::string str_;
  Array arr_;
  Object obj_;
};

inline bool parse(const std::string& text, Value& out) {
  std::string ignored;
  return Value::parse(text, out, ignored);
}

// Escapes a UTF-8 string for inclusion in a JSON document (with quotes).
std::string escapeString(const std::string& in);

}  // namespace json
}  // namespace quinco
