use alloc::sync::Arc;
use core::ptr::NonNull;
use rdma_shim::bindings::*;
use rdma_shim::{Error, KernelResult};

use crate::{device::DeviceRef, ControlpathError};

/// Context abstract the protection domain and memory registeration
/// in the kernel space
#[allow(dead_code)]
#[derive(Debug)]
pub struct Context {
    inner_device: DeviceRef,
    gid_index: usize,
    #[cfg(feature = "kernel")]
    gid_attr: NonNull<ib_gid_attr>,
    #[cfg(feature = "kernel")]
    lkey: u32,
    #[cfg(feature = "kernel")]
    rkey: u32,

    #[cfg(feature = "user")]
    ctx: NonNull<ibv_context>,

    pd: NonNull<ib_pd>,
}

unsafe impl Send for Context {}
unsafe impl Sync for Context {}

pub type ContextRef = Arc<Context>;

#[derive(Debug)]
pub struct AddressHandler {
    _ctx: ContextRef,
    inner: NonNull<ib_ah>,
}

#[cfg(feature = "kernel")]
const IB_PD_UNSAFE_GLOBAL_RKEY: u32 = 0x01;

impl Context {
    pub fn new(dev: &DeviceRef) -> Result<ContextRef, ControlpathError> {
        Self::new_with_gid_index(dev, 0)
    }

    pub fn new_with_gid_index(
        dev: &DeviceRef,
        gid_index: usize,
    ) -> Result<ContextRef, ControlpathError> {
        #[cfg(feature = "kernel")]
        {
            let mr_flags = ib_access_flags::IB_ACCESS_LOCAL_WRITE
                | ib_access_flags::IB_ACCESS_REMOTE_READ
                | ib_access_flags::IB_ACCESS_REMOTE_WRITE
                | ib_access_flags::IB_ACCESS_REMOTE_ATOMIC;
            Self::new_from_flags_with_gid_index(dev, mr_flags as i32, gid_index)
        }

        #[cfg(feature = "user")]
        {
            Self::new_from_flags_with_gid_index(dev, 0, gid_index)
        }
    }

    #[cfg(feature = "user")]
    pub fn raw_ptr(&self) -> &NonNull<ibv_context> { 
        &self.ctx
    }

    #[cfg(feature = "kernel")]
    pub fn raw_ptr(&self) -> &NonNull<ib_device> { 
        self.get_dev_ref().raw_ptr()
    }    

    #[allow(unused_variables)]
    pub fn new_from_flags(dev: &DeviceRef, mr_flags: i32) -> Result<ContextRef, ControlpathError> {
        Self::new_from_flags_with_gid_index(dev, mr_flags, 0)
    }

    #[allow(unused_variables)]
    pub fn new_from_flags_with_gid_index(
        dev: &DeviceRef,
        mr_flags: i32,
        gid_index: usize,
    ) -> Result<ContextRef, ControlpathError> {
        #[cfg(feature = "kernel")]
        {
            let gid_attr = NonNull::new(unsafe {
                bd_rdma_get_gid_attr(dev.raw_ptr().as_ptr(), 1, gid_index as _)
                    as *mut ib_gid_attr
            })
            .ok_or(ControlpathError::ContextError("gid_attr", Error::EINVAL))?;

            // first allocate a PD
            let pd = match NonNull::new(unsafe {
                ib_alloc_pd(dev.raw_ptr().as_ptr(), IB_PD_UNSAFE_GLOBAL_RKEY)
            }) {
                Some(pd) => pd,
                None => {
                    unsafe { bd_rdma_put_gid_attr(gid_attr.as_ptr()) };
                    return Err(ControlpathError::ContextError("pd", Error::EAGAIN));
                }
            };

            let (lkey, rkey) = unsafe {
                let pd = pd.as_ref();
                (pd.local_dma_lkey, pd.unsafe_global_rkey)
            };

            Ok(Arc::new(Self {
                inner_device: dev.clone(),
                gid_index,
                gid_attr,
                lkey,
                rkey,
                pd,
            }))
        }

        #[cfg(feature = "user")]
        {
            let ctx = NonNull::new(unsafe { ibv_open_device(dev.raw_ptr().as_ptr()) })
                .ok_or(ControlpathError::ContextError("ibv_context", Error::EAGAIN))?;

            let pd = NonNull::new(unsafe { ibv_alloc_pd(ctx.as_ptr()) })
                .ok_or(ControlpathError::ContextError("pd", Error::EAGAIN))?;
            Ok(Arc::new(Self {
                inner_device: dev.clone(),
                gid_index,
                ctx,
                pd,
            }))
        }
    }

    #[inline]
    pub fn gid_index(&self) -> usize {
        self.gid_index
    }

    #[cfg(feature = "kernel")]
    #[inline]
    pub fn gid_attr(&self) -> *const ib_gid_attr {
        self.gid_attr.as_ptr() as *const ib_gid_attr
    }

    #[cfg(feature = "kernel")]
    /// Get the lkey of this context
    pub fn lkey(&self) -> u32 {
        self.lkey
    }

    #[cfg(feature = "kernel")]
    /// Get the rkey of this context
    pub fn rkey(&self) -> u32 {
        self.rkey
    }

    pub fn get_dev_ref(&self) -> &DeviceRef {
        &self.inner_device
    }

    pub fn get_pd(&self) -> &NonNull<ib_pd> {
        &self.pd
    }

    /// Create an address handler of this context
    ///
    /// - port_num: local port number
    /// - gid_idx : local gid index
    ///
    /// - lid : remote lid
    /// - gid : remote gid
    ///
    #[allow(unused_variables)] // gid_idx may not be used in the kernel
    pub fn create_address_handler(
        self: &Arc<Self>,
        port_num: u8,
        gid_idx: usize,
        lid: u32,
        gid: ib_gid,
    ) -> Result<AddressHandler, ControlpathError> {
        let mut ah_attr: rdma_ah_attr = Default::default();

        #[cfg(feature = "kernel")]
        {
            #[cfg(BASE_INBOX_RDMA_5_14)]
            {
                ah_attr = self.resolve_address_attr(port_num, gid_idx, gid)?;
            }

            #[cfg(not(BASE_INBOX_RDMA_5_14))]
            {
                ah_attr.type_ = rdma_ah_attr_type::RDMA_AH_ATTR_TYPE_IB;

                unsafe {
                    bd_rdma_ah_set_dlid(&mut ah_attr, lid as u32);
                }

                ah_attr.sl = 0;
                ah_attr.port_num = port_num as _;
                ah_attr.grh.sgid_index = gid_idx as _;
                ah_attr.grh.flow_label = 0;
                ah_attr.grh.hop_limit = 255;

                unsafe {
                    ah_attr.grh.dgid.global.subnet_prefix = gid.global.subnet_prefix;
                    ah_attr.grh.dgid.global.interface_id = gid.global.interface_id;
                }
            }
        }

        #[cfg(feature = "user")]
        {
            ah_attr.port_num = port_num;
            ah_attr.is_global = 1;
            ah_attr.dlid = lid as _;
            ah_attr.sl = 0;
            ah_attr.src_path_bits = 0;

            ah_attr.grh.dgid = gid;
            ah_attr.grh.flow_label = 0;
            ah_attr.grh.hop_limit = 255;
            ah_attr.grh.sgid_index = gid_idx as _;
        }

        let ptr = unsafe { rdma_create_ah_wrapper(self.get_pd().as_ptr(), &mut ah_attr as _) };

        #[cfg(all(feature = "kernel", BASE_INBOX_RDMA_5_14))]
        unsafe {
            rdma_destroy_ah_attr(&mut ah_attr as *mut _);
        }

        if unsafe { ptr_is_err(ptr as _) } > 0 {
            Err(ControlpathError::CreationError(
                "AddressHandler",
                Error::EAGAIN,
            ))
        } else {
            Ok(AddressHandler {
                _ctx: self.clone(),
                // we have checked the pointer!
                inner: unsafe { NonNull::new_unchecked(ptr) },
            })
        }
    }
}

#[cfg(all(feature = "kernel", BASE_INBOX_RDMA_5_14))]
impl Context {
    pub(crate) fn resolve_address_attr(
        &self,
        port_num: u8,
        gid_idx: usize,
        gid: ib_gid,
    ) -> Result<rdma_ah_attr, ControlpathError> {
        let gid_attr = NonNull::new(unsafe {
            bd_rdma_get_gid_attr(
                self.inner_device.raw_ptr().as_ptr(),
                port_num as _,
                gid_idx as _,
            ) as *mut ib_gid_attr
        })
        .ok_or(ControlpathError::ContextError("gid_attr", Error::EINVAL))?;

        let mut path: sa_path_rec = Default::default();
        let ret = unsafe {
            bd_resolve_roce_path(
                self.inner_device.raw_ptr().as_ptr(),
                port_num as _,
                gid_idx as _,
                &gid as *const _,
                0,
                &mut path as *mut _,
            )
        };
        if ret != 0 {
            unsafe { bd_rdma_put_gid_attr(gid_attr.as_ptr()) };
            return Err(ControlpathError::CreationError(
                "RoCE route",
                Error::from_kernel_errno(ret),
            ));
        }

        let mut ah_attr: rdma_ah_attr = Default::default();
        let ret = unsafe {
            ib_init_ah_attr_from_path(
                self.inner_device.raw_ptr().as_ptr(),
                port_num as _,
                &mut path as *mut _,
                &mut ah_attr as *mut _,
                gid_attr.as_ptr(),
            )
        };
        unsafe { bd_rdma_put_gid_attr(gid_attr.as_ptr()) };
        if ret != 0 {
            return Err(ControlpathError::CreationError(
                "RoCE address handle",
                Error::from_kernel_errno(ret),
            ));
        }
        Ok(ah_attr)
    }
}

impl Context {
    /// get device attr
    pub fn get_device_attr(&self) -> KernelResult<ib_device_attr> {
        #[cfg(feature = "kernel")]
        {
            self.inner_device.get_device_attr()
        }

        #[cfg(feature = "user")]
        {
            let mut dev_attr: ib_device_attr = Default::default();
            let err = unsafe { ibv_query_device(self.ctx.as_ptr(), &mut dev_attr as _) };

            if err != 0 {
                return Err(Error::from_kernel_errno(err));
            }
            Ok(dev_attr)
        }
    }

    #[inline]
    pub fn get_port_attr(&self, port_id: u8) -> KernelResult<ib_port_attr> {
        #[cfg(feature = "kernel")]
        {
            self.inner_device.get_port_attr(port_id)
        }

        #[cfg(feature = "user")]
        {
            let mut port_attr: ib_port_attr = Default::default();
            let err = unsafe { ibv_query_port(self.ctx.as_ptr(), port_id, &mut port_attr as _) };

            if err != 0 {
                return Err(Error::from_kernel_errno(err));
            }
            Ok(port_attr)
        }
    }

    /// query the gid of a specific port
    pub fn query_gid(&self, port_id: u8, gid_idx: usize) -> KernelResult<ib_gid> {
        #[cfg(feature = "kernel")]
        {
            self.inner_device.query_gid(port_id, gid_idx)
        }

        #[cfg(feature = "user")]
        {
            let mut gid: ib_gid = Default::default();
            let err =
                unsafe { ibv_query_gid(self.ctx.as_ptr(), port_id, gid_idx as _, &mut gid as _) };
            if err != 0 {
                return Err(Error::from_kernel_errno(err));
            }
            Ok(gid)
        }
    }
}

impl AddressHandler {
    pub fn raw_ptr(&self) -> &NonNull<ib_ah> {
        &self.inner
    }
}

impl Drop for AddressHandler {
    fn drop(&mut self) {
        unsafe {
            rdma_destroy_ah(self.inner.as_ptr());
        }
    }
}

impl Drop for Context {
    fn drop(&mut self) {
        unsafe {
            #[cfg(feature = "kernel")]
            bd_rdma_put_gid_attr(self.gid_attr());
            ib_dealloc_pd(self.pd.as_ptr());
        }
    }
}

#[cfg(feature = "user")]
#[cfg(test)]
mod tests {
    #[test]
    fn device_attr() {
        let ctx = crate::UDriver::create()
            .expect("failed to query device")
            .devices()
            .into_iter()
            .next()
            .expect("no rdma device available")
            .open_context()
            .expect("failed to create RDMA context");

        ctx.get_device_attr().unwrap();
        let port_lid = ctx.get_port_attr(1).unwrap().lid;
        let gid = ctx.query_gid(1, 0).unwrap();

        ctx.create_address_handler(1, 0, port_lid as _, gid)
            .unwrap();
    }
}
