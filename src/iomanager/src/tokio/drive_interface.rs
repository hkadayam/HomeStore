use std::{io, sync::Arc};

use tokio::fs::File as TokioFile;

/// IOBuffer backed by AlignedBox for tokio.
pub struct IOBuffer(pub aligned_box::AlignedBox<[u8]>);

impl IOBuffer {
    pub fn new(size: usize) -> Self {
        // Round up size to alignment boundary (4096) to maintain alignment guarantees
        const ALIGNMENT: usize = 4096;
        let aligned_size = ((size + ALIGNMENT - 1) / ALIGNMENT) * ALIGNMENT;

        Self(aligned_box::AlignedBox::slice_from_default(ALIGNMENT, aligned_size)
            .expect("Failed to allocate aligned buffer"))
    }

    pub fn from_vec(vec: Vec<u8>) -> Self {
        const ALIGNMENT: usize = 4096;
        let size = vec.len();
        let aligned_size = ((size + ALIGNMENT - 1) / ALIGNMENT) * ALIGNMENT;

        let mut buffer = aligned_box::AlignedBox::slice_from_default(ALIGNMENT, aligned_size)
            .expect("Failed to allocate aligned buffer");
        buffer[..size].copy_from_slice(&vec);
        Self(buffer)
    }

    pub fn len(&self) -> usize { self.0.len() }
    pub fn as_slice(&self) -> &[u8] { &self.0 }
    pub fn as_mut_slice(&mut self) -> &mut [u8] { &mut self.0 }
    pub fn alignment(&self) -> usize { 4096 }
    pub fn resize(&mut self, new_len: usize) {
        if new_len != self.len() {
            // Round up to alignment boundary to maintain alignment guarantees
            const ALIGNMENT: usize = 4096;
            let aligned_size = ((new_len + ALIGNMENT - 1) / ALIGNMENT) * ALIGNMENT;

            // AlignedBox doesn't have resize, so allocate new and copy
            let mut new_box = aligned_box::AlignedBox::slice_from_default(ALIGNMENT, aligned_size)
                .expect("Failed to allocate aligned buffer");
            let to_copy = std::cmp::min(self.len(), aligned_size);
            new_box[..to_copy].copy_from_slice(&self.0[..to_copy]);
            self.0 = new_box;
        }
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

pub struct IoDevice {
    file: Arc<TokioFile>,
    dev_id: u32,
    dev_name: String,
    is_block_device: bool,
}

impl IoDevice {
    pub fn dev_id(&self) -> u32 { self.dev_id }
    pub fn dev_name(&self) -> &str { &self.dev_name }
    pub fn file(&self) -> &TokioFile { &self.file }
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
    pub async fn open_dev(devname: String, oflags: i32) -> io::Result<Arc<IoDevice>> {
        use std::fs::OpenOptions as StdOpenOptions;
        #[cfg(unix)]
        use std::os::unix::fs::{OpenOptionsExt, FileTypeExt};
        
        #[cfg(unix)]
        let std_file = StdOpenOptions::new().read(true).write(true).custom_flags(oflags).open(&devname)?;
        #[cfg(not(unix))]
        let std_file = StdOpenOptions::new().read(true).write(true).open(&devname)?;
        
        // Detect if this is a block device
        #[cfg(unix)]
        let is_block_device = std_file.metadata()?.file_type().is_block_device();
        #[cfg(not(unix))]
        let is_block_device = false;
        
        let file = TokioFile::from_std(std_file);
        let dev_id = devname.as_bytes().iter().map(|&b| b as u32).sum();
        Ok(Arc::new(IoDevice { file: Arc::new(file), dev_id, dev_name: devname, is_block_device }))
    }

    pub async fn get_size(iodev: &IoDevice) -> io::Result<u64> { Ok(iodev.file.metadata().await?.len()) }

    pub fn close_dev(&self, _iodev: Arc<IoDevice>) {}

    pub async fn read(&self, iodev: &IoDevice, mut buf: IOBuffer, offset: u64) -> (io::Result<usize>, IOBuffer) {
        use tokio::io::{AsyncReadExt, AsyncSeekExt};
        let res = async {
            let mut fh = iodev.file.try_clone().await?;
            fh.seek(io::SeekFrom::Start(offset)).await?;
            fh.read_exact(buf.as_mut_slice()).await?;
            Ok(buf.len())
        }
        .await;
        (res, buf)
    }

    pub async fn write(&self, iodev: &IoDevice, buf: &IOBuffer, offset: u64) -> io::Result<usize> {
        use tokio::io::{AsyncSeekExt, AsyncWriteExt};
        let mut fh = iodev.file.try_clone().await?;
        fh.seek(io::SeekFrom::Start(offset)).await?;
        fh.write_all(buf.as_slice()).await?;
        Ok(buf.len())
    }

    pub async fn readv(
        &self, iodev: &IoDevice, mut bufs: Vec<IOBuffer>, mut offset: u64,
    ) -> (io::Result<usize>, Vec<IOBuffer>) {
        use tokio::io::{AsyncReadExt, AsyncSeekExt};
        let mut fh = match iodev.file.try_clone().await {
            Ok(f) => f,
            Err(e) => return (Err(e), bufs),
        };
        let mut total = 0usize;
        for b in bufs.iter_mut() {
            if let Err(e) = fh.seek(io::SeekFrom::Start(offset)).await {
                return (Err(e), bufs);
            }
            if let Err(e) = fh.read_exact(b.as_mut_slice()).await {
                return (Err(e), bufs);
            }
            let n = b.len();
            total += n;
            offset += n as u64;
        }
        (Ok(total), bufs)
    }

    pub async fn writev(&self, iodev: &IoDevice, bufs: Vec<IOBuffer>, mut offset: u64) -> io::Result<usize> {
        use tokio::io::{AsyncSeekExt, AsyncWriteExt};
        let mut fh = iodev.file.try_clone().await?;
        let mut total = 0usize;
        for b in bufs.into_iter() {
            fh.seek(io::SeekFrom::Start(offset)).await?;
            fh.write_all(b.as_slice()).await?;
            let n = b.len();
            total += n;
            offset += n as u64;
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
            
            // Perform ioctl in blocking thread pool
            let fd_copy = fd;
            let result = tokio::task::spawn_blocking(move || {
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
        let mut buf = IOBuffer::new(size as usize);
        for b in buf.as_mut_slice().iter_mut() {
            *b = 0;
        }
        self.write(iodev, &buf, offset).await
    }

    pub async fn fsync(&self, iodev: &IoDevice) -> io::Result<()> {
        let fh = iodev.file.try_clone().await?;
        fh.sync_all().await
    }
}
