use crate::bindings::{mm_struct, pmem_flush_tlb_all, vm_area_struct};

use super::vma::VMA;

pub type VirtAddrType = u64;
pub type PhyAddrType = u64;

/// Simpler wrapper of the kernel's `mm_struct`
/// It provides some handy utilities written in rust
#[derive(Debug)]
pub struct MemoryDescriptor {
    mm_inner: &'static mut mm_struct,
}

pub struct MMReadGuard {
    mm: *mut mm_struct,
}

impl Drop for MMReadGuard {
    fn drop(&mut self) {
        unsafe { crate::bindings::pmem_mmap_read_unlock(self.mm) };
    }
}

/// taken from /include/uapi/linux/mman.h in the linux kernel
#[allow(dead_code)]
pub mod mmap_flags {
    pub const MAP_SHARED: crate::linux_kernel_module::c_types::c_ulong = 0x01;
    pub const MAP_PRIVATE: crate::linux_kernel_module::c_types::c_ulong = 0x02;
}

#[allow(dead_code)]
impl MemoryDescriptor {
    pub unsafe fn new(mm_ptr: *mut mm_struct) -> Self {
        Self {
            mm_inner: &mut (*mm_ptr),
        }
    }

    pub fn get_vma_iter(&self) -> VMAIter {
        VMAIter::new(self)
    }

    pub fn read_lock(&self) -> MMReadGuard {
        let mm = self.mm_inner as *const _ as *mut mm_struct;
        unsafe { crate::bindings::pmem_mmap_read_lock(mm) };
        MMReadGuard { mm }
    }

    pub fn find_vma(
        &self,
        addr: VirtAddrType,
    ) -> core::option::Option<&'static mut vm_area_struct> {
        let vma_p =
            unsafe { crate::bindings::find_vma(self.mm_inner as *const _ as *mut mm_struct, addr) };
        if vma_p == core::ptr::null_mut() {
            return None;
        }
        return unsafe { Some(&mut (*vma_p)) };
    }

    #[allow(dead_code)]
    pub fn unmap_region(
        &mut self,
        addr_s: VirtAddrType,
        sz: usize,
    ) -> crate::linux_kernel_module::c_types::c_int {
        use crate::bindings::pmem_do_munmap;
        unsafe { pmem_do_munmap(self.get_mm_inner(), addr_s, sz, core::ptr::null_mut()) }
    }

    // According to:  https://www.kernel.org/doc/html/latest/core-api/cachetlb.html
    #[allow(dead_code)]
    #[inline]
    pub fn flush_tlb_all(&mut self) {
        unsafe { pmem_flush_tlb_all() };
    }

    #[allow(dead_code)]
    #[inline]
    pub fn flush_tlb_mm(&mut self) {
        use crate::bindings::pmem_flush_tlb_mm;
        unsafe {
            pmem_flush_tlb_mm(self.mm_inner as *mut _);
        }
    }

    // Walk the Linux page table, find the PTE corresponding
    // to the target virtual address.
    pub fn find_pte(
        &mut self, 
        addr: VirtAddrType
    ) -> core::option::Option<&'static mut crate::bindings::pte_t> {
        let pte_p = unsafe { 
            crate::bindings::pmem_get_pte(
                self.mm_inner as *const _ as *mut mm_struct, addr) 
        };
        if pte_p == core::ptr::null_mut() {
            return None;
        }
        return unsafe { Some(&mut (*pte_p)) };
    }
}

impl MemoryDescriptor {
    /// find a specific virtual memory area (VMA) based on the index
    ///
    /// # Arguments
    /// * `idx` - index in the `mm_struct`'s ordered VMA set
    #[allow(dead_code)]
    pub fn get_vma_area(&self, idx: usize) -> core::option::Option<*mut vm_area_struct> {
        self.get_vma_iter()
            .nth(idx)
            .map(|vma| unsafe { vma.get_raw_ptr() })
    }

    #[allow(dead_code)]
    fn get_mm_inner(&mut self) -> *mut mm_struct {
        self.mm_inner as *mut _
    }
}

/// Init an iterator to iterate all the vm_area_struct of a process
///
/// Usage:
/// ```
/// let mm = MemoryDescriptor::new(...);
/// let vma_iter = mm.get_vma_iter();
/// for vma in vma_iter {
///     ... s
/// }
/// ```
pub struct VMAIter {
    mm: *mut mm_struct,
    next_addr: VirtAddrType,
}

// uses an iterator to simplfiy memory range traversal
impl Iterator for VMAIter {
    type Item = VMA<'static>;

    fn next(&mut self) -> Option<Self::Item> {
        let vma = unsafe { crate::bindings::find_vma(self.mm, self.next_addr) };
        if vma == core::ptr::null_mut() {
            return None;
        }

        self.next_addr = unsafe { crate::bindings::pmem_vma_get_end(vma) };
        unsafe { Some(VMA::new(&mut (*vma))) }
    }
}

impl VMAIter {
    pub fn new(m: &MemoryDescriptor) -> Self {
        Self {
            mm: m.mm_inner as *const _ as *mut mm_struct,
            next_addr: 0,
        }
    }
}
