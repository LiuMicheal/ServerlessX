use std::env;

fn main() {
    println!("cargo:rerun-if-env-changed=MITOSIS_RDMA_ABI");

    if env::var("MITOSIS_RDMA_ABI").as_deref() == Ok("inbox") {
        println!("cargo:rustc-cfg=BASE_INBOX_RDMA_5_14");
    }
}
