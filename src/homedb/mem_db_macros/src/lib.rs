//! reactor_method macro
//! 
//! Inline body into spawn closure to avoid recursion

use proc_macro::TokenStream;
use quote::quote;
use syn::{parse_macro_input, ItemFn, FnArg, visit_mut, visit_mut::VisitMut};

struct SelfReplacer;

impl VisitMut for SelfReplacer {
    fn visit_expr_mut(&mut self, expr: &mut syn::Expr) {
        if let syn::Expr::Path(ref mut path_expr) = expr {
            if path_expr.path.is_ident("self") {
                *expr = syn::parse_quote!(&cloned);
                return;
            }
        }
        visit_mut::visit_expr_mut(self, expr);
    }
    
    fn visit_receiver_mut(&mut self, receiver: &mut syn::Receiver) {
        // Replace &self, &mut self with &cloned
        visit_mut::visit_receiver_mut(self, receiver);
    }
}

#[proc_macro_attribute]
pub fn reactor_method(_attr: TokenStream, item: TokenStream) -> TokenStream {
    let input = parse_macro_input!(item as ItemFn);
    
    let attrs = &input.attrs;
    let vis = &input.vis;
    let sig = &input.sig;
    let mut body = input.block.clone();
    
    // Transform self references in body to cloned
    SelfReplacer.visit_block_mut(&mut body);
    
    let param_names: Vec<_> = sig.inputs.iter().skip(1).filter_map(|arg| {
        if let FnArg::Typed(pat_type) = arg {
            if let syn::Pat::Ident(pat_ident) = &*pat_type.pat {
                return Some(&pat_ident.ident);
            }
        }
        None
    }).collect();
    
    let original_body = &input.block;
    
    let expanded = quote! {
        #(#attrs)*
        #vis #sig {
            if iomgr::iomgr().current_reactor().is_some() {
                #original_body
            } else {
                let cloned = self.clone();
                iomgr::spawn(async move {
                    #body
                }).join()
            }
        }
    };
    
    TokenStream::from(expanded)
}
