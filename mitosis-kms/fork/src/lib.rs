#![no_std]

extern crate alloc;

use mitosis::KRdmaKit;
use mitosis::linux_kernel_module;
use mitosis::log;
use mitosis::os_network;
use mitosis::core_syscall_handler::*;
use mitosis::startup::{end_instance, start_instance};
use mitosis::syscalls::*;

use mitosis_macros::declare_module_param; 

declare_module_param!(mac_id, u64);

/// The module corresponding to the kernel module lifetime
#[allow(dead_code)]
struct Module {
    service: SysCallsService<MitosisSysCallHandler>,
}

use os_network::block_on;

impl linux_kernel_module::KernelModule for Module {
    /// Called by the kernel upon the kernel module creation
    fn init() -> linux_kernel_module::KernelResult<Self> {
        let id = mac_id::read();
        log::info!("Remote fork kernel module assigned ID={}", id);

        // Currently, we use a default configuration of MITOSIS
        let mut config: mitosis::Config = Default::default();

        config
            .set_num_nics_used(1)
            .set_rpc_threads(2)
            .set_machine_id(id as usize);

        #[cfg(feature = "legacy_dct")]
        config.set_init_dc_targets(12);

        assert!(start_instance(config.clone()).is_some());
        let service = SysCallsService::<MitosisSysCallHandler>::new()?;
        log::info!(
            "MITOSIS_EVENT version=1 event=module_init status=ok machine_id={} control={} data={}",
            id,
            mitosis::CONTROL_TRANSPORT,
            mitosis::DATA_TRANSPORT
        );

        Ok(Self { service })
    }
}

impl Drop for Module {
    fn drop(&mut self) {
        end_instance();
        log::info!(
            "MITOSIS_EVENT version=1 event=module_exit status=ok control={} data={}",
            mitosis::CONTROL_TRANSPORT,
            mitosis::DATA_TRANSPORT
        );
    }
}

#[cfg(feature = "use_rc")]
#[link_section = ".modinfo"]
#[used]
static MITOSIS_TRANSPORT_MODINFO: [u8; b"mitosis_transport=use_rc\0".len()] =
    *b"mitosis_transport=use_rc\0";

#[cfg(feature = "use_rc")]
#[link_section = ".modinfo"]
#[used]
static MITOSIS_CONTROL_MODINFO: [u8; b"mitosis_control=ud\0".len()] =
    *b"mitosis_control=ud\0";

#[cfg(feature = "use_rc")]
#[link_section = ".modinfo"]
#[used]
static MITOSIS_DATA_MODINFO: [u8; b"mitosis_data=rc\0".len()] = *b"mitosis_data=rc\0";

#[cfg(feature = "use_rc")]
#[link_section = ".modinfo"]
#[used]
static MITOSIS_STAGE_MODINFO: [u8; b"mitosis_stage=serverlesspd_stage1\0".len()] =
    *b"mitosis_stage=serverlesspd_stage1\0";

#[cfg(all(feature = "use_rc", feature = "cow"))]
#[link_section = ".modinfo"]
#[used]
static MITOSIS_BUILD_FEATURES_MODINFO: [u8; b"build_features=cow,use_rc\0".len()] =
    *b"build_features=cow,use_rc\0";

#[cfg(all(feature = "use_rc", not(feature = "cow")))]
#[link_section = ".modinfo"]
#[used]
static MITOSIS_BUILD_FEATURES_MODINFO: [u8; b"build_features=use_rc\0".len()] =
    *b"build_features=use_rc\0";

#[cfg(feature = "use_rc")]
linux_kernel_module::kernel_module!(
    Module,
    author: b"xmm",
    description: b"Mitosis ServerlessPD Stage 1 kernel rfork (UD control, RC data)",
    license: b"GPL"
);

#[cfg(feature = "legacy_dct")]
linux_kernel_module::kernel_module!(
    Module,
    author: b"xmm",
    description: b"The kernel module for exposing system calls.",
    license: b"GPL"
);
