pub use vma::*;
pub use page_table::*;
pub use page::*;

use crate::descriptors::{ParentDescriptor, CompactPageTable};
use alloc::vec::Vec;
use rust_kernel_rdma_base::VmallocAllocator;

#[allow(unused_imports)]
use crate::linux_kernel_module;

#[allow(dead_code)]
pub struct ShadowProcess {
    descriptor: ParentDescriptor,

    shadow_vmas: Vec<ShadowVMA<'static>>,

    // COW shadow page table is only needed.
    // However, for testing purposes, we need to maintain the copy page table.
    // FIXME: maybe we should use enum for this?
    copy_shadow_pagetable: core::option::Option<ShadowPageTable<Copy4KPage>>,
    cow_shadow_pagetable: core::option::Option<ShadowPageTable<COW4KPage>>,
}

impl ShadowProcess {
    pub fn get_descriptor_ref(&self) -> &ParentDescriptor {
        &self.descriptor
    }
}

impl ShadowProcess {
    /// Crate a new shadow processing by marking all the
    /// memories of the original one to copy-on-write(COW).
    pub fn new_cow(rdma_descriptor: crate::descriptors::RDMADescriptor) -> Self {
        let mut shadow_pt = ShadowPageTable::<COW4KPage>::new();
        let mut shadow_vmas: Vec<ShadowVMA<'static>> = Vec::new();

        let mut vma_descriptors = Vec::new();
        let mut vma_page_table: Vec<CompactPageTable, VmallocAllocator> = Vec::new_in(VmallocAllocator);
        // the generation process
        let task = crate::kern_wrappers::task::Task::new();
        let mut mm = task.get_memory_descriptor();
        let _mmap_guard = mm.read_lock();

        for vma in mm.get_vma_iter() {
            vma_descriptors.push(vma.generate_descriptor());
            shadow_vmas.push(ShadowVMA::new(vma, true));
            vma_page_table.push(Default::default());
        }

        for (idx, _) in mm.get_vma_iter().enumerate() {
            let pt: &mut CompactPageTable = vma_page_table.get_mut(idx).unwrap();
            let s_vma = shadow_vmas.get(idx).unwrap();
            VMACOWPTGenerator::new(s_vma, &mut shadow_pt, pt).generate();
        }
        // clear the TLB
        mm.flush_tlb_mm();

        let regs = task.generate_reg_descriptor();
        let saved_sp = regs.others.sp;
        let saved_ip = regs.others.ip;
        let stack_page = saved_sp & !4095;
        let ip_page = saved_ip & !4095;
        let lookup = |addr: u64| {
            vma_descriptors
                .iter()
                .enumerate()
                .find(|(_, vma)| addr >= vma.get_start() && addr < vma.get_end())
                .and_then(|(idx, vma)| {
                    vma_page_table[idx]
                        .lookup_offset((addr - vma.get_start()) as u32)
                })
        };
        let stack_pa = lookup(stack_page);
        let ip_pa = lookup(ip_page);
        let mut stack_word = 0u64;
        let stack_read_rc = unsafe {
            crate::bindings::pmem_copy_from_user_u64(saved_sp, &mut stack_word as *mut _)
        };
        crate::log::info!(
            "MITOSIS_DIAG event=prepare_resume_pages saved_ip=0x{:x} saved_sp=0x{:x} ip_hit={} ip_pa=0x{:x} stack_hit={} stack_pa=0x{:x} stack_read_rc={} stack_word=0x{:x}",
            saved_ip,
            saved_sp,
            ip_pa.is_some(),
            ip_pa.unwrap_or(0),
            stack_pa.is_some(),
            stack_pa.unwrap_or(0),
            stack_read_rc,
            stack_word
        );

        Self {
            shadow_vmas,
            cow_shadow_pagetable: Some(shadow_pt),
            copy_shadow_pagetable: None,
            descriptor: ParentDescriptor {
                machine_info: rdma_descriptor,
                regs,
                page_table: vma_page_table,
                vma: vma_descriptors,
            },
        }
    }

    pub fn new_copy(rdma_descriptor: crate::descriptors::RDMADescriptor) -> Self {
        let mut shadow_pt = ShadowPageTable::<Copy4KPage>::new();
        let mut shadow_vmas: Vec<ShadowVMA<'static>> = Vec::new();

        let mut vma_descriptors = Vec::new();
        let mut vma_page_table = Vec::new_in(VmallocAllocator);
        // the generation process
        let task = crate::kern_wrappers::task::Task::new();
        let mm = task.get_memory_descriptor();
        let _mmap_guard = mm.read_lock();

        // crate::log::debug!("before iterating the VMAs");
        for vma in mm.get_vma_iter() {
            vma_descriptors.push(vma.generate_descriptor());
            shadow_vmas.push(ShadowVMA::new(vma, false));
            vma_page_table.push(Default::default());
        }

        // crate::log::debug!("before iterating the page table");
        for (idx, _) in mm.get_vma_iter().enumerate() {
            let pt: &mut CompactPageTable = vma_page_table.get_mut(idx).unwrap();
            let s_vma = shadow_vmas.get(idx).unwrap();
            VMACopyPTGenerator::new(s_vma, &mut shadow_pt, pt).generate();
        }

        Self {
            shadow_vmas,
            cow_shadow_pagetable: None,
            copy_shadow_pagetable: Some(shadow_pt),
            descriptor: ParentDescriptor {
                machine_info: rdma_descriptor,
                regs: task.generate_reg_descriptor(),
                page_table: vma_page_table,
                vma: vma_descriptors,
            },
        }
    }
}

pub mod vma;
pub mod page_table;
pub mod page;
