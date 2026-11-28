#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace gandon {

struct McpFunctionInfo {
    std::uint64_t start{};
    std::string name;
};

struct McpInstructionInfo {
    std::uint64_t address{};
    std::uint32_t size{};
    std::string mnemonic;
    std::string operands;
};

struct McpXrefInfo {
    std::uint64_t from{};
    std::uint64_t to{};
    std::string kind;
    std::string from_name;
    std::string to_name;
};

struct McpDecompileResult {
    bool has_binary{false};
    bool ok{false};
    std::string text; // pseudocode when ok, error message otherwise
};

struct McpHostCallbacks {
    std::function<void(const std::string&)> log;
    std::function<void(const std::string&)> status;

    std::function<std::optional<std::string>()> info_json;
    std::function<std::optional<std::uint64_t>(const std::string&)> resolve_address;
    std::function<std::uint64_t()> current_address;

    std::function<std::vector<McpFunctionInfo>()> list_functions;
    std::function<std::vector<McpInstructionInfo>(std::uint64_t address, std::size_t count)> disasm_at;
    std::function<std::optional<std::string>(std::uint64_t address)> listing_at;
    std::function<McpDecompileResult(std::uint64_t address, bool address_comments)> decompile_at;
    std::function<McpDecompileResult(std::uint64_t address, bool address_comments, const std::string& language)> decompile_at_lang;
    std::function<std::vector<McpXrefInfo>(std::uint64_t address, const std::string& direction)> xrefs_at;

    std::function<std::optional<std::string>(std::size_t min_length)> strings_text;
    std::function<std::optional<std::string>()> imports_text;
    std::function<std::optional<std::string>()> exports_text;
    std::function<std::optional<std::string>()> resources_text;
    std::function<std::optional<std::string>()> segments_text;
    std::function<std::optional<std::string>(std::uint64_t address, std::size_t rows)> hex_dump_at;

    std::function<std::optional<std::vector<std::uint64_t>>(const std::string& pattern)> search_bytes;
    std::function<std::optional<std::vector<std::uint64_t>>(const std::string& text)> search_text;

    std::function<std::optional<std::string>()> run_auto_analysis;
    std::function<bool(std::uint64_t address, const std::string& name)> rename;
    std::function<bool(std::uint64_t address, const std::string& comment)> add_comment;
    std::function<bool(std::uint64_t address, const std::vector<std::uint8_t>& bytes)> patch_bytes;
    std::function<bool(std::uint64_t address)> navigate;

    std::function<bool(const std::string& path, std::string& error)> save_database;
    std::function<bool(const std::string& path, std::string& error)> load_database;

    // Types & prototypes
    std::function<std::optional<std::string>()> list_types;
    std::function<bool(const std::string& c_source, std::string& error)> define_type;
    std::function<bool(std::uint64_t address, const std::string& prototype, std::string& error)> set_prototype;

    // Live Win32 debugger
    std::function<std::string()> dbg_status_json;
    std::function<std::optional<std::string>()> dbg_regs_json;
    std::function<bool(std::uint64_t address, bool enable, std::string& error)> dbg_toggle_bp;
    std::function<bool(bool step_over, std::string& error)> dbg_step;
    std::function<std::optional<std::string>(std::uint64_t address, std::size_t size)> dbg_read_mem;

    // Unicorn CPU emulator
    std::function<std::optional<std::string>(std::uint64_t address, std::size_t max_instructions)> emu_run_json;
    std::function<std::optional<std::string>()> emu_regs_json;

    // PDB & Patching
    std::function<bool(const std::string& pdb_path, std::size_t& loaded_count, std::string& error)> load_pdb;
    std::function<std::string()> list_patches_json;
    std::function<bool(std::uint64_t address, const std::string& asm_text, std::size_t& bytes_written, std::string& error)> assemble_patch;
};

struct McpServerConfig {
    std::string host{"127.0.0.1"};
    int port{8766};
    std::string token; // empty = no bearer auth required
};

class McpServer {
public:
    McpServer();
    ~McpServer();

    McpServer(const McpServer&) = delete;
    McpServer& operator=(const McpServer&) = delete;

    void set_callbacks(McpHostCallbacks callbacks);

    bool start(const McpServerConfig& config, std::string& error_out);
    void stop();

    bool is_running() const;
    McpServerConfig config() const;
    std::string endpoint_url() const;
    unsigned requests_served() const;

private:
    struct Impl;
    Impl* impl_;
};

} // namespace gandon
