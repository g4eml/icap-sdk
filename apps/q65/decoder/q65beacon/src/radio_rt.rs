// SPDX-License-Identifier: GPL-3.0-or-later
//! Bare-metal runtime glue for the on-radio ICAP build.
//!
//! Provides the one global allocator and one panic handler the no_std
//! staticlib needs to link into the ICAP C app (replaces the `bare`
//! feature's stubs, which are compile/size-proof only).
//!
//!  * allocator: a free-list heap (`linked_list_allocator`) over the ABI
//!    v2 region the loader granted (`svc->heap_base/heap_size`); the C
//!    shim hands it to us once, at runtime, via `q65b_heap_init`.
//!  * panic handler: notes the panic to the shim's SD debug log, then
//!    traps with an undefined instruction so the firmware's app-task
//!    fault handler reaps only this app ("App crashed", ICAP APP_API §2).
//!    The SD debug log already holds every decode stage up to the fault.
use core::panic::PanicInfo;
use linked_list_allocator::LockedHeap;

#[global_allocator]
static HEAP: LockedHeap = LockedHeap::empty();

/// Initialise the decoder heap over `[base, base+size)`.  Must be called
/// by the C shim exactly once, before `q65b_decode_cb()`.  `base` must be
/// 8-byte aligned and point at `size` bytes of zeroed, writable region
/// memory (the ABI v2 `svc->heap_base`, after the loaded `.rodata`).
///
/// # Safety
/// `base`/`size` must describe a region owned exclusively by the decoder
/// for its lifetime.
#[no_mangle]
pub unsafe extern "C" fn q65b_heap_init(base: *mut u8, size: usize) {
    HEAP.lock().init(base, size);
}

extern "C" {
    /// Provided by the C shim: write the buffered debug log (with a PANIC
    /// marker) out to the SD card before the app is reaped.
    fn q65b_panic_note();
}

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    unsafe {
        q65b_panic_note();
    }
    #[cfg(target_arch = "arm")]
    unsafe {
        core::arch::asm!("udf #0", options(noreturn));
    }
    #[cfg(not(target_arch = "arm"))]
    loop {}
}
