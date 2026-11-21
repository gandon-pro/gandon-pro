#include "gandon/pdb_loader.hpp"

#include <algorithm>
#include <unordered_set>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#endif

namespace gandon {

#if defined(_WIN32)
namespace {

std::string utf8_from_wide_str(const wchar_t* text) {
    if (!text || !*text) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) return {};
    std::string out(static_cast<std::size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), needed, nullptr, nullptr);
    return out;
}

struct EnumContext {
    std::vector<PdbSymbol>* out{nullptr};
    std::unordered_set<std::uint64_t> seen;
};

BOOL CALLBACK enum_symbol_cb(PSYMBOL_INFOW info, ULONG symbol_size, PVOID user_context) {
    auto* ctx = static_cast<EnumContext*>(user_context);
    if (!ctx || !ctx->out || !info || !info->Address || !info->Name[0]) return TRUE;
    // SymTagFunction = 5, SymTagPublicSymbol = 10, SymTagData = 7
    const ULONG tag = info->Tag;
    if (tag != 5 && tag != 10 && tag != 7) return TRUE;
    if (!ctx->seen.insert(info->Address).second) return TRUE;
    PdbSymbol sym;
    sym.address = static_cast<std::uint64_t>(info->Address);
    sym.size = static_cast<std::uint64_t>(symbol_size ? symbol_size : info->Size);
    sym.name = utf8_from_wide_str(info->Name);
    sym.is_function = (tag == 5 || tag == 10);
    if (!sym.name.empty()) ctx->out->push_back(std::move(sym));
    return TRUE;
}

} // namespace

bool load_pdb_symbols(const std::filesystem::path& binary_path,
                      std::uint64_t image_base,
                      const std::filesystem::path& pdb_override,
                      std::vector<PdbSymbol>& out_symbols,
                      std::string& error) {
    out_symbols.clear();
    error.clear();

    std::filesystem::path target_pdb = pdb_override;
    if (target_pdb.empty() && !binary_path.empty()) {
        std::filesystem::path adjacent = binary_path;
        adjacent.replace_extension(L".pdb");
        std::error_code ec;
        if (std::filesystem::exists(adjacent, ec)) target_pdb = adjacent;
    }

    std::wstring search_path;
    if (!target_pdb.empty()) search_path = target_pdb.parent_path().wstring();
    else if (!binary_path.empty()) search_path = binary_path.parent_path().wstring();

    // Unique pseudo-handle per call so we never collide with process-wide symbol state.
    static std::uint64_t next_handle = 0x50444200ull;
    HANDLE sym_handle = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(++next_handle));

    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAVOR_COMPRESSED | SYMOPT_NO_PROMPTS);
    if (!SymInitializeW(sym_handle, search_path.empty() ? nullptr : search_path.c_str(), FALSE)) {
        error = "SymInitializeW failed (Win32 error " + std::to_string(GetLastError()) + ")";
        return false;
    }

    const std::wstring load_target = !target_pdb.empty() ? target_pdb.wstring() : binary_path.wstring();
    DWORD file_size = 0;
    {
        std::error_code ec;
        const auto sz = std::filesystem::file_size(load_target, ec);
        if (!ec && sz > 0 && sz <= 0xFFFFFFFFull) file_size = static_cast<DWORD>(sz);
    }

    const DWORD64 base = image_base ? image_base : 0x140000000ull;
    const DWORD64 loaded = SymLoadModuleExW(sym_handle, nullptr, load_target.c_str(), nullptr,
                                            base, file_size ? file_size : 0x1000000u, nullptr, 0);
    if (!loaded) {
        error = "SymLoadModuleExW failed for " + utf8_from_wide_str(load_target.c_str()) +
                " (Win32 error " + std::to_string(GetLastError()) + ")";
        SymCleanup(sym_handle);
        return false;
    }

    EnumContext ctx;
    ctx.out = &out_symbols;
    if (!SymEnumSymbolsW(sym_handle, loaded, L"*", enum_symbol_cb, &ctx)) {
        error = "SymEnumSymbolsW failed (Win32 error " + std::to_string(GetLastError()) + ")";
        SymUnloadModule64(sym_handle, loaded);
        SymCleanup(sym_handle);
        return false;
    }

    SymUnloadModule64(sym_handle, loaded);
    SymCleanup(sym_handle);

    std::sort(out_symbols.begin(), out_symbols.end(),
              [](const PdbSymbol& a, const PdbSymbol& b) { return a.address < b.address; });
    return true;
}

#else

bool load_pdb_symbols(const std::filesystem::path&,
                      std::uint64_t,
                      const std::filesystem::path&,
                      std::vector<PdbSymbol>& out_symbols,
                      std::string& error) {
    out_symbols.clear();
    error = "PDB symbol loading via DbgHelp is only supported on Windows.";
    return false;
}

#endif

} // namespace gandon
