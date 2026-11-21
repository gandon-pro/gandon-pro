#include "gandon/mcp_server.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

namespace gandon {
namespace {

constexpr const char* kServerName = "gandon-pro";
constexpr const char* kServerVersion = "2.0.0";
constexpr const char* kDefaultProtocolVersion = "2025-03-26";

struct JsonValue {
    enum class Type { Null, Bool, Number, String, Array, Object } type{Type::Null};
    bool boolean{false};
    double number{0};
    std::string str;
    std::vector<JsonValue> array;
    std::vector<std::pair<std::string, JsonValue>> object;

    bool is_string() const { return type == Type::String; }
    bool is_object() const { return type == Type::Object; }
    bool is_array() const { return type == Type::Array; }
    bool is_null() const { return type == Type::Null; }

    const JsonValue* find(const std::string& key) const {
        for (const auto& [k, v] : object) {
            if (k == key) return &v;
        }
        return nullptr;
    }
    std::string as_string(const std::string& fallback = {}) const {
        return type == Type::String ? str : fallback;
    }
    long long as_int(long long fallback = 0) const {
        return type == Type::Number ? static_cast<long long>(number) : fallback;
    }
    bool as_bool(bool fallback = false) const {
        return type == Type::Bool ? boolean : fallback;
    }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& text) : text_(text) {}

    bool parse(JsonValue& out) {
        skip_ws();
        if (!parse_value(out)) return false;
        skip_ws();
        return true;
    }

private:
    const std::string& text_;
    std::size_t pos_{0};

    char peek() const { return pos_ < text_.size() ? text_[pos_] : '\0'; }
    void skip_ws() {
        while (pos_ < text_.size() &&
               (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\r' || text_[pos_] == '\n')) {
            ++pos_;
        }
    }
    bool consume(char expected) {
        skip_ws();
        if (peek() != expected) return false;
        ++pos_;
        return true;
    }
    bool consume_literal(const char* literal) {
        const auto len = std::strlen(literal);
        if (text_.compare(pos_, len, literal) != 0) return false;
        pos_ += len;
        return true;
    }

    bool parse_value(JsonValue& out) {
        skip_ws();
        switch (peek()) {
        case '{': return parse_object(out);
        case '[': return parse_array(out);
        case '"': return parse_string(out);
        case 't': return consume_literal("true") && (out.type = JsonValue::Type::Bool, out.boolean = true, true);
        case 'f': return consume_literal("false") && (out.type = JsonValue::Type::Bool, out.boolean = false, true);
        case 'n': return consume_literal("null") && (out.type = JsonValue::Type::Null, true);
        default: return parse_number(out);
        }
    }

    bool parse_object(JsonValue& out) {
        if (!consume('{')) return false;
        out.type = JsonValue::Type::Object;
        skip_ws();
        if (peek() == '}') { ++pos_; return true; }
        while (true) {
            skip_ws();
            JsonValue key;
            if (!parse_string(key)) return false;
            if (!consume(':')) return false;
            JsonValue val;
            if (!parse_value(val)) return false;
            out.object.emplace_back(std::move(key.str), std::move(val));
            skip_ws();
            if (peek() == ',') { ++pos_; continue; }
            if (peek() == '}') { ++pos_; break; }
            return false;
        }
        return true;
    }

    bool parse_array(JsonValue& out) {
        if (!consume('[')) return false;
        out.type = JsonValue::Type::Array;
        skip_ws();
        if (peek() == ']') { ++pos_; return true; }
        while (true) {
            JsonValue val;
            if (!parse_value(val)) return false;
            out.array.push_back(std::move(val));
            skip_ws();
            if (peek() == ',') { ++pos_; continue; }
            if (peek() == ']') { ++pos_; break; }
            return false;
        }
        return true;
    }

    bool parse_string(JsonValue& out) {
        if (!consume('"')) return false;
        std::string value;
        while (true) {
            if (pos_ >= text_.size()) return false;
            const char ch = text_[pos_++];
            if (ch == '"') break;
            if (ch == '\\') {
                if (pos_ >= text_.size()) return false;
                const char esc = text_[pos_++];
                switch (esc) {
                case '"': value += '"'; break;
                case '\\': value += '\\'; break;
                case '/': value += '/'; break;
                case 'n': value += '\n'; break;
                case 't': value += '\t'; break;
                case 'r': value += '\r'; break;
                case 'b': value += '\b'; break;
                case 'f': value += '\f'; break;
                case 'u': {
                    if (pos_ + 4 > text_.size()) return false;
                    const auto code = static_cast<unsigned>(std::strtoul(text_.substr(pos_, 4).c_str(), nullptr, 16));
                    pos_ += 4;
                    if (code < 0x80) value += static_cast<char>(code);
                    else if (code < 0x800) {
                        value += static_cast<char>(0xC0 | (code >> 6));
                        value += static_cast<char>(0x80 | (code & 0x3F));
                    } else {
                        value += static_cast<char>(0xE0 | (code >> 12));
                        value += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                        value += static_cast<char>(0x80 | (code & 0x3F));
                    }
                    break;
                }
                default: return false;
                }
            } else {
                value += ch;
            }
        }
        out.type = JsonValue::Type::String;
        out.str = std::move(value);
        return true;
    }

    bool parse_number(JsonValue& out) {
        const auto start = pos_;
        if (peek() == '-') ++pos_;
        while (std::isdigit(static_cast<unsigned char>(peek()))) ++pos_;
        if (peek() == '.') {
            ++pos_;
            while (std::isdigit(static_cast<unsigned char>(peek()))) ++pos_;
        }
        if (peek() == 'e' || peek() == 'E') {
            ++pos_;
            if (peek() == '+' || peek() == '-') ++pos_;
            while (std::isdigit(static_cast<unsigned char>(peek()))) ++pos_;
        }
        if (pos_ == start) return false;
        out.type = JsonValue::Type::Number;
        out.number = std::strtod(text_.substr(start, pos_ - start).c_str(), nullptr);
        return true;
    }
};

std::string json_escape(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (unsigned char ch : value) {
        switch (ch) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (ch < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", ch);
                out += buf;
            } else {
                out += static_cast<char>(ch);
            }
        }
    }
    return out;
}

std::string json_number_literal(double value) {
    if (value == static_cast<long long>(value)) return std::to_string(static_cast<long long>(value));
    std::ostringstream out;
    out << value;
    return out.str();
}

std::string id_literal(const JsonValue* id) {
    if (!id || id->is_null()) return "null";
    if (id->type == JsonValue::Type::String) return "\"" + json_escape(id->str) + "\"";
    if (id->type == JsonValue::Type::Number) return json_number_literal(id->number);
    return "null";
}

std::string hex_u64(std::uint64_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << value;
    return out.str();
}

std::string generate_session_id() {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<std::uint64_t> dist;
    char buf[40]{};
    std::snprintf(buf, sizeof(buf), "%016llx%016llx",
                  static_cast<unsigned long long>(dist(gen)),
                  static_cast<unsigned long long>(dist(gen)));
    return buf;
}

struct ToolError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

bool check_bearer_auth(const std::string& header, const std::string& expected_token) {
    if (expected_token.empty()) return true;
    const std::string prefix = "bearer ";
    if (header.size() < prefix.size()) return false;
    if (!iequals(header.substr(0, prefix.size()), prefix)) return false;
    std::string supplied = header.substr(prefix.size());
    while (!supplied.empty() && (supplied.front() == ' ' || supplied.front() == '\t')) supplied.erase(0, 1);
    while (!supplied.empty() && (supplied.back() == ' ' || supplied.back() == '\t')) supplied.pop_back();
    return supplied == expected_token;
}

bool decode_chunked_body(const std::string& raw, std::string& out) {
    out.clear();
    std::size_t pos = 0;
    while (pos < raw.size()) {
        const auto crlf = raw.find("\r\n", pos);
        if (crlf == std::string::npos) return false;
        std::string size_line = raw.substr(pos, crlf - pos);
        if (const auto semi = size_line.find(';'); semi != std::string::npos) size_line.resize(semi);
        std::size_t chunk_size = 0;
        try {
            chunk_size = static_cast<std::size_t>(std::stoull(size_line, nullptr, 16));
        } catch (...) {
            return false;
        }
        pos = crlf + 2;
        if (chunk_size == 0) return true;
        if (pos + chunk_size + 2 > raw.size()) return false;
        out.append(raw, pos, chunk_size);
        pos += chunk_size + 2;
    }
    return false;
}

} // namespace

struct McpServer::Impl {
    McpHostCallbacks cb;
    McpServerConfig cfg;
    std::atomic<bool> running{false};
    std::atomic<unsigned> requests_served{0};
    std::thread server_thread;
    SOCKET listen_socket{INVALID_SOCKET};
    std::mutex socket_mutex;
    std::string session_id;

    std::mutex analysis_mutex;
    std::atomic<bool> analysis_running{false};
    bool analysis_has_result{false};
    std::string analysis_summary;
    std::thread analysis_thread;

    void log(const std::string& msg) const {
        if (cb.log) cb.log(msg);
    }
    void status(const std::string& msg) const {
        if (cb.status) cb.status(msg);
    }

    std::uint64_t resolve_or_throw(const std::string& text) const {
        if (text.empty()) throw ToolError("'address' is required");
        if (!cb.resolve_address) throw ToolError("host does not support resolve_address");
        const auto resolved = cb.resolve_address(text);
        if (!resolved) throw ToolError("could not resolve address/name: " + text);
        return *resolved;
    }

    static std::string require_opt(const std::optional<std::string>& opt, const char* what) {
        if (!opt) throw ToolError(std::string("no binary is open, or ") + what + " could not be computed");
        return *opt;
    }

    std::string tool_info(const JsonValue&) const {
        if (!cb.info_json) throw ToolError("info callback unavailable");
        return require_opt(cb.info_json(), "info");
    }

    std::string tool_resolve(const JsonValue& args) const {
        const auto text = args.find("address") ? args.find("address")->as_string() : std::string();
        const auto address = resolve_or_throw(text);
        return "{\"address\":\"" + hex_u64(address) + "\"}";
    }

    std::string tool_functions(const JsonValue& args) const {
        if (!cb.list_functions) throw ToolError("function list unavailable");
        const auto offset = args.find("offset") ? static_cast<std::size_t>((std::max)(0LL, args.find("offset")->as_int(0))) : 0;
        const auto limit = args.find("limit") ? static_cast<std::size_t>((std::max)(1LL, args.find("limit")->as_int(200))) : 200;
        const auto all = cb.list_functions();
        std::ostringstream out;
        out << "{\"total\":" << all.size() << ",\"offset\":" << offset << ",\"functions\":[";
        bool first = true;
        std::size_t emitted = 0;
        for (std::size_t i = offset; i < all.size() && emitted < limit; ++i, ++emitted) {
            if (!first) out << ',';
            first = false;
            out << "{\"start\":\"" << hex_u64(all[i].start) << "\",\"name\":\"" << json_escape(all[i].name) << "\"}";
        }
        out << "],\"truncated\":" << ((offset + emitted) < all.size() ? "true" : "false") << "}";
        return out.str();
    }

    std::string tool_disasm(const JsonValue& args) const {
        if (!cb.disasm_at) throw ToolError("disasm unavailable");
        const auto text = args.find("address") ? args.find("address")->as_string() : std::string();
        const auto address = resolve_or_throw(text);
        const auto count = args.find("count") ? static_cast<std::size_t>((std::max)(1LL, args.find("count")->as_int(40))) : 40;
        const auto insns = cb.disasm_at(address, count);
        std::ostringstream out;
        out << "{\"address\":\"" << hex_u64(address) << "\",\"instructions\":[";
        for (std::size_t i = 0; i < insns.size(); ++i) {
            if (i) out << ',';
            std::string full = insns[i].mnemonic;
            if (!insns[i].operands.empty()) full += " " + insns[i].operands;
            out << "{\"address\":\"" << hex_u64(insns[i].address) << "\",\"size\":" << insns[i].size
                << ",\"text\":\"" << json_escape(full) << "\"}";
        }
        out << "]}";
        return out.str();
    }

    std::string tool_listing(const JsonValue& args) const {
        if (!cb.listing_at) throw ToolError("listing unavailable");
        const auto text = args.find("address") ? args.find("address")->as_string() : std::string();
        const auto address = resolve_or_throw(text);
        const auto listing = require_opt(cb.listing_at(address), "listing");
        return "{\"address\":\"" + hex_u64(address) + "\",\"listing\":\"" + json_escape(listing) + "\"}";
    }

    std::string tool_decompile(const JsonValue& args) const {
        if (!cb.decompile_at) throw ToolError("decompiler unavailable");
        const auto text = args.find("address") ? args.find("address")->as_string() : std::string();
        const auto address = resolve_or_throw(text);
        const bool comments = args.find("address_comments") && args.find("address_comments")->as_bool();
        const auto res = cb.decompile_at(address, comments);
        if (!res.has_binary) throw ToolError("no binary is open");
        std::ostringstream out;
        out << "{\"address\":\"" << hex_u64(address) << "\",\"decompiled\":" << (res.ok ? "true" : "false") << ",";
        out << (res.ok ? "\"pseudocode\":\"" : "\"error\":\"") << json_escape(res.text) << "\"}";
        return out.str();
    }

    std::string tool_decompile_to_file(const JsonValue& args) const {
        if (!cb.list_functions || !cb.decompile_at) throw ToolError("decompiler unavailable");
        const auto path = args.find("path") ? args.find("path")->as_string() : std::string();
        if (path.empty()) throw ToolError("'path' is required");
        const auto offset = args.find("offset") ? static_cast<std::size_t>((std::max)(0LL, args.find("offset")->as_int(0))) : 0;
        const auto req_limit = args.find("limit") ? args.find("limit")->as_int(0) : 0;
        const std::size_t limit = req_limit > 0 ? static_cast<std::size_t>(req_limit) : SIZE_MAX;
        const bool comments = args.find("address_comments") && args.find("address_comments")->as_bool();
        const auto all = cb.list_functions();
        std::ofstream file(path, std::ios::trunc);
        if (!file) throw ToolError("could not create file: " + path);
        std::size_t written = 0, failed = 0, emitted = 0;
        for (std::size_t i = offset; i < all.size() && emitted < limit; ++i, ++emitted) {
            const auto res = cb.decompile_at(all[i].start, comments);
            file << "// ---- " << hex_u64(all[i].start) << " " << all[i].name << " ----\n";
            if (res.ok) { file << res.text << "\n\n"; ++written; }
            else { file << "// (decompilation failed: " << res.text << ")\n\n"; ++failed; }
        }
        file.flush();
        if (!file) throw ToolError("error writing file: " + path);
        std::ostringstream out;
        out << "{\"path\":\"" << json_escape(path) << "\",\"functions_written\":" << written
            << ",\"functions_failed\":" << failed << "}";
        return out.str();
    }

    std::string tool_xrefs(const JsonValue& args) const {
        if (!cb.xrefs_at) throw ToolError("xrefs unavailable");
        const auto text = args.find("address") ? args.find("address")->as_string() : std::string();
        const auto address = resolve_or_throw(text);
        const auto direction = args.find("direction") ? args.find("direction")->as_string("to") : std::string("to");
        const auto refs = cb.xrefs_at(address, direction);
        std::ostringstream out;
        out << "{\"address\":\"" << hex_u64(address) << "\",\"direction\":\"" << json_escape(direction) << "\",\"xrefs\":[";
        for (std::size_t i = 0; i < refs.size(); ++i) {
            if (i) out << ',';
            out << "{\"from\":\"" << hex_u64(refs[i].from) << "\",\"to\":\"" << hex_u64(refs[i].to)
                << "\",\"kind\":\"" << json_escape(refs[i].kind) << "\"";
            if (!refs[i].from_name.empty()) out << ",\"from_name\":\"" << json_escape(refs[i].from_name) << "\"";
            if (!refs[i].to_name.empty()) out << ",\"to_name\":\"" << json_escape(refs[i].to_name) << "\"";
            out << "}";
        }
        out << "]}";
        return out.str();
    }

    std::string tool_strings(const JsonValue& args) const {
        if (!cb.strings_text) throw ToolError("strings unavailable");
        const auto min_len = args.find("min_length") ? static_cast<std::size_t>((std::max)(1LL, args.find("min_length")->as_int(5))) : 5;
        return "{\"text\":\"" + json_escape(require_opt(cb.strings_text(min_len), "strings")) + "\"}";
    }

    std::string tool_imports(const JsonValue&) const {
        return "{\"text\":\"" + json_escape(require_opt(cb.imports_text ? cb.imports_text() : std::nullopt, "imports")) + "\"}";
    }
    std::string tool_exports(const JsonValue&) const {
        return "{\"text\":\"" + json_escape(require_opt(cb.exports_text ? cb.exports_text() : std::nullopt, "exports")) + "\"}";
    }
    std::string tool_resources(const JsonValue&) const {
        return "{\"text\":\"" + json_escape(require_opt(cb.resources_text ? cb.resources_text() : std::nullopt, "resources")) + "\"}";
    }
    std::string tool_segments(const JsonValue&) const {
        return "{\"text\":\"" + json_escape(require_opt(cb.segments_text ? cb.segments_text() : std::nullopt, "segments")) + "\"}";
    }

    std::string tool_hex(const JsonValue& args) const {
        if (!cb.hex_dump_at) throw ToolError("hex dump unavailable");
        const auto text = args.find("address") ? args.find("address")->as_string() : std::string();
        const auto address = resolve_or_throw(text);
        const auto rows = args.find("rows") ? static_cast<std::size_t>((std::max)(1LL, args.find("rows")->as_int(16))) : 16;
        const auto dump = require_opt(cb.hex_dump_at(address, rows), "hex dump");
        return "{\"address\":\"" + hex_u64(address) + "\",\"hex\":\"" + json_escape(dump) + "\"}";
    }

    std::string tool_search_bytes(const JsonValue& args) const {
        if (!cb.search_bytes) throw ToolError("search_bytes unavailable");
        const auto pattern = args.find("pattern") ? args.find("pattern")->as_string() : std::string();
        if (pattern.empty()) throw ToolError("'pattern' is required, e.g. '55 8B EC ?? ??'");
        const auto matches = cb.search_bytes(pattern);
        if (!matches) throw ToolError("no binary is open or invalid pattern");
        std::ostringstream out;
        out << "{\"matches\":[";
        for (std::size_t i = 0; i < matches->size(); ++i) {
            if (i) out << ',';
            out << "\"" << hex_u64((*matches)[i]) << "\"";
        }
        out << "]}";
        return out.str();
    }

    std::string tool_search_text(const JsonValue& args) const {
        if (!cb.search_text) throw ToolError("search_text unavailable");
        const auto text = args.find("text") ? args.find("text")->as_string() : std::string();
        if (text.empty()) throw ToolError("'text' is required");
        const auto matches = cb.search_text(text);
        if (!matches) throw ToolError("no binary is open");
        std::ostringstream out;
        out << "{\"matches\":[";
        for (std::size_t i = 0; i < matches->size(); ++i) {
            if (i) out << ',';
            out << "\"" << hex_u64((*matches)[i]) << "\"";
        }
        out << "]}";
        return out.str();
    }

    std::string analysis_state_json() {
        std::lock_guard<std::mutex> lock(analysis_mutex);
        std::ostringstream out;
        out << "{\"running\":" << (analysis_running.load() ? "true" : "false")
            << ",\"has_result\":" << (analysis_has_result ? "true" : "false");
        if (analysis_has_result) out << ",\"summary\":\"" << json_escape(analysis_summary) << "\"";
        out << "}";
        return out.str();
    }

    std::string tool_auto_analysis(const JsonValue& args) {
        if (!cb.run_auto_analysis) throw ToolError("auto_analysis unavailable");
        const bool async = args.find("async") && args.find("async")->as_bool();
        if (!async) {
            const auto summary = require_opt(cb.run_auto_analysis(), "auto-analysis");
            return "{\"summary\":\"" + json_escape(summary) + "\",\"async\":false}";
        }
        {
            std::lock_guard<std::mutex> lock(analysis_mutex);
            if (analysis_running.load()) return "{\"started\":false,\"already_running\":true}";
            analysis_running = true;
            analysis_has_result = false;
            analysis_summary.clear();
        }
        if (analysis_thread.joinable()) analysis_thread.join();
        analysis_thread = std::thread([this] {
            const auto result = cb.run_auto_analysis();
            std::lock_guard<std::mutex> lock(analysis_mutex);
            analysis_summary = result.value_or("no binary open");
            analysis_has_result = true;
            analysis_running = false;
        });
        return "{\"started\":true,\"async\":true}";
    }

    std::string tool_analysis_wait(const JsonValue& args) {
        const auto timeout_ms = args.find("timeout_seconds") ? (std::max)(1LL, args.find("timeout_seconds")->as_int(30)) * 1000 : 30000;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (analysis_running.load() && running.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return analysis_state_json();
    }

    std::string tool_rename(const JsonValue& args) const {
        const auto text = args.find("address") ? args.find("address")->as_string() : std::string();
        const auto name = args.find("name") ? args.find("name")->as_string() : std::string();
        if (text.empty() || name.empty()) throw ToolError("'address' and 'name' are both required");
        const auto address = resolve_or_throw(text);
        if (!cb.rename || !cb.rename(address, name)) throw ToolError("rename failed");
        return "{\"address\":\"" + hex_u64(address) + "\",\"name\":\"" + json_escape(name) + "\"}";
    }

    std::string tool_comment(const JsonValue& args) const {
        const auto text = args.find("address") ? args.find("address")->as_string() : std::string();
        const auto comment = args.find("comment") ? args.find("comment")->as_string() : std::string();
        const auto address = resolve_or_throw(text);
        if (!cb.add_comment || !cb.add_comment(address, comment)) throw ToolError("add_comment failed");
        return "{\"address\":\"" + hex_u64(address) + "\",\"comment\":\"" + json_escape(comment) + "\"}";
    }

    std::string tool_patch(const JsonValue& args) const {
        const auto text = args.find("address") ? args.find("address")->as_string() : std::string();
        const auto bytes_text = args.find("bytes") ? args.find("bytes")->as_string() : std::string();
        if (text.empty() || bytes_text.empty()) throw ToolError("'address' and 'bytes' are both required");
        const auto address = resolve_or_throw(text);
        std::vector<std::uint8_t> bytes;
        std::istringstream tokens(bytes_text);
        std::string token;
        while (tokens >> token) {
            try {
                bytes.push_back(static_cast<std::uint8_t>(std::stoul(token, nullptr, 16)));
            } catch (...) {
                throw ToolError("bad byte token: " + token);
            }
        }
        if (bytes.empty()) throw ToolError("empty replacement bytes");
        if (!cb.patch_bytes || !cb.patch_bytes(address, bytes)) throw ToolError("patch failed");
        return "{\"address\":\"" + hex_u64(address) + "\",\"patched_bytes\":" + std::to_string(bytes.size()) +
               ",\"note\":\"applied to live open database (Ctrl+Z reverts)\"}";
    }

    std::string tool_navigate(const JsonValue& args) const {
        const auto text = args.find("address") ? args.find("address")->as_string() : std::string();
        const auto address = resolve_or_throw(text);
        if (!cb.navigate || !cb.navigate(address)) throw ToolError("navigate failed");
        return "{\"address\":\"" + hex_u64(address) + "\",\"navigated\":true}";
    }

    std::string tool_save_database(const JsonValue& args) const {
        const auto path = args.find("path") ? args.find("path")->as_string() : std::string();
        if (path.empty()) throw ToolError("'path' is required");
        std::string error;
        if (!cb.save_database || !cb.save_database(path, error)) {
            throw ToolError("save_database failed: " + (error.empty() ? std::string("unknown error") : error));
        }
        return "{\"path\":\"" + json_escape(path) + "\",\"saved\":true}";
    }

    std::string tool_load_database(const JsonValue& args) const {
        const auto path = args.find("path") ? args.find("path")->as_string() : std::string();
        if (path.empty()) throw ToolError("'path' is required");
        std::string error;
        if (!cb.load_database || !cb.load_database(path, error)) {
            throw ToolError("load_database failed: " + (error.empty() ? std::string("unknown error") : error));
        }
        return "{\"path\":\"" + json_escape(path) + "\",\"loaded\":true}";
    }

    std::string call_tool(const std::string& name, const JsonValue& args) {
        if (name == "gandon_info") return tool_info(args);
        if (name == "gandon_resolve_address") return tool_resolve(args);
        if (name == "gandon_list_functions") return tool_functions(args);
        if (name == "gandon_disasm") return tool_disasm(args);
        if (name == "gandon_listing") return tool_listing(args);
        if (name == "gandon_decompile") return tool_decompile(args);
        if (name == "gandon_decompile_to_file") return tool_decompile_to_file(args);
        if (name == "gandon_xrefs") return tool_xrefs(args);
        if (name == "gandon_strings") return tool_strings(args);
        if (name == "gandon_imports") return tool_imports(args);
        if (name == "gandon_exports") return tool_exports(args);
        if (name == "gandon_resources") return tool_resources(args);
        if (name == "gandon_segments") return tool_segments(args);
        if (name == "gandon_hex") return tool_hex(args);
        if (name == "gandon_search_bytes") return tool_search_bytes(args);
        if (name == "gandon_search_text") return tool_search_text(args);
        if (name == "gandon_auto_analysis") return tool_auto_analysis(args);
        if (name == "gandon_analysis_status") return analysis_state_json();
        if (name == "gandon_analysis_wait") return tool_analysis_wait(args);
        if (name == "gandon_rename") return tool_rename(args);
        if (name == "gandon_add_comment") return tool_comment(args);
        if (name == "gandon_patch_bytes") return tool_patch(args);
        if (name == "gandon_navigate") return tool_navigate(args);
        if (name == "gandon_save_database") return tool_save_database(args);
        if (name == "gandon_load_database") return tool_load_database(args);
        throw ToolError("Unknown tool: " + name);
    }

    static std::string tools_list_json() {
        static const char* kAddrSchema =
            "{\"type\":\"object\",\"properties\":{\"address\":{\"type\":\"string\",\"description\":\"Hex/decimal address, function/import/export name, or renamed label.\"}},\"required\":[\"address\"]}";
        struct Spec { const char* name; const char* desc; const char* schema; };
        static const Spec specs[] = {
            {"gandon_info", "Live binary info: format, architecture, entry point, sections, function/xref counts.", "{\"type\":\"object\",\"properties\":{}}"},
            {"gandon_resolve_address", "Resolve a name/label/hex/decimal text to an address.", kAddrSchema},
            {"gandon_list_functions", "List discovered functions (start, name), paginated with offset/limit.", "{\"type\":\"object\",\"properties\":{\"offset\":{\"type\":\"integer\"},\"limit\":{\"type\":\"integer\"}}}"},
            {"gandon_disasm", "Disassemble instructions starting at an address.", "{\"type\":\"object\",\"properties\":{\"address\":{\"type\":\"string\"},\"count\":{\"type\":\"integer\"}},\"required\":[\"address\"]}"},
            {"gandon_listing", "Full IDA-style text listing of the function containing an address.", kAddrSchema},
            {"gandon_decompile", "Decompile one function (x86/x64) to C-like pseudocode.", "{\"type\":\"object\",\"properties\":{\"address\":{\"type\":\"string\"},\"address_comments\":{\"type\":\"boolean\"}},\"required\":[\"address\"]}"},
            {"gandon_decompile_to_file", "Decompile a range of functions directly to a file on disk.", "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"offset\":{\"type\":\"integer\"},\"limit\":{\"type\":\"integer\"},\"address_comments\":{\"type\":\"boolean\"}},\"required\":[\"path\"]}"},
            {"gandon_xrefs", "Cross-references to, from, or both for an address.", "{\"type\":\"object\",\"properties\":{\"address\":{\"type\":\"string\"},\"direction\":{\"type\":\"string\",\"enum\":[\"to\",\"from\",\"both\"]}},\"required\":[\"address\"]}"},
            {"gandon_strings", "Strings embedded in the binary.", "{\"type\":\"object\",\"properties\":{\"min_length\":{\"type\":\"integer\"}}}"},
            {"gandon_imports", "Imported functions.", "{\"type\":\"object\",\"properties\":{}}"},
            {"gandon_exports", "Exported functions.", "{\"type\":\"object\",\"properties\":{}}"},
            {"gandon_resources", "PE resource directory as text.", "{\"type\":\"object\",\"properties\":{}}"},
            {"gandon_segments", "Memory segments/sections overview.", "{\"type\":\"object\",\"properties\":{}}"},
            {"gandon_hex", "Hex dump starting at an address.", "{\"type\":\"object\",\"properties\":{\"address\":{\"type\":\"string\"},\"rows\":{\"type\":\"integer\"}},\"required\":[\"address\"]}"},
            {"gandon_search_bytes", "Search the open binary for a byte pattern ('??' wildcard).", "{\"type\":\"object\",\"properties\":{\"pattern\":{\"type\":\"string\"}},\"required\":[\"pattern\"]}"},
            {"gandon_search_text", "Search the open binary for an ASCII substring.", "{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\"}},\"required\":[\"text\"]}"},
            {"gandon_auto_analysis", "Run recursive-descent auto-analysis (supports async:true).", "{\"type\":\"object\",\"properties\":{\"async\":{\"type\":\"boolean\"}}}"},
            {"gandon_analysis_status", "Non-blocking status check for async auto-analysis.", "{\"type\":\"object\",\"properties\":{}}"},
            {"gandon_analysis_wait", "Wait up to timeout_seconds for async auto-analysis.", "{\"type\":\"object\",\"properties\":{\"timeout_seconds\":{\"type\":\"integer\"}}}"},
            {"gandon_rename", "Rename a function or label in the live database.", "{\"type\":\"object\",\"properties\":{\"address\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"}},\"required\":[\"address\",\"name\"]}"},
            {"gandon_add_comment", "Set a comment at an address in the live database.", "{\"type\":\"object\",\"properties\":{\"address\":{\"type\":\"string\"},\"comment\":{\"type\":\"string\"}},\"required\":[\"address\"]}"},
            {"gandon_patch_bytes", "Patch bytes in the live session (space-separated hex, e.g. '90 90').", "{\"type\":\"object\",\"properties\":{\"address\":{\"type\":\"string\"},\"bytes\":{\"type\":\"string\"}},\"required\":[\"address\",\"bytes\"]}"},
            {"gandon_navigate", "Jump the GUI view to an address.", kAddrSchema},
            {"gandon_save_database", "Save the live session to a .gnd database file.", "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}"},
            {"gandon_load_database", "Load a .gnd database file into the live session.", "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}"}
        };
        std::ostringstream out;
        out << '[';
        bool first = true;
        for (const auto& s : specs) {
            if (!first) out << ',';
            first = false;
            out << "{\"name\":\"" << s.name << "\",\"description\":\"" << json_escape(s.desc) << "\",\"inputSchema\":" << s.schema << "}";
        }
        out << ']';
        return out.str();
    }

    std::string dispatch_one(const JsonValue& message) {
        if (!message.is_object()) {
            return "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32600,\"message\":\"Invalid Request\"}}";
        }
        const auto* method_val = message.find("method");
        const auto* id_val = message.find("id");
        const std::string method = method_val ? method_val->as_string() : std::string();
        const std::string id_text = id_literal(id_val);
        const bool is_notification = !id_val;

        if (method == "initialize") {
            std::string proto = kDefaultProtocolVersion;
            if (const auto* params = message.find("params")) {
                const auto req = params->find("protocolVersion") ? params->find("protocolVersion")->as_string() : std::string();
                if (req == "2024-11-05" || req == "2025-03-26" || req == "2025-06-18") proto = req;
            }
            return "{\"jsonrpc\":\"2.0\",\"id\":" + id_text + ",\"result\":{\"protocolVersion\":\"" + proto +
                   "\",\"capabilities\":{\"tools\":{\"listChanged\":false}},\"serverInfo\":{\"name\":\"" +
                   kServerName + "\",\"version\":\"" + kServerVersion + "\"}}}";
        }
        if (method == "notifications/initialized" || method == "notifications/cancelled" || method.rfind("notifications/", 0) == 0) {
            return {};
        }
        if (method == "ping") {
            return "{\"jsonrpc\":\"2.0\",\"id\":" + id_text + ",\"result\":{}}";
        }
        if (method == "tools/list") {
            return "{\"jsonrpc\":\"2.0\",\"id\":" + id_text + ",\"result\":{\"tools\":" + tools_list_json() + "}}";
        }
        if (method == "tools/call") {
            const auto* params = message.find("params");
            const auto name = params && params->find("name") ? params->find("name")->as_string() : std::string();
            const auto* arguments = params ? params->find("arguments") : nullptr;
            static const JsonValue empty_args{};
            const auto& args = arguments ? *arguments : empty_args;
            ++requests_served;
            try {
                const auto result = call_tool(name, args);
                return "{\"jsonrpc\":\"2.0\",\"id\":" + id_text + ",\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"" +
                       json_escape(result) + "\"}],\"isError\":false}}";
            } catch (const std::exception& exc) {
                return "{\"jsonrpc\":\"2.0\",\"id\":" + id_text + ",\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"" +
                       json_escape(exc.what()) + "\"}],\"isError\":true}}";
            }
        }
        if (is_notification) return {};
        return "{\"jsonrpc\":\"2.0\",\"id\":" + id_text + ",\"error\":{\"code\":-32601,\"message\":\"Method not found: " + json_escape(method) + "\"}}";
    }

    std::string dispatch(const std::string& body) {
        JsonValue parsed;
        if (!JsonParser(body).parse(parsed)) {
            return "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32700,\"message\":\"Parse error\"}}";
        }
        if (parsed.is_array()) {
            if (parsed.array.empty()) {
                return "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32600,\"message\":\"Empty batch\"}}";
            }
            std::ostringstream out;
            out << '[';
            bool any = false;
            for (const auto& item : parsed.array) {
                const auto resp = dispatch_one(item);
                if (!resp.empty()) {
                    if (any) out << ',';
                    any = true;
                    out << resp;
                }
            }
            out << ']';
            return any ? out.str() : std::string();
        }
        return dispatch_one(parsed);
    }

    static std::string read_line(SOCKET socket, std::string& residual) {
        for (;;) {
            const auto newline = residual.find("\r\n");
            if (newline != std::string::npos) {
                std::string line = residual.substr(0, newline);
                residual.erase(0, newline + 2);
                return line;
            }
            if (residual.size() > 65536) return {};
            char buffer[4096];
            const auto received = recv(socket, buffer, sizeof(buffer), 0);
            if (received <= 0) return {};
            residual.append(buffer, static_cast<std::size_t>(received));
        }
    }

    static bool send_all(SOCKET socket, const std::string& data) {
        std::size_t sent = 0;
        while (sent < data.size()) {
            const auto result = send(socket, data.data() + sent, static_cast<int>(data.size() - sent), 0);
            if (result <= 0) return false;
            sent += static_cast<std::size_t>(result);
        }
        return true;
    }

    void send_response(SOCKET socket, int status_code, const char* status_text, const std::string& content_type,
                       const std::string& body, bool include_session = false) const {
        std::ostringstream head;
        head << "HTTP/1.1 " << status_code << " " << status_text << "\r\n";
        if (!content_type.empty()) head << "Content-Type: " << content_type << "\r\n";
        head << "Content-Length: " << body.size() << "\r\n"
             << "Access-Control-Allow-Origin: *\r\n"
             << "Access-Control-Expose-Headers: Mcp-Session-Id\r\n";
        if (include_session && !session_id.empty()) {
            head << "Mcp-Session-Id: " << session_id << "\r\n";
        }
        head << "Connection: close\r\n\r\n";
        send_all(socket, head.str());
        if (!body.empty()) send_all(socket, body);
    }

    void handle_connection(SOCKET client_socket) {
        DWORD timeout_ms = 15000;
        setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
        setsockopt(client_socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));

        std::string residual;
        const auto request_line = read_line(client_socket, residual);
        if (request_line.empty()) { closesocket(client_socket); return; }

        std::istringstream req_stream(request_line);
        std::string method, path, http_version;
        req_stream >> method >> path >> http_version;

        std::size_t content_length = 0;
        bool has_content_length = false;
        bool is_chunked = false;
        std::string authorization;
        for (;;) {
            const auto header_line = read_line(client_socket, residual);
            if (header_line.empty()) break;
            const auto colon = header_line.find(':');
            if (colon == std::string::npos) continue;
            std::string key = header_line.substr(0, colon);
            std::string value = header_line.substr(colon + 1);
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(0, 1);
            while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.pop_back();
            for (auto& ch : key) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (key == "content-length") {
                content_length = static_cast<std::size_t>(std::strtoull(value.c_str(), nullptr, 10));
                has_content_length = true;
            } else if (key == "transfer-encoding") {
                std::string low = value;
                for (auto& ch : low) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                if (low.find("chunked") != std::string::npos) is_chunked = true;
            } else if (key == "authorization") {
                authorization = value;
            }
        }

        std::string body = std::move(residual);
        if (is_chunked) {
            std::string decoded;
            while (!decode_chunked_body(body, decoded)) {
                if (body.size() > 16 * 1024 * 1024) break;
                char buffer[8192];
                const auto received = recv(client_socket, buffer, sizeof(buffer), 0);
                if (received <= 0) break;
                body.append(buffer, static_cast<std::size_t>(received));
            }
            body = std::move(decoded);
        } else if (has_content_length) {
            constexpr std::size_t kMaxBody = 16 * 1024 * 1024;
            const std::size_t target = (std::min)(content_length, kMaxBody);
            while (body.size() < target) {
                char buffer[8192];
                const auto to_read = static_cast<int>((std::min)(sizeof(buffer), target - body.size()));
                const auto received = recv(client_socket, buffer, to_read, 0);
                if (received <= 0) break;
                body.append(buffer, static_cast<std::size_t>(received));
            }
            if (body.size() > target) body.resize(target);
        }

        const bool is_mcp_path = (path == "/mcp" || path.rfind("/mcp?", 0) == 0 || path == "/" || path.rfind("/?", 0) == 0);
        if (method == "OPTIONS") {
            std::ostringstream head;
            head << "HTTP/1.1 204 No Content\r\n"
                 << "Access-Control-Allow-Origin: *\r\n"
                 << "Access-Control-Allow-Methods: GET, POST, DELETE, OPTIONS\r\n"
                 << "Access-Control-Allow-Headers: Content-Type, Authorization, Mcp-Session-Id, MCP-Protocol-Version, Accept, Last-Event-ID\r\n"
                 << "Access-Control-Expose-Headers: Mcp-Session-Id\r\n"
                 << "Content-Length: 0\r\nConnection: close\r\n\r\n";
            send_all(client_socket, head.str());
        } else if (method == "GET" && (path == "/health" || path.rfind("/health?", 0) == 0)) {
            std::ostringstream body_out;
            body_out << "{\"status\":\"ok\",\"server\":\"" << kServerName << "\",\"version\":\"" << kServerVersion
                     << "\",\"protocol\":\"" << kDefaultProtocolVersion << "\",\"requests_served\":" << requests_served.load() << "}";
            send_response(client_socket, 200, "OK", "application/json", body_out.str());
        } else if (method == "GET" && is_mcp_path) {
            send_response(client_socket, 405, "Method Not Allowed", "application/json",
                          "{\"error\":\"GET SSE stream not offered; use POST /mcp\"}");
        } else if (method == "DELETE" && is_mcp_path) {
            if (!check_bearer_auth(authorization, cfg.token)) {
                send_response(client_socket, 401, "Unauthorized", "application/json", "{\"error\":\"missing or invalid bearer token\"}");
            } else {
                send_response(client_socket, 200, "OK", "application/json", "{\"closed\":true}");
            }
        } else if (method == "POST" && is_mcp_path) {
            if (!check_bearer_auth(authorization, cfg.token)) {
                send_response(client_socket, 401, "Unauthorized", "application/json", "{\"error\":\"missing or invalid bearer token\"}");
            } else {
                const auto response = dispatch(body);
                if (response.empty()) send_response(client_socket, 202, "Accepted", "", {}, true);
                else send_response(client_socket, 200, "OK", "application/json", response, true);
            }
        } else {
            send_response(client_socket, 404, "Not Found", "application/json",
                          "{\"error\":\"unknown endpoint; POST JSON-RPC to /mcp, GET /health\"}");
        }

        shutdown(client_socket, SD_SEND);
        closesocket(client_socket);
    }

    std::string endpoint_url() const {
        return "http://" + cfg.host + ":" + std::to_string(cfg.port) + "/mcp";
    }

    void server_loop() {
        log("[mcp] Listening on " + endpoint_url() + (cfg.token.empty() ? " (no token)" : " (bearer token required)"));
        status("MCP server listening on " + endpoint_url());

        while (running.load()) {
            SOCKET s = INVALID_SOCKET;
            {
                std::lock_guard<std::mutex> lock(socket_mutex);
                s = listen_socket;
            }
            if (s == INVALID_SOCKET) break;
            sockaddr_in client{};
            int client_len = sizeof(client);
            const SOCKET client_socket = accept(s, reinterpret_cast<sockaddr*>(&client), &client_len);
            if (client_socket == INVALID_SOCKET) {
                if (!running.load()) break;
                continue;
            }
            handle_connection(client_socket);
        }

        {
            std::lock_guard<std::mutex> lock(socket_mutex);
            if (listen_socket != INVALID_SOCKET) {
                closesocket(listen_socket);
                listen_socket = INVALID_SOCKET;
            }
        }
        WSACleanup();
    }
};

McpServer::McpServer() : impl_(new Impl{}) {}

McpServer::~McpServer() {
    stop();
    delete impl_;
    impl_ = nullptr;
}

void McpServer::set_callbacks(McpHostCallbacks callbacks) {
    impl_->cb = std::move(callbacks);
}

bool McpServer::start(const McpServerConfig& config, std::string& error_out) {
    stop();
    impl_->cfg = config;
    if (impl_->cfg.host.empty()) impl_->cfg.host = "127.0.0.1";
    if (impl_->cfg.port <= 0 || impl_->cfg.port > 65535) impl_->cfg.port = 8766;
    impl_->session_id = generate_session_id();

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        error_out = "WSAStartup failed";
        return false;
    }

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        error_out = "socket() failed: " + std::to_string(WSAGetLastError());
        WSACleanup();
        return false;
    }

    BOOL reuse = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(impl_->cfg.port));
    if (InetPtonA(AF_INET, impl_->cfg.host.c_str(), &addr.sin_addr) != 1) {
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    }

    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR || listen(s, 16) == SOCKET_ERROR) {
        error_out = "Could not bind " + impl_->cfg.host + ":" + std::to_string(impl_->cfg.port) +
                    " (WSA error " + std::to_string(WSAGetLastError()) + ")";
        closesocket(s);
        WSACleanup();
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(impl_->socket_mutex);
        impl_->listen_socket = s;
    }
    impl_->running = true;
    impl_->server_thread = std::thread([this] { impl_->server_loop(); });
    return true;
}

void McpServer::stop() {
    impl_->running = false;
    {
        std::lock_guard<std::mutex> lock(impl_->socket_mutex);
        if (impl_->listen_socket != INVALID_SOCKET) {
            closesocket(impl_->listen_socket);
            impl_->listen_socket = INVALID_SOCKET;
        }
    }
    if (impl_->server_thread.joinable()) impl_->server_thread.join();
    if (impl_->analysis_thread.joinable()) impl_->analysis_thread.join();
}

bool McpServer::is_running() const { return impl_->running.load(); }
McpServerConfig McpServer::config() const { return impl_->cfg; }
std::string McpServer::endpoint_url() const {
    return impl_->endpoint_url();
}
unsigned McpServer::requests_served() const { return impl_->requests_served.load(); }

} // namespace gandon
