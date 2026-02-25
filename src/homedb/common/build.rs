fn main() {
    let sync_frontend  = std::env::var("CARGO_FEATURE_SYNC_FRONTEND").is_ok();
    let async_frontend = std::env::var("CARGO_FEATURE_ASYNC_FRONTEND").is_ok();
    let sync_backend   = std::env::var("CARGO_FEATURE_SYNC_BACKEND").is_ok();
    let async_backend  = std::env::var("CARGO_FEATURE_ASYNC_BACKEND").is_ok();

    if sync_frontend && async_frontend {
        eprintln!("cargo:error=sync_frontend and async_frontend cannot both be enabled");
        std::process::exit(1);
    }
    if sync_backend && async_backend {
        eprintln!("cargo:error=sync_backend and async_backend cannot both be enabled");
        std::process::exit(1);
    }
    if async_frontend && sync_backend {
        eprintln!("cargo:error=async_frontend requires async_backend (not sync_backend)");
        std::process::exit(1);
    }
}
