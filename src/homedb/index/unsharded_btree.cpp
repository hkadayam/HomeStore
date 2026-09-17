// UnshardedBtree is a header-only template.  This TU exists so the CMake target has a source file per module and
// so the DbKey/DbValue instantiation is explicit (Phase 1 only ever creates UnshardedBtree<DbKey, DbValue>).

#include "homedb/index/unsharded_btree.h"

#include "homedb/index/db_kv.h"

namespace homedb {

// Explicit instantiation for the only K/V pair Phase 1 uses.  MVCC (Phase 2) will add
// UnshardedBtree<MvccKey<DbKey>, MvccValue<DbValue>>.
template class UnshardedBtree< DbKey, DbValue >;

} // namespace homedb
