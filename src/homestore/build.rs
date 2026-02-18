// Build script to enforce mutual exclusivity of sync_code and async_code features

fn main() {
    // Cargo sets CARGO_FEATURE_<name> environment variables for enabled features
    let sync_enabled = std::env::var("CARGO_FEATURE_SYNC_CODE").is_ok();
    let async_enabled = std::env::var("CARGO_FEATURE_ASYNC_CODE").is_ok();

    // Check mutual exclusivity
    if sync_enabled && async_enabled {
        panic!(
            "\n\n\
            ╔══════════════════════════════════════════════════════════════╗\n\
            ║  ERROR: Mutually Exclusive Features Detected                ║\n\
            ╠══════════════════════════════════════════════════════════════╣\n\
            ║  Features 'sync_code' and 'async_code' are mutually         ║\n\
            ║  exclusive. Please enable only one of them.                 ║\n\
            ║                                                              ║\n\
            ║  For sync mode:   --features sync_code --no-default-features║\n\
            ║  For async mode:  --features async_code (or use default)    ║\n\
            ╚══════════════════════════════════════════════════════════════╝\n"
        );
    }

    // If neither is set, default to async_code (matches the default feature)
    if !sync_enabled && !async_enabled {
        println!("cargo:rustc-cfg=feature=\"async_code\"");
        println!("cargo:warning=No sync/async mode specified, defaulting to async_code");
    }

    println!("cargo:rerun-if-changed=build.rs");
}
