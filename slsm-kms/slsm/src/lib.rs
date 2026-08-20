#![no_std]
#![feature(allocator_api, get_mut_unchecked, new_uninit)]

extern crate alloc;

use alloc::sync::Arc;
use alloc::vec::Vec;
use core::ptr;
use core::sync::atomic::{compiler_fence, AtomicBool, Ordering};

use KRdmaKit::comm_manager::{CMServer, Explorer};
use KRdmaKit::context::Context;
use KRdmaKit::memory_region::MemoryRegion;
use KRdmaKit::queue_pairs::builder::QueuePairBuilder;
use KRdmaKit::services::rc::ReliableConnectionServer;
use KRdmaKit::{KDriver, QueuePair};
use KRdmaKit::rdma_shim::bindings::{ib_gid, ib_wc};
use KRdmaKit::rdma_shim::linux_kernel_module;
use KRdmaKit::rdma_shim::linux_kernel_module::bindings;
use KRdmaKit::rdma_shim::linux_kernel_module::c_types::{
    c_int, c_long, c_uint, c_ulong, c_void,
};
use KRdmaKit::rdma_shim::linux_kernel_module::file_operations::FileOperations;
use KRdmaKit::rdma_shim::rust_kernel_linux_util::{kthread, timer::KTimer};
use KRdmaKit::rdma_shim::{log, Error};

const ABI_VERSION: u32 = 1;
const MAX_CHUNKS: usize = 8;
const CHUNK_SIZE: usize = 1024 * 1024;
const MAX_SST_SIZE: usize = MAX_CHUNKS * CHUNK_SIZE;
const GID_INDEX: usize = 3;
const RDMA_PORT: u8 = 1;
const SERVICE_ID: u64 = 0x534c_534d_0001;
const CAP_SYS_RAWIO: c_int = 17;

// The legacy linux-kernel-module bindings whitelist only a few errno values.
const ERRNO_ENOENT: u32 = 2;
const ERRNO_EPERM: u32 = 1;
const ERRNO_EIO: u32 = 5;
const ERRNO_EBUSY: u32 = 16;
const ERRNO_ENODEV: u32 = 19;
const ERRNO_ENOTTY: u32 = 25;
const ERRNO_EBADMSG: u32 = 74;
const ERRNO_EOVERFLOW: u32 = 75;
const ERRNO_ENOTCONN: u32 = 107;
const ERRNO_ECONNREFUSED: u32 = 111;
const ERRNO_EHOSTUNREACH: u32 = 113;
const ERRNO_EADDRINUSE: u32 = 98;

const IOCTL_CONNECT_PEER: u32 = 0x534c_0001;
const IOCTL_REGISTER_REGION: u32 = 0x534c_0002;
const IOCTL_FETCH_REGION: u32 = 0x534c_0003;
const IOCTL_UNREGISTER_REGION: u32 = 0x534c_0004;
const IOCTL_DISCONNECT: u32 = 0x534c_0005;

const FNV_OFFSET_BASIS: u64 = 0xcbf2_9ce4_8422_2325;
const FNV_PRIME: u64 = 0x0000_0100_0000_01b3;

#[repr(C)]
#[derive(Copy, Clone, Default)]
struct RemoteChunk {
    remote_addr: u64,
    length: u32,
    rkey: u32,
}

#[repr(C)]
#[derive(Copy, Clone, Default)]
struct SstDescriptor {
    version: u32,
    chunk_count: u32,
    sst_id: u64,
    total_length: u64,
    chunk_size: u32,
    reserved: u32,
    checksum: u64,
    owner_gid: [u8; 16],
    service_id: u64,
    chunks: [RemoteChunk; MAX_CHUNKS],
}

#[repr(C)]
#[derive(Copy, Clone, Default)]
struct ConnectReq {
    version: u32,
    reserved: u32,
    peer_gid: [u8; 16],
    service_id: u64,
}

#[repr(C)]
#[derive(Copy, Clone, Default)]
struct RegisterReq {
    user_addr: u64,
    length: u64,
    sst_id: u64,
    descriptor: SstDescriptor,
}

#[repr(C)]
#[derive(Copy, Clone, Default)]
struct FetchReq {
    user_addr: u64,
    capacity: u64,
    descriptor: SstDescriptor,
    fetched_length: u64,
    checksum: u64,
    elapsed_us: u64,
}

struct SlsmMemoryRegion {
    inner: MemoryRegion,
    data: usize,
}

impl SlsmMemoryRegion {
    fn new(context: Arc<Context>, capacity: usize) -> Result<Self, c_long> {
        let data = unsafe {
            bindings::krealloc(ptr::null(), capacity as _, bindings::GFP_KERNEL)
        };
        if data.is_null() {
            return Err(errno(bindings::ENOMEM));
        }
        let inner = match unsafe { MemoryRegion::new_from_raw(context, data, capacity) } {
            Ok(mr) => mr,
            Err(_) => {
                unsafe { bindings::kfree(data as *const c_void) };
                return Err(errno(bindings::ENOMEM));
            }
        };
        Ok(Self {
            inner,
            data: data as usize,
        })
    }

    fn mr(&self) -> &MemoryRegion {
        &self.inner
    }
}

impl Drop for SlsmMemoryRegion {
    fn drop(&mut self) {
        unsafe { bindings::kfree(self.data as *const c_void) };
    }
}

struct RegisteredRegion {
    _chunks: Vec<SlsmMemoryRegion>,
    _descriptor: SstDescriptor,
}

pub struct SlsmFile {
    context: Arc<Context>,
    registered: Option<RegisteredRegion>,
    connection: Option<Arc<QueuePair>>,
}

static mut GLOBAL_CONTEXT: Option<Arc<Context>> = None;
static mut GLOBAL_DRIVER: Option<Arc<KDriver>> = None;
static mut GLOBAL_SERVER: Option<Arc<ReliableConnectionServer>> = None;
static mut GLOBAL_CM_SERVER: Option<CMServer<ReliableConnectionServer>> = None;
static DEVICE_OPEN: AtomicBool = AtomicBool::new(false);

extern "C" {
    fn capable(capability: c_int) -> bool;
}

fn has_rawio_capability() -> bool {
    unsafe { capable(CAP_SYS_RAWIO) }
}

fn errno(code: u32) -> c_long {
    -(code as c_long)
}

fn kernel_error(code: u32) -> Error {
    Error::from_kernel_errno(-(code as i32))
}

unsafe fn copy_from_user<T: Copy + Default>(arg: c_ulong) -> Result<T, c_long> {
    let mut value = T::default();
    let not_copied = bindings::_copy_from_user(
        (&mut value as *mut T).cast::<c_void>(),
        arg as *const c_void,
        core::mem::size_of::<T>() as u64,
    );
    if not_copied == 0 {
        Ok(value)
    } else {
        Err(errno(bindings::EFAULT))
    }
}

unsafe fn copy_to_user<T: Copy>(arg: c_ulong, value: &T) -> Result<(), c_long> {
    let not_copied = bindings::_copy_to_user(
        arg as *mut c_void,
        (value as *const T).cast::<c_void>(),
        core::mem::size_of::<T>() as u64,
    );
    if not_copied == 0 {
        Ok(())
    } else {
        Err(errno(bindings::EFAULT))
    }
}

fn fnv1a(mut hash: u64, bytes: &[u8]) -> u64 {
    for byte in bytes {
        hash ^= *byte as u64;
        hash = hash.wrapping_mul(FNV_PRIME);
    }
    hash
}

fn validate_descriptor(descriptor: &SstDescriptor) -> Result<(), c_long> {
    let count = descriptor.chunk_count as usize;
    if descriptor.version != ABI_VERSION
        || count == 0
        || count > MAX_CHUNKS
        || descriptor.total_length == 0
        || descriptor.total_length > MAX_SST_SIZE as u64
        || descriptor.chunk_size != CHUNK_SIZE as u32
        || descriptor.service_id != SERVICE_ID
    {
        return Err(errno(bindings::EINVAL));
    }

    let mut total = 0u64;
    for chunk in descriptor.chunks[..count].iter() {
        if chunk.remote_addr == 0 || chunk.rkey == 0 || chunk.length == 0 || chunk.length as usize > CHUNK_SIZE {
            return Err(errno(bindings::EINVAL));
        }
        total = total
            .checked_add(chunk.length as u64)
            .ok_or_else(|| errno(ERRNO_EOVERFLOW))?;
    }
    if total != descriptor.total_length {
        return Err(errno(bindings::EINVAL));
    }
    Ok(())
}

impl SlsmFile {
    fn connect_peer(&mut self, arg: c_ulong) -> c_long {
        if self.connection.is_some() {
            return errno(ERRNO_EBUSY);
        }
        let req = match unsafe { copy_from_user::<ConnectReq>(arg) } {
            Ok(req) => req,
            Err(ret) => return ret,
        };
        if req.version != ABI_VERSION || req.service_id != SERVICE_ID || req.peer_gid == [0; 16] {
            return errno(bindings::EINVAL);
        }

        let mut gid = ib_gid::default();
        gid.raw = req.peer_gid;
        let mut builder = QueuePairBuilder::new(&self.context);
        builder
            .allow_remote_rw()
            .allow_remote_atomic()
            .set_port_num(RDMA_PORT);
        let prepared = match builder.build_rc() {
            Ok(qp) => qp,
            Err(err) => {
                log::error!("SLSM_EVENT event=connect status=error step=build_qp error={:?}", err);
                return errno(ERRNO_EIO);
            }
        };
        let explorer = Explorer::new_with_gid_index(self.context.get_dev_ref(), GID_INDEX);
        let path = match unsafe { explorer.resolve_inner(req.service_id, RDMA_PORT, gid) } {
            Ok(path) => path,
            Err(err) => {
                log::error!("SLSM_EVENT event=connect status=error step=resolve error={:?}", err);
                return errno(ERRNO_EHOSTUNREACH);
            }
        };
        match prepared.handshake(req.service_id, path) {
            Ok(qp) => {
                self.connection = Some(qp);
                log::info!("SLSM_EVENT event=connect status=ok service_id={}", req.service_id);
                0
            }
            Err(err) => {
                log::error!("SLSM_EVENT event=connect status=error step=handshake error={:?}", err);
                errno(ERRNO_ECONNREFUSED)
            }
        }
    }

    fn register_region(&mut self, arg: c_ulong) -> c_long {
        if self.registered.is_some() {
            return errno(ERRNO_EBUSY);
        }
        let mut req = match unsafe { copy_from_user::<RegisterReq>(arg) } {
            Ok(req) => req,
            Err(ret) => return ret,
        };
        if req.user_addr == 0 || req.length == 0 || req.length > MAX_SST_SIZE as u64 {
            return errno(bindings::EINVAL);
        }

        let gid = match self.context.get_dev_ref().query_gid(RDMA_PORT, GID_INDEX) {
            Ok(gid) => gid,
            Err(_) => return errno(ERRNO_EIO),
        };
        let chunk_count = ((req.length as usize) + CHUNK_SIZE - 1) / CHUNK_SIZE;
        let mut descriptor = SstDescriptor {
            version: ABI_VERSION,
            chunk_count: chunk_count as u32,
            sst_id: req.sst_id,
            total_length: req.length,
            chunk_size: CHUNK_SIZE as u32,
            owner_gid: unsafe { gid.raw },
            service_id: SERVICE_ID,
            ..Default::default()
        };
        let mut chunks = Vec::with_capacity(chunk_count);
        let mut offset = 0usize;
        let mut checksum = FNV_OFFSET_BASIS;

        for index in 0..chunk_count {
            let length = core::cmp::min(CHUNK_SIZE, req.length as usize - offset);
            let mr = match SlsmMemoryRegion::new(self.context.clone(), length) {
                Ok(mr) => mr,
                Err(ret) => {
                    log::error!("SLSM_EVENT event=register status=error step=allocate chunk={}", index);
                    return ret;
                }
            };
            let source = match req.user_addr.checked_add(offset as u64) {
                Some(address) => address,
                None => return errno(ERRNO_EOVERFLOW),
            };
            let not_copied = unsafe {
                bindings::_copy_from_user(
                    mr.mr().get_virt_addr() as *mut c_void,
                    source as *const c_void,
                    length as u64,
                )
            };
            if not_copied != 0 {
                return errno(bindings::EFAULT);
            }
            let bytes = unsafe {
                core::slice::from_raw_parts(mr.mr().get_virt_addr() as *const u8, length)
            };
            checksum = fnv1a(checksum, bytes);
            descriptor.chunks[index] = RemoteChunk {
                remote_addr: unsafe { mr.mr().get_rdma_addr() },
                length: length as u32,
                rkey: mr.mr().rkey().0,
            };
            chunks.push(mr);
            offset += length;
        }

        descriptor.checksum = checksum;
        req.descriptor = descriptor;
        if let Err(ret) = unsafe { copy_to_user(arg, &req) } {
            return ret;
        }
        self.registered = Some(RegisteredRegion {
            _chunks: chunks,
            _descriptor: descriptor,
        });
        log::info!(
            "SLSM_EVENT event=register status=ok sst_id={} bytes={} chunks={} checksum={:#x}",
            descriptor.sst_id,
            descriptor.total_length,
            descriptor.chunk_count,
            descriptor.checksum
        );
        0
    }

    fn wait_for_completion(qp: &QueuePair, expected_wr_id: u64) -> Result<(), c_long> {
        let mut completions: [ib_wc; 1] = [Default::default(); 1];
        loop {
            match qp.poll_send_cq(&mut completions) {
                Ok(polled) if !polled.is_empty() => {
                    if polled[0].get_wr_id() != expected_wr_id {
                        log::error!(
                            "SLSM_EVENT event=fetch status=error step=completion expected_wr_id={} actual_wr_id={}",
                            expected_wr_id,
                            polled[0].get_wr_id()
                        );
                        continue;
                    }
                    if polled[0].status == 0 {
                        return Ok(());
                    }
                    log::error!(
                        "SLSM_EVENT event=fetch status=error step=completion wc_status={}",
                        polled[0].status
                    );
                    return Err(errno(ERRNO_EIO));
                }
                Ok(_) => {}
                Err(err) => {
                    log::error!("SLSM_EVENT event=fetch status=error step=poll error={:?}", err);
                    return Err(errno(ERRNO_EIO));
                }
            }
            kthread::yield_now();
        }
    }

    fn fetch_region(&mut self, arg: c_ulong) -> c_long {
        let mut req = match unsafe { copy_from_user::<FetchReq>(arg) } {
            Ok(req) => req,
            Err(ret) => return ret,
        };
        if let Err(ret) = validate_descriptor(&req.descriptor) {
            return ret;
        }
        if req.user_addr == 0 || req.capacity < req.descriptor.total_length {
            return errno(bindings::EINVAL);
        }
        let qp = match self.connection.as_ref() {
            Some(qp) => qp.clone(),
            None => return errno(ERRNO_ENOTCONN),
        };
        let staging = match SlsmMemoryRegion::new(self.context.clone(), CHUNK_SIZE) {
            Ok(mr) => mr,
            Err(ret) => return ret,
        };

        let timer = KTimer::new();
        let mut checksum = FNV_OFFSET_BASIS;
        let mut offset = 0u64;
        let count = req.descriptor.chunk_count as usize;
        for (index, chunk) in req.descriptor.chunks[..count].iter().enumerate() {
            let length = chunk.length as u64;
            compiler_fence(Ordering::SeqCst);
            if let Err(err) = qp.post_send_read(
                staging.mr(),
                0..length,
                true,
                chunk.remote_addr,
                chunk.rkey,
                index as u64,
            ) {
                log::error!("SLSM_EVENT event=fetch status=error step=post chunk={} error={:?}", index, err);
                return errno(ERRNO_EIO);
            }
            if let Err(ret) = Self::wait_for_completion(&qp, index as u64) {
                return ret;
            }
            compiler_fence(Ordering::SeqCst);
            let bytes = unsafe {
                core::slice::from_raw_parts(staging.mr().get_virt_addr() as *const u8, length as usize)
            };
            checksum = fnv1a(checksum, bytes);
            let destination = match req.user_addr.checked_add(offset) {
                Some(address) => address,
                None => return errno(ERRNO_EOVERFLOW),
            };
            let not_copied = unsafe {
                bindings::_copy_to_user(
                    destination as *mut c_void,
                    staging.mr().get_virt_addr() as *const c_void,
                    length,
                )
            };
            if not_copied != 0 {
                return errno(bindings::EFAULT);
            }
            offset += length;
        }

        req.fetched_length = offset;
        req.checksum = checksum;
        req.elapsed_us = core::cmp::max(timer.get_passed_usec(), 0) as u64;
        if let Err(ret) = unsafe { copy_to_user(arg, &req) } {
            return ret;
        }
        if checksum != req.descriptor.checksum {
            log::error!(
                "SLSM_EVENT event=fetch status=error reason=checksum expected={:#x} actual={:#x}",
                req.descriptor.checksum,
                checksum
            );
            return errno(ERRNO_EBADMSG);
        }
        log::info!(
            "SLSM_EVENT event=fetch status=ok sst_id={} bytes={} chunks={} checksum={:#x} elapsed_us={}",
            req.descriptor.sst_id,
            req.fetched_length,
            req.descriptor.chunk_count,
            req.checksum,
            req.elapsed_us
        );
        0
    }
}

impl Drop for SlsmFile {
    fn drop(&mut self) {
        self.connection = None;
        self.registered = None;
        DEVICE_OPEN.store(false, Ordering::Release);
    }
}

impl FileOperations for SlsmFile {
    fn open(_file: *mut bindings::file) -> linux_kernel_module::KernelResult<Self> {
        if !has_rawio_capability() {
            return Err(kernel_error(ERRNO_EPERM));
        }
        let context = unsafe { GLOBAL_CONTEXT.as_ref().map(Arc::clone) }
            .ok_or_else(|| kernel_error(ERRNO_ENODEV))?;
        if DEVICE_OPEN
            .compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire)
            .is_err()
        {
            return Err(kernel_error(ERRNO_EBUSY));
        }
        Ok(Self {
            context,
            registered: None,
            connection: None,
        })
    }

    fn ioctrl(&mut self, cmd: c_uint, arg: c_ulong) -> c_long {
        if !has_rawio_capability() {
            return errno(ERRNO_EPERM);
        }
        match cmd {
            IOCTL_CONNECT_PEER => self.connect_peer(arg),
            IOCTL_REGISTER_REGION => self.register_region(arg),
            IOCTL_FETCH_REGION => self.fetch_region(arg),
            IOCTL_UNREGISTER_REGION => {
                if self.registered.take().is_some() {
                    log::info!("SLSM_EVENT event=unregister status=ok");
                    0
                } else {
                    errno(ERRNO_ENOENT)
                }
            }
            IOCTL_DISCONNECT => {
                if self.connection.take().is_some() {
                    log::info!("SLSM_EVENT event=disconnect status=ok");
                    0
                } else {
                    errno(ERRNO_ENOTCONN)
                }
            }
            _ => errno(ERRNO_ENOTTY),
        }
    }

    fn mmap(&mut self, _vma: *mut bindings::vm_area_struct) -> c_int {
        -(bindings::EINVAL as c_int)
    }
}

struct Module {
    registration: Option<linux_kernel_module::chrdev::Registration>,
}

impl linux_kernel_module::KernelModule for Module {
    fn init() -> linux_kernel_module::KernelResult<Self> {
        let driver = unsafe { KDriver::create() }.ok_or_else(|| kernel_error(ERRNO_ENODEV))?;
        if driver.devices().len() != 1 {
            log::error!(
                "SLSM_EVENT event=module_init status=error reason=rdma_device_count count={}",
                driver.devices().len()
            );
            return Err(kernel_error(bindings::EINVAL));
        }
        let context = driver.devices()[0]
            .open_context_with_gid_index(GID_INDEX)
            .map_err(|_| kernel_error(ERRNO_ENODEV))?;
        let gid = context
            .get_dev_ref()
            .query_gid(RDMA_PORT, GID_INDEX)
            .map_err(|_| kernel_error(ERRNO_ENODEV))?;
        let server = ReliableConnectionServer::create(&context, RDMA_PORT);
        let cm_server = CMServer::new(SERVICE_ID, &server, context.get_dev_ref())
            .map_err(|_| kernel_error(ERRNO_EADDRINUSE))?;

        unsafe {
            GLOBAL_CONTEXT = Some(context.clone());
            GLOBAL_DRIVER = Some(driver);
            GLOBAL_SERVER = Some(server);
            GLOBAL_CM_SERVER = Some(cm_server);
        }
        let registration = match linux_kernel_module::chrdev::builder(
            linux_kernel_module::cstr!("slsm"),
            0..1,
        )?
        .register_device::<SlsmFile>(linux_kernel_module::cstr!("slsm"))
        .build()
        {
            Ok(registration) => registration,
            Err(err) => {
                unsafe {
                    GLOBAL_CM_SERVER = None;
                    GLOBAL_SERVER = None;
                    GLOBAL_CONTEXT = None;
                    GLOBAL_DRIVER = None;
                }
                return Err(err);
            }
        };

        log::info!(
            "SLSM_EVENT event=module_init status=ok abi={} device={} gid={} gid_index={} service_id={}",
            ABI_VERSION,
            context.get_dev_ref().name(),
            Explorer::gid_to_string(&gid),
            GID_INDEX,
            SERVICE_ID
        );
        Ok(Self {
            registration: Some(registration),
        })
    }
}

impl Drop for Module {
    fn drop(&mut self) {
        self.registration = None;
        unsafe {
            GLOBAL_CM_SERVER = None;
            GLOBAL_SERVER = None;
            GLOBAL_CONTEXT = None;
            GLOBAL_DRIVER = None;
        }
        log::info!("SLSM_EVENT event=module_exit status=ok");
    }
}

#[link_section = ".modinfo"]
#[used]
static SLSM_ABI_MODINFO: [u8; b"slsm_abi=1\0".len()] = *b"slsm_abi=1\0";

#[link_section = ".modinfo"]
#[used]
static SLSM_STAGE_MODINFO: [u8; b"slsm_stage=stage1_rc_poc\0".len()] =
    *b"slsm_stage=stage1_rc_poc\0";

linux_kernel_module::kernel_module!(
    Module,
    author: b"ServerlessX",
    description: b"ServerlessLSM Stage 1 RC SST read PoC",
    license: b"GPL"
);
