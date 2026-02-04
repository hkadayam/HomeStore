use std::{io, sync::Arc};

use glommio::io::{DmaBuffer, DmaFile};

/// IOBuffer backed by glommio's DmaBuffer (aligned for O_DIRECT).
pub struct IOBuffer(pub glommio::io::DmaBuffer);

impl IOBuffer {
    pub fn new(size: usize) -> Self {
        // Round up size to alignment boundary (4096) to maintain alignment guarantees
        const ALIGNMENT: usize = 4096;
        let aligned_size = ((size + ALIGNMENT - 1) / ALIGNMENT) * ALIGNMENT;
        Self(glommio::io::DmaFile::alloc_dma_buffer(aligned_size))
    }
    pub fn len(&self) -> usize { self.0.len() }
    pub fn as_slice(&self) -> &[u8] { self.0.as_ref() }
    pub fn as_mut_slice(&mut self) -> &mut [u8] { self.0.as_mut() }
    /// Alignment of glommio DmaBuffer (return page size default 4096)
    pub fn alignment(&self) -> usize { 4096 }
    /// Resize the buffer. Glommio's DmaBuffer is fixed-size; allocate new and
    /// copy. Rounds up to alignment boundary to maintain alignment guarantees.
    pub fn resize(&mut self, new_len: usize) {
        // Round up to alignment boundary
        const ALIGNMENT: usize = 4096;
        let aligned_size = ((new_len + ALIGNMENT - 1) / ALIGNMENT) * ALIGNMENT;
        
        if aligned_size == self.len() {
            return;
        }
        let mut new_buf = glommio::io::DmaFile::alloc_dma_buffer(aligned_size);
        let to_copy = std::cmp::min(self.len(), aligned_size);
        new_buf.as_mut()[..to_copy].copy_from_slice(&self.0.as_ref()[..to_copy]);
        if aligned_size > to_copy {
            for b in &mut new_buf.as_mut()[to_copy..] {
                *b = 0;
            }
        }
        self.0 = new_buf;
    }
}

impl std::ops::Deref for IOBuffer {
    type Target = [u8];
    fn deref(&self) -> &Self::Target { self.as_slice() }
}

impl std::ops::DerefMut for IOBuffer {
    fn deref_mut(&mut self) -> &mut Self::Target { self.as_mut_slice() }
}

impl std::fmt::Debug for IOBuffer {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("IOBuffer").field("len", &self.len()).field("alignment", &self.alignment()).finish()
    }
}

/// Direct I/O device backed by glommio's DmaFile (io_uring + O_DIRECT).
pub struct IoDevice {
    file: Arc<DmaFile>,
    dev_id: u32,
    dev_name: String,
    is_block_device: bool,
}

impl IoDevice {
    pub fn dev_id(&self) -> u32 { self.dev_id }
    pub fn dev_name(&self) -> &str { &self.dev_name }
    pub fn file(&self) -> &DmaFile { &self.file }
    pub fn is_block_device(&self) -> bool { self.is_block_device }
}

pub struct DriveInterface {}

impl DriveInterface {
    pub fn new() -> Self { Self {} }
}

impl Default for DriveInterface {
    fn default() -> Self { Self::new() }
}

impl DriveInterface {
    /// Open a path returning an IoDevice. Caller should include O_DIRECT in
    /// oflags if required.
    pub async fn open_dev(devname: String, oflags: i32) -> io::Result<Arc<IoDevice>> {
        use std::{fs::OpenOptions, os::unix::fs::{OpenOptionsExt, FileTypeExt}};
        let std_file = OpenOptions::new().read(true).write(true).custom_flags(oflags).open(&devname)?;
        
        // Detect if this is a block device
        let is_block_device = std_file.metadata()?.file_type().is_block_device();
        
        let file = DmaFile::from(std_file).await?;
        let dev_id = devname.as_bytes().iter().map(|&b| b as u32).sum();
        Ok(Arc::new(IoDevice { file: Arc::new(file), dev_id, dev_name: devname, is_block_device }))
    }

    pub async fn get_size(iodev: &IoDevice) -> io::Result<u64> {
        let meta = iodev.file().metadata().await?;
        Ok(meta.len())
    }

    pub fn close_dev(&self, _iodev: Arc<IoDevice>) {
        /* drop closes */
    }

    /// Read into provided aligned buffer; returns number of bytes read and the
    /// buffer back.
    pub async fn read(&self, iodev: &IoDevice, mut buf: IOBuffer, offset: u64) -> (io::Result<usize>, IOBuffer) {
        let res = iodev.file.read_at(offset, &mut buf.0).await;
        (res, buf)
    }

    /// Write from the provided aligned buffer; returns number of bytes written.
    pub async fn write(&self, iodev: &IoDevice, buf: &IOBuffer, offset: u64) -> io::Result<usize> {
        iodev.file.write_at(offset, &buf.0).await
    }

    pub async fn readv(
        &self, iodev: &IoDevice, mut bufs: Vec<IOBuffer>, mut offset: u64,
    ) -> (io::Result<usize>, Vec<IOBuffer>) {
        let mut total = 0usize;
        for b in bufs.iter_mut() {
            match iodev.file.read_at(offset, &mut b.0).await {
                Ok(n) => {
                    total += n;
                    offset += n as u64;
                    if n != b.len() {
                        break;
                    }
                }
                Err(e) => return (Err(e), bufs),
            }
        }
        (Ok(total), bufs)
    }

    pub async fn writev(&self, iodev: &IoDevice, bufs: Vec<IOBuffer>, mut offset: u64) -> io::Result<usize> {
        let mut total = 0usize;
        for b in bufs.into_iter() {
            let n = iodev.file.write_at(offset, &b.0).await?;
            total += n;
            offset += n as u64;
            if n != b.len() {
                break;
            }
        }
        Ok(total)
    }

    pub async fn write_zero(&self, iodev: &IoDevice, size: u64, offset: u64) -> io::Result<usize> {
        // On Linux with block devices, try to use BLKZEROOUT ioctl for efficiency
        #[cfg(target_os = "linux")]
        if iodev.is_block_device {
            return self.write_zero_ioctl(iodev, size, offset).await;
        }
        
        // Fallback: write actual zeros
        self.write_zero_buffer(iodev, size, offset).await
    }

    #[cfg(target_os = "linux")]
    async fn write_zero_ioctl(&self, iodev: &IoDevice, size: u64, offset: u64) -> io::Result<usize> {
        use std::os::unix::io::AsRawFd;
        
        const BLKZEROOUT: u64 = 0x127f; // ioctl number for BLKZEROOUT
        const MAX_WRITE_ZERO_CHUNK: u64 = 1024 * 1024 * 1024; // 1GB chunks
        
        let fd = iodev.file.as_raw_fd();
        let mut remaining = size;
        let mut current_offset = offset;
        let original_size = size;
        
        while remaining > 0 {
            let chunk_size = remaining.min(MAX_WRITE_ZERO_CHUNK);
            let range = [current_offset, chunk_size];
            
            // Perform ioctl using glommio's blocking thread pool
            let fd_copy = fd;
            let result = glommio::executor().spawn_blocking(move || {
                unsafe {
                    let ret = libc::ioctl(fd_copy, BLKZEROOUT as libc::c_ulong, &range as *const [u64; 2]);
                    if ret != 0 {
                        Err(io::Error::last_os_error())
                    } else {
                        Ok(())
                    }
                }
            }).await;
            
            match result {
                Ok(Ok(())) => {
                    current_offset += chunk_size;
                    remaining -= chunk_size;
                }
                Ok(Err(e)) => {
                    eprintln!("BLKZEROOUT ioctl failed at offset={}, falling back to buffer write: {}", current_offset, e);
                    // Fall back to writing zeros
                    return self.write_zero_buffer(iodev, size, offset).await;
                }
                Err(e) => {
                    return Err(io::Error::new(io::ErrorKind::Other, format!("Task join error: {}", e)));
                }
            }
        }
        
        Ok(original_size as usize)
    }

    async fn write_zero_buffer(&self, iodev: &IoDevice, size: u64, offset: u64) -> io::Result<usize> {
        let mut dma = DmaFile::alloc_dma_buffer(size as usize);
        for b in dma.as_mut().iter_mut() {
            *b = 0;
        }
        iodev.file.write_at(offset, &dma).await
    }

    pub async fn fsync(&self, iodev: &IoDevice) -> io::Result<()> { iodev.file.fsync().await }
}
