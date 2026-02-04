# Virtual Device Feature Comparison

This table compares capabilities and restrictions across different VirtualDev types in the `store/` module.

## Legend
- ✅ **Yes** - Feature is fully supported
- ❌ **No** - Feature is not supported
- ⚠️ **Limited** - Feature is supported with restrictions (see notes)
- 🚧 **Planned** - Feature is planned for future implementation

---

## VDev Type Comparison

| Feature/Capability              | SimpleLogStreamVdev       | IndexedLogStreamVdev    | FixedBlkStreamVdev       | DynamicBlkVdev          |
|---------------------------------|---------------------------|-------------------------|--------------------------|-------------------------|
| **Write Operations**            |                           |                         |                          |                         |
| Random writes                   | ❌                        | ❌                      | ⚠️ (block-aligned)       | ✅                      |
| Append-only writes              | ✅                        | ✅                      | ✅                       | ✅                      |
| Arbitrary size writes           | ✅                        | ✅                      | ❌ (block-aligned)       | ❌ (block-aligned)      |
| Deferred persistence            | ✅                        | ✅                      | ✅                       | ❌ (immediate)          |
| Multi-session support           | ✅                        | ✅                      | ✅                       | ✅                      |
| Auto-expansion                  | ✅                        | ✅                      | ✅                       | ✅                      |
| **Read Operations**             |                           |                         |                          |                         |
| Runtime reads                   | ❌                        | ✅                      | ✅                       | ✅                      |
| Recovery-only reads             | ✅                        | ✅                      | ❌                       | ❌                      |
| Read caching                    | ❌                        | TBD                     | ❌                       | ✅                      |
| Random access reads             | ❌                        | ✅ (indexed)            | ✅                       | ✅                      |
| **Data Management**             |                           |                         |                          |                         |
| Block allocation                | ❌                        | ❌                      | ✅                       | ✅                      |
| Block invalidation              | ❌                        | ❌                      | ✅                       | ✅                      |
| Stream truncation               | ✅                        | ✅                      | ❌                       | ❌                      |
| Rollback support                | ❌                        | ❌                      | ⚠️ (checkpoint)          | ⚠️ (checkpoint)         |
| Fast destroy                    | ✅                        | ✅                      | ✅                       | ✅                      |
| **Metadata**                    |                           |                         |                          |                         |
| Metadata size                   | Minimal (tail_offset)     | TBD                     | Per-chunk allocator      | Per-chunk allocator     |
| Metadata persistence            | Per flush                 | TBD                     | Per checkpoint           | Per write               |
| **Architecture**                |                           |                         |                          |                         |
| Lock-free appends               | ✅                        | TBD                     | ❌ (mutex)               | ❌ (mutex)              |
| ChunkPool integration           | ✅                        | TBD                     | ✅                       | ✅                      |
| Block alignment                 | Internal                  | TBD                     | External                 | External                |
| **Session Management**          |                           |                         |                          |                         |
| Checkpoint sessions             | Caller-managed            | TBD                     | Internal                 | Internal                |
| Session validation              | ❌                        | TBD                     | ✅                       | ✅                      |
| **Use Cases**                   |                           |                         |                          |                         |
| Primary use case                | WAL, journals             | Event logs with search  | Block storage            | General-purpose storage |
| Performance profile             | High-write, no-read       | Balanced read/write     | Medium write, cached read| Balanced, cached        |

---

## Detailed Notes

### SimpleLogStreamVdev
- Optimized for pure append workloads with recovery-time reads
- No runtime read support - data only accessible during recovery
- Minimal metadata overhead (only tail_offset)
- Lock-free concurrent appends for maximum throughput
- Best for: WAL, transaction logs, audit logs, journals

### IndexedLogStreamVdev
*To be documented when implemented*

### FixedBlkStreamVdev  
*To be documented when implemented*

### DynamicBlkVdev
*To be documented when implemented*

---

## Selection Guide

Choose your VDev type based on:

1. **SimpleLogStreamVdev** if:
   - You need maximum write throughput
   - Reads only happen during recovery
   - Data is small, arbitrary-sized appends
   - Minimal metadata is critical

2. **IndexedLogStreamVdev** if:
   - TBD

3. **FixedBlkStreamVdev** if:
   - TBD

4. **DynamicBlkVdev** if:
   - TBD

---

*Last updated: 2026-01-28*
