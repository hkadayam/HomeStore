use proc_macro::TokenStream;
use quote::quote;
use syn::{parse_macro_input, ItemFn, LitInt};

/// Macro to run async tests using iomanager runtime.
///
/// This macro transforms an async test function into a regular test that runs
/// on the iomanager runtime. It automatically initializes the iomanager on first use.
///
/// # Example
///
/// ```rust
/// use iomgr::{iomgr, iomanager_test};
///
/// // Default: 4 threads
/// #[iomanager_test]
/// async fn test_something() {
///     let result = iomgr().spawn_on_reactor_with_result(0, async {
///         42
///     }).await;
///     assert_eq!(result, 42);
/// }
///
/// // Custom: 8 threads
/// #[iomanager_test(8)]
/// async fn test_parallel() {
///     // test with more threads
/// }
/// ```
///
/// This is equivalent to:
///
/// ```rust
/// #[test]
/// fn test_something() {
///     static INIT: std::sync::Once = std::sync::Once::new();
///     INIT.call_once(|| {
///         iomgr::init_iomgr(4).expect("Failed to init IOManager");
///     });
///     
///     iomgr::run_test(async {
///         let result = iomanager().spawn_on_reactor_with_result(0, async {
///             42
///         }).await;
///         assert_eq!(result, 42);
///     });
/// }
/// ```
#[proc_macro_attribute]
pub fn iomanager_test(attr: TokenStream, item: TokenStream) -> TokenStream {
    let input = parse_macro_input!(item as ItemFn);

    // Parse the number of threads (default to 4 if not specified)
    let num_threads = if attr.is_empty() {
        4
    } else {
        match syn::parse::<LitInt>(attr) {
            Ok(lit) => lit.base10_parse::<usize>().unwrap_or(4),
            Err(_) => {
                return syn::Error::new_spanned(
                    &input.sig.fn_token,
                    "iomanager_test attribute expects an integer (e.g., #[iomanager_test(8)])"
                )
                .to_compile_error()
                .into();
            }
        }
    };

    let fn_name = &input.sig.ident;
    let fn_block = &input.block;
    let fn_attrs = &input.attrs;
    let fn_vis = &input.vis;

    // Check if function is async
    if input.sig.asyncness.is_none() {
        return syn::Error::new_spanned(&input.sig.fn_token, "iomanager_test functions must be async")
            .to_compile_error()
            .into();
    }

    // Check if function has parameters
    if !input.sig.inputs.is_empty() {
        return syn::Error::new_spanned(&input.sig.inputs, "iomanager_test functions cannot have parameters")
            .to_compile_error()
            .into();
    }

    let expanded = quote! {
        #(#fn_attrs)*
        #[test]
        #fn_vis fn #fn_name() {
            // #region agent log
            let test_name = stringify!(#fn_name);
            let thread_id = std::thread::current().id();
            let log_entry = format!("{{\"location\":\"macro:93\",\"message\":\"test entry\",\"data\":{{\"test\":\"{}\",\"thread\":\"{:?}\"}},\"timestamp\":{},\"hypothesisId\":\"H5\"}}\n", test_name, thread_id, std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_millis());
            let _ = std::fs::OpenOptions::new().create(true).append(true).open("/Users/hkadayam/src/Homestore/.cursor/debug.log").and_then(|mut f| std::io::Write::write_all(&mut f, log_entry.as_bytes()));
            // #endregion

            // Init IOManager (refcounting handles concurrent tests)
            let _ = iomgr::init_iomgr(#num_threads);

            // #region agent log
            let log_entry = format!("{{\"location\":\"macro:97\",\"message\":\"test pre-run_test\",\"data\":{{\"test\":\"{}\",\"thread\":\"{:?}\"}},\"timestamp\":{},\"hypothesisId\":\"H5\"}}\n", test_name, thread_id, std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_millis());
            let _ = std::fs::OpenOptions::new().create(true).append(true).open("/Users/hkadayam/src/Homestore/.cursor/debug.log").and_then(|mut f| std::io::Write::write_all(&mut f, log_entry.as_bytes()));
            // #endregion

            // Run test (includes shutdown at end)
            iomgr::run_test(async #fn_block);

            // #region agent log
            let log_entry = format!("{{\"location\":\"macro:102\",\"message\":\"test exit\",\"data\":{{\"test\":\"{}\",\"thread\":\"{:?}\"}},\"timestamp\":{},\"hypothesisId\":\"H5\"}}\n", test_name, thread_id, std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_millis());
            let _ = std::fs::OpenOptions::new().create(true).append(true).open("/Users/hkadayam/src/Homestore/.cursor/debug.log").and_then(|mut f| std::io::Write::write_all(&mut f, log_entry.as_bytes()));
            // #endregion
        }
    };

    TokenStream::from(expanded)
}

