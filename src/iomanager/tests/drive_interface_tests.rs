//! Basic drive interface tests migrated from legacy implementation.
//! These cover simple round-trip write/read and vectored write/read using
//! IOBuffer.

use std::{fs::OpenOptions, io::Write, path::PathBuf};

use iomgr::{DriveInterface, IOBuffer};

#[cfg(feature = "tokio")]
fn temp_file_path(name: &str) -> PathBuf {
    let mut p = std::env::temp_dir();
    p.push(format!("iomanager_test_{}", name));
    p
}

#[cfg(feature = "tokio")]
fn create_temp_file(path: &PathBuf, size: usize) {
    // Pre-create file with given size so positional I/O succeeds beyond EOF.
    let mut f = OpenOptions::new().create(true).write(true).open(path).expect("create temp file");
    f.set_len(size as u64).expect("set len");
    f.flush().ok();
}

#[cfg(feature = "tokio")]
#[tokio::test]
async fn test_iobuffer_write_read_roundtrip() {
    let path = temp_file_path("roundtrip");
    create_temp_file(&path, 4096);

    let di = DriveInterface::new();
    let iodev = DriveInterface::open_dev(path.to_str().unwrap().to_string(), 0).await.expect("open_dev");

    // Prepare write buffer.
    let mut wbuf = IOBuffer::new(4096);
    for (i, b) in wbuf.as_mut_slice().iter_mut().enumerate() {
        *b = (i % 251) as u8;
    }
    let written = di.write(&iodev, &wbuf, 0).await.expect("write");
    assert_eq!(written, wbuf.len());

    // Read back into fresh buffer.
    let rbuf = IOBuffer::new(4096);
    let (res, rbuf) = di.read(&iodev, rbuf, 0).await;
    let read = res.expect("read");
    assert_eq!(read, wbuf.len());
    assert_eq!(rbuf.as_slice(), wbuf.as_slice());
}

#[cfg(feature = "tokio")]
#[tokio::test]
async fn test_iobuffer_writev_readv() {
    let path = temp_file_path("writev_readv");
    // total size: 3 * 1024
    create_temp_file(&path, 3 * 1024);

    let di = DriveInterface::new();
    let iodev = DriveInterface::open_dev(path.to_str().unwrap().to_string(), 0).await.expect("open_dev");

    // Prepare vectored buffers.
    let bufs: Vec<IOBuffer> = (0..3)
        .map(|i| {
            let mut b = IOBuffer::new(1024);
            for (idx, v) in b.as_mut_slice().iter_mut().enumerate() {
                *v = (i * 7 + idx as i32) as u8;
            }
            b
        })
        .collect();

    let total_expected: usize = bufs.iter().map(|b| b.len()).sum();
    // writev takes ownership; create a owned copy via manual duplication
    let bufs_for_write: Vec<IOBuffer> = bufs
        .iter()
        .map(|orig| {
            let mut b = IOBuffer::new(orig.len());
            b.as_mut_slice().copy_from_slice(orig.as_slice());
            b
        })
        .collect();
    let written = di.writev(&iodev, bufs_for_write, 0).await.expect("writev");
    assert_eq!(written, total_expected);

    // Readv
    let read_vecs: Vec<IOBuffer> = (0..3).map(|_| IOBuffer::new(1024)).collect();
    let (res, read_vecs) = di.readv(&iodev, read_vecs, 0).await;
    let read_bytes = res.expect("readv");
    assert_eq!(read_bytes, total_expected);

    for (orig, read) in bufs.iter().zip(read_vecs.iter()) {
        assert_eq!(orig.as_slice(), read.as_slice());
    }
}
