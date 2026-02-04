# PhysicalDev Tests

This document describes the comprehensive test suite for `PhysicalDev` device I/O operations.

## Platform Requirements

**IMPORTANT**: These tests require **Linux** with io_uring support, as they use the `tokio-uring` crate which is Linux-specific. The tests will not compile or run on macOS or Windows.

## Running the Tests

On a Linux system with io_uring support:

```bash
cd /Users/hkadayam/src/Homestore/src/homestore
cargo test --lib device::physical_dev::tests -- --nocapture
```

To run individual tests:

```bash
# Test 1: Direct I/O write variations
cargo test --lib test_direct_io_write_variations -- --nocapture

# Test 2: Direct I/O read variations
cargo test --lib test_direct_io_read_variations -- --nocapture

# Test 3: Buffered I/O with fsync
cargo test --lib test_buffered_io_with_fsync -- --nocapture

# Test 4: Write, close, reopen, and read
cargo test --lib test_write_close_reopen_read -- --nocapture

# Test 5: Combined operations
cargo test --lib test_all_io_operations_combined -- --nocapture

# Test 6: Alignment verification
cargo test --lib test_alignment_requirements -- --nocapture
```

## Test Coverage

### Test 1: Direct I/O Write Variations (`test_direct_io_write_variations`)

Tests all write operations with Direct I/O (`O_DIRECT` flag):

- **Single write**: Tests `write()` method with a single page
- **Vectored write**: Tests `writev()` method with multiple buffers
- **Write zeros**: Tests `write_zero()` method for efficient zero-writing

**Validates**:
- Direct I/O write operations succeed
- Proper error handling
- Page-aligned operations

### Test 2: Direct I/O Read Variations (`test_direct_io_read_variations`)

Tests all read operations with Direct I/O:

- **Single read**: Tests `read()` method with a single page
- **Vectored read**: Tests `readv()` method with multiple buffers
- **Data verification**: Validates that read data matches what was written

**Validates**:
- Direct I/O read operations succeed
- Zero-copy buffer handling (buffers passed by ownership)
- Data integrity (read matches write)
- Vectored I/O returns correct data in each buffer

### Test 3: Buffered I/O with Fsync (`test_buffered_io_with_fsync`)

Tests buffered I/O mode (without `O_DIRECT`):

- **Buffered writes**: Multiple write operations in buffered mode
- **Fsync operation**: Tests `fsync()` to flush data to disk
- **Verification**: Reads back data to ensure sync worked

**Validates**:
- Buffered I/O operations work correctly
- `fsync()` successfully flushes data
- Data persists after sync operation

### Test 4: Write, Close, Reopen, and Read (`test_write_close_reopen_read`)

Tests data persistence across device close/reopen cycles:

- **Phase 1**: Write multiple patterns to device
- **Close**: Properly close the device
- **Phase 2**: Reopen device and verify all data persists
- **Vectored verification**: Use `readv()` to validate multiple patterns at once

**Validates**:
- Data persists after device close
- Device caching works correctly
- Multiple patterns can be distinguished
- Both single and vectored reads return correct data

### Test 5: Combined Operations (`test_all_io_operations_combined`)

Comprehensive test combining all I/O operations:

1. Single write
2. Vectored write
3. Write zeros
4. Single read with verification
5. Vectored read with verification
6. Fsync operation

**Validates**:
- All operations work together correctly
- No interference between different operation types
- Zero-write produces actual zeros
- Sequential operations maintain data integrity

### Test 6: Alignment Requirements (`test_alignment_requirements`)

Tests alignment properties and requirements:

**Validates**:
- `optimal_page_size()` returns valid value
- `align_size()` returns valid value
- Page size >= align size
- Data start offset is page-aligned
- Data start < data end
- All offset calculations are correct

## Test Design

### Test File Management

All tests create temporary files in `/tmp/` with unique names:
- Auto-cleanup after each test
- File size: 10-20 MB (sufficient for multiple page operations)
- Files are properly closed before deletion

### Page Alignment

Tests respect device alignment requirements:
- All I/O operations use `optimal_page_size()` for buffer sizing
- Direct I/O requires proper alignment (4KB typical)
- Test data patterns are easily identifiable (0xAA, 0xBB, 0xCC, etc.)

### Error Handling

Tests use `assert!()` with descriptive messages:
- Clear failure messages indicate which operation failed
- Results are checked with `.unwrap()` or explicit assertions
- Each test includes success confirmation messages

## Test Patterns

The tests use distinctive byte patterns for easy verification:

- `0x11`, `0x22`, `0x33` - Sequential patterns
- `0x42`, `0x43` - Read test patterns
- `0x55`, `0x66` - Buffered I/O patterns
- `0xAA`, `0xBB`, `0xCC` - Persistence test patterns
- `0xAB`, `0xCD`, `0xEF`, `0x12` - Direct I/O write patterns

## Example Output

```
✓ Direct I/O write variations test passed
✓ Direct I/O read variations test passed
✓ Buffered I/O with fsync test passed
✓ Data written and device closed
✓ Data validated after reopen
✓ Write-close-reopen-read test passed
✓ Combined I/O operations test passed
✓ Alignment verification test passed
```

## Troubleshooting

### Test Failures on Linux

1. **Permission Issues**: Ensure you have write access to `/tmp/`
2. **io_uring Support**: Kernel must support io_uring (Linux 5.1+)
3. **Direct I/O**: Some filesystems don't support O_DIRECT on regular files
   - Consider using a block device for testing
   - Or use filesystems that support Direct I/O (ext4, xfs)

### Test Won't Compile

- Ensure you're on Linux
- Check kernel version: `uname -r` (need 5.1+)
- Verify tokio-uring dependency in Cargo.toml

## Future Enhancements

Potential additional tests:

1. **Error handling**: Test invalid offsets, sizes
2. **Concurrent I/O**: Multiple simultaneous operations
3. **Large I/O**: Multi-megabyte operations
4. **Chunk operations**: Test chunk creation/loading with I/O
5. **Performance**: Latency and throughput measurements
6. **Edge cases**: Unaligned access, device boundary conditions
