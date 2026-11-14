//! Linux file assembly for the Qt transfer engine. Publication and fsync stay with the caller.
use std::ffi::{c_int, c_void};
use std::io;

const EIO: c_int = 5;
const EINTR: c_int = 4;
const EXDEV: c_int = 18;
const EINVAL: c_int = 22;
const ENOSYS: c_int = 38;
const EOPNOTSUPP: c_int = 95;

#[cfg(not(target_os = "linux"))]
compile_error!("IDM Native's file assembly currently targets Linux");

unsafe extern "C" {
    fn copy_file_range(
        input: c_int,
        input_offset: *mut i64,
        output: c_int,
        output_offset: *mut i64,
        length: usize,
        flags: u32,
    ) -> isize;
    fn read(fd: c_int, buffer: *mut c_void, length: usize) -> isize;
    fn write(fd: c_int, buffer: *const c_void, length: usize) -> isize;
}

fn errno() -> c_int {
    io::Error::last_os_error().raw_os_error().unwrap_or(EIO)
}

fn kernel_copy(input: c_int, output: c_int, length: usize) -> Result<usize, c_int> {
    // Null offsets advance the borrowed descriptors exactly like read/write.
    let result = unsafe {
        copy_file_range(
            input,
            std::ptr::null_mut(),
            output,
            std::ptr::null_mut(),
            length,
            0,
        )
    };
    if result < 0 {
        Err(errno())
    } else {
        Ok(result as usize)
    }
}

fn buffered_copy(input: c_int, output: c_int, mut remaining: u64) -> c_int {
    let mut buffer = [0u8; 128 * 1024];
    while remaining > 0 {
        let length = remaining.min(buffer.len() as u64) as usize;
        let count = unsafe { read(input, buffer.as_mut_ptr().cast(), length) };
        if count < 0 {
            let error = errno();
            if error == EINTR {
                continue;
            }
            return error;
        }
        if count == 0 {
            return EIO;
        }
        let count = count as usize;
        let mut written = 0;
        while written < count {
            let result = unsafe {
                write(
                    output,
                    buffer[written..count].as_ptr().cast(),
                    count - written,
                )
            };
            if result < 0 {
                let error = errno();
                if error == EINTR {
                    continue;
                }
                return error;
            }
            if result == 0 {
                return EIO;
            }
            written += result as usize;
        }
        remaining -= count as u64;
    }
    0
}

fn copy_exact(
    input: c_int,
    output: c_int,
    mut remaining: u64,
    mut copy: impl FnMut(c_int, c_int, usize) -> Result<usize, c_int>,
) -> c_int {
    while remaining > 0 {
        match copy(input, output, remaining.min(16 * 1024 * 1024) as usize) {
            Ok(0) => return EIO,
            Ok(count) => remaining -= count as u64,
            Err(EINTR) => continue,
            Err(EXDEV | EINVAL | ENOSYS | EOPNOTSUPP) => {
                return buffered_copy(input, output, remaining);
            }
            Err(error) => return error,
        }
    }
    0
}

/// Copy exactly `length` bytes at the current offsets of caller-owned descriptors.
/// Returns zero on success or a Linux errno on failure, including EIO for early EOF.
/// Offsets can advance on failure. Neither descriptor is closed; the caller must
/// flush userspace buffers before calling and discard incomplete output on error.
#[unsafe(no_mangle)]
pub extern "C" fn idm_copy_fd(input: c_int, output: c_int, length: u64) -> c_int {
    std::panic::catch_unwind(|| copy_exact(input, output, length, kernel_copy)).unwrap_or(EIO)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs::{self, File, OpenOptions};
    use std::io::{Read, Seek, SeekFrom, Write};
    use std::os::fd::AsRawFd;
    use std::path::PathBuf;
    use std::sync::atomic::{AtomicUsize, Ordering};

    struct Temp {
        path: PathBuf,
        file: File,
    }
    impl Temp {
        fn new() -> Self {
            static NEXT: AtomicUsize = AtomicUsize::new(0);
            let path = std::env::temp_dir().join(format!(
                "idm-io-{}-{}",
                std::process::id(),
                NEXT.fetch_add(1, Ordering::Relaxed)
            ));
            let file = OpenOptions::new()
                .read(true)
                .write(true)
                .create_new(true)
                .open(&path)
                .unwrap();
            Self { path, file }
        }
        fn bytes(&mut self) -> Vec<u8> {
            self.file.rewind().unwrap();
            let mut bytes = Vec::new();
            self.file.read_to_end(&mut bytes).unwrap();
            bytes
        }
    }
    impl Drop for Temp {
        fn drop(&mut self) {
            let _ = fs::remove_file(&self.path);
        }
    }

    #[test]
    fn kernel_copy_preserves_ownership_and_offsets() {
        let mut source = Temp::new();
        let mut output = Temp::new();
        source.file.write_all(b"prefixPAYLOADsuffix").unwrap();
        source.file.seek(SeekFrom::Start(6)).unwrap();
        output.file.write_all(b"header").unwrap();
        assert_eq!(
            idm_copy_fd(source.file.as_raw_fd(), output.file.as_raw_fd(), 7),
            0
        );
        assert_eq!(source.file.stream_position().unwrap(), 13);
        assert_eq!(output.file.stream_position().unwrap(), 13);
        assert_eq!(output.bytes(), b"headerPAYLOAD");
        // Both files still work after the FFI returns.
        source.file.write_all(b"!").unwrap();
        output.file.write_all(b"!").unwrap();
    }

    #[test]
    fn unsupported_kernel_copy_falls_back_for_multiple_chunks() {
        for error in [EXDEV, EINVAL, ENOSYS, EOPNOTSUPP] {
            let mut source = Temp::new();
            let mut output = Temp::new();
            let data: Vec<u8> = (0..400_123).map(|n| (n % 251) as u8).collect();
            source.file.write_all(&data).unwrap();
            source.file.rewind().unwrap();
            assert_eq!(
                copy_exact(
                    source.file.as_raw_fd(),
                    output.file.as_raw_fd(),
                    data.len() as u64,
                    |_, _, _| Err(error)
                ),
                0
            );
            assert_eq!(output.bytes(), data);
        }
    }

    #[test]
    fn fallback_continues_after_partial_kernel_copy_and_interruption() {
        let mut source = Temp::new();
        let mut output = Temp::new();
        source.file.write_all(b"0123456789").unwrap();
        source.file.rewind().unwrap();
        let mut call = 0;
        assert_eq!(
            copy_exact(
                source.file.as_raw_fd(),
                output.file.as_raw_fd(),
                10,
                |input, out, _| {
                    call += 1;
                    match call {
                        1 => Err(EINTR),
                        2 => {
                            assert_eq!(buffered_copy(input, out, 3), 0);
                            Ok(3)
                        }
                        _ => Err(EXDEV),
                    }
                }
            ),
            0
        );
        assert_eq!(output.bytes(), b"0123456789");
    }

    #[test]
    fn early_eof_and_bad_descriptors_fail() {
        let mut source = Temp::new();
        let output = Temp::new();
        source.file.write_all(b"short").unwrap();
        source.file.rewind().unwrap();
        assert_eq!(
            idm_copy_fd(source.file.as_raw_fd(), output.file.as_raw_fd(), 6),
            EIO
        );
        source.file.rewind().unwrap();
        assert_eq!(
            copy_exact(
                source.file.as_raw_fd(),
                output.file.as_raw_fd(),
                6,
                |_, _, _| Err(EXDEV)
            ),
            EIO
        );
        assert_eq!(idm_copy_fd(-1, output.file.as_raw_fd(), 1), 9);
        source.file.rewind().unwrap();
        assert_eq!(
            copy_exact(source.file.as_raw_fd(), -1, 5, |_, _, _| Err(EXDEV)),
            9
        );
        assert_eq!(idm_copy_fd(-1, -1, 0), 0);
    }

    #[test]
    #[ignore = "manual local file-copy benchmark; includes exact output verification"]
    fn benchmark_assembly() {
        use std::time::Instant;
        let mut source = Temp::new();
        let data: Vec<u8> = (0..64 * 1024 * 1024).map(|n| (n % 251) as u8).collect();
        source.file.write_all(&data).unwrap();
        source.file.rewind().unwrap();
        let mut baseline = Temp::new();
        let mut output = Temp::new();
        let start = Instant::now();
        loop {
            let mut buffer = vec![0; 65536];
            let count = source.file.read(&mut buffer).unwrap();
            if count == 0 {
                break;
            }
            baseline.file.write_all(&buffer[..count]).unwrap();
        }
        let old = start.elapsed();
        source.file.rewind().unwrap();
        let start = Instant::now();
        assert_eq!(
            idm_copy_fd(
                source.file.as_raw_fd(),
                output.file.as_raw_fd(),
                data.len() as u64
            ),
            0
        );
        let new = start.elapsed();
        assert_eq!(baseline.bytes(), data);
        assert_eq!(output.bytes(), data);
        println!(
            "64 MiB cached local copy, excluding fsync: allocated 64 KiB reads {old:?}, Rust kernel/fallback {new:?}; exact bytes verified"
        );
    }
}
