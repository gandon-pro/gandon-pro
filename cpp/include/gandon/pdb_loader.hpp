#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace gandon {

struct PdbSymbol {
    std::uint64_t address{0};
    std::uint64_t size{0};
    std::string name;
    bool is_function{false};
};

// Loads symbols from a Windows PDB file associated with `binary_path` (or an explicit `pdb_override`).
// When `pdb_override` is empty, checks `<binary_dir>/<binary_stem>.pdb` and lets DbgHelp resolve CodeView info.
bool load_pdb_symbols(const std::filesystem::path& binary_path,
                      std::uint64_t image_base,
                      const std::filesystem::path& pdb_override,
                      std::vector<PdbSymbol>& out_symbols,
                      std::string& error);

} // namespace gandon
