use alloc::vec::Vec;
use core::sync::atomic::Ordering::SeqCst;
use core::sync::atomic::{compiler_fence};

use hashbrown::HashMap;
#[allow(unused_imports)]
use crate::descriptors::{ChildDescriptor, RDMADescriptor};
use crate::descriptors::rdma::TransportGuard;
use crate::shadow_process::*;

#[allow(unused_imports)]
use crate::linux_kernel_module;

use crate::get_mem_pool_mut;
use os_network::bytes::ToBytes;
use os_network::{msg::UDMsg as RMemory, serialize::Serialize};

struct ProcessBundler {
    #[allow(dead_code)]
    process: ShadowProcess,
    serialized_buf: RMemory,
    serialized_buf_len: usize,

    #[allow(dead_code)] // keeps any transport-specific parent resource alive
    transport_guards: Vec<TransportGuard>,
}

impl ProcessBundler {
    fn new(process: ShadowProcess, transport_guard: TransportGuard) -> core::option::Option<Self> {
        let len = process.get_descriptor_ref().serialization_buf_len();
        crate::log::debug!(
            "Alloc serialization buf sz {} KB",
            len / 1024
        );
        let mut buf = unsafe { get_mem_pool_mut() }.pop_one();
        crate::log::debug!("serialization buf allocation done!");

        if len == 0 || len > buf.len() {
            crate::log::error!(
                "descriptor serialization refused: required {}, capacity {}",
                len,
                buf.len()
            );
            return None;
        }
        if !process.get_descriptor_ref().serialize(buf.get_bytes_mut()) {
            crate::log::error!(
                "descriptor serialization failed: required {}, capacity {}",
                len,
                buf.len()
            );
            return None;
        }
        compiler_fence(SeqCst);

        crate::log::debug!("Process bundle descriptor len: {}", buf.len());

        let mut transport_guards = Vec::new();
        transport_guards.push(transport_guard);

        Some(Self {
            process: process,
            serialized_buf: buf,
            serialized_buf_len: len,
            transport_guards,
        })
    }

    fn get_serialize_buf_sz(&self) -> usize {
        self.serialized_buf_len
    }
}

pub struct ShadowProcessService {
    registered_processes: HashMap<usize, ProcessBundler>,
}

impl ShadowProcessService {
    pub fn new() -> Self {
        Self {
            registered_processes: Default::default(),
        }
    }

    pub fn query_descriptor_buf(&self, key: usize) -> core::option::Option<(&RMemory, usize)> {
        self.registered_processes
            .get(&key)
            .map(|s| (&s.serialized_buf, s.serialized_buf_len))
    }

    pub fn query_descriptor(
        &self,
        key: usize,
    ) -> core::option::Option<&crate::descriptors::ParentDescriptor> {
        self.registered_processes
            .get(&key)
            .map(|s| s.process.get_descriptor_ref())
    }

    /// # Return
    /// * The size of the serialization buffer
    pub fn add_myself_copy(&mut self, key: usize) -> core::option::Option<usize> {
        if self.registered_processes.contains_key(&key) {
            crate::log::warn!(
                "Failed to prepare: the register key {} has already been taken. ",
                key
            );
            return None;
        }

        let (transport_guard, descriptor) = RDMADescriptor::new_for_transport()?;

        let bundler = ProcessBundler::new(
            crate::shadow_process::ShadowProcess::new_copy(descriptor),
            transport_guard,
        )?;
        let ret = bundler.get_serialize_buf_sz();

        self.registered_processes.insert(key, bundler);

        return Some(ret);
    }

    /// # Return
    /// * The size of the serialization buffer    
    pub fn add_myself_cow(&mut self, key: usize) -> core::option::Option<usize> {
        if self.registered_processes.contains_key(&key) {
            crate::log::warn!(
                "Failed to prepare: the register key {} has already been taken. ",
                key
            );
            return None;
        }

        let (transport_guard, descriptor) = RDMADescriptor::new_for_transport()?;

        let bundler = ProcessBundler::new(
            crate::shadow_process::ShadowProcess::new_cow(descriptor),
            transport_guard,
        )?;
        let ret = bundler.get_serialize_buf_sz();

        self.registered_processes.insert(key, bundler);

        return Some(ret);
    }

    pub fn unregister(&mut self, key: usize) {
        self.registered_processes.remove(&key);
    }
}
