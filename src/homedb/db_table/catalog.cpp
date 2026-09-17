#include "homedb/db_table/catalog.h"

#include <cstring>

namespace homedb {

void CatalogEntry::set_name(std::string const& s) {
    std::memset(name, 0, sizeof(name));
    auto const n = std::min(s.size(), kNameMax - 1);
    std::memcpy(name, s.data(), n);
}

std::string CatalogEntry::get_name() const { return std::string{name}; }

} // namespace homedb
