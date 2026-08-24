# SLSM baseline bring-up (2026-08-24)

This note records the minimum deployment gates for the first four SLSM
baseline steps. All software installation, compilation, HDFS activity, and
RocksDB workloads ran inside the two `slsm` Guests. The physical hosts were
not used for the LSM workload.

## Guest roles

| Guest | Role in this stage | Result |
| --- | --- | --- |
| `mem01` Guest | Rocks-Local client/compaction process and HDFS client | Passed build, write, local compaction, reopen/read |
| `mem02` Guest | Single-node HDFS NameNode + DataNode | Passed HDFS health and cross-Guest client test |

Both Guests reported Rocky Linux 9.8, kernel `5.14.0-687.10.1.el9_8.0.1`,
16 vCPUs, and the same CaaS-LSM source revision:

`asu-idi/CaaS-LSM@072ce2d18b019c18b386341e2a09c1fd36f3ab22`

The source tree itself was not patched. Build directories and Hadoop data
remain outside this repository.

## Gates

### 1. Guest preflight

Passed on both Guests: compiler/CMake, Java 8 and Java 11 runtimes, protobuf,
gRPC, compression libraries, and `liburing` development packages were
available. The two Guest network paths could reach one another.

### 2. Dependencies and source

The CaaS-LSM tree was present at the fixed revision on both Guests. The
Hadoop 3.3.1 archive was checksum-verified before being unpacked as a client
on both Guests. Java 8 was selected for the RocksDB HDFS plugin because this
upstream CMake file expects the Java 8-style `jre/lib/amd64/server` path.

Both Guests built the HDFS-enabled benchmark successfully:

```text
cmake -S . -B build-hdfs \
  -DCMAKE_BUILD_TYPE=Release \
  -DWITH_HDFS=ON \
  -DROCKSDB_BUILD_SHARED=OFF
cmake --build build-hdfs --target db_bench -j8
```

The resulting `db_bench` SHA-256 was identical on both Guests:

`c0dba04edd70507bab0f6a0a82297e7f45b5f01fb24eac763972d412727f1d9b`

### 3. Minimal HDFS

The NameNode and DataNode ran on the `mem02` Guest. The `mem01` Guest used
the Hadoop client to create `/slsm-smoke`, upload a small file, read it back,
and run `fsck`. The result was `HEALTHY`, with one live DataNode and no
missing or corrupt blocks.

The HDFS service is deliberately a single-node smoke configuration for this
stage, with replication factor 1. It is not a production or fault-tolerance
claim.

### 4. Rocks-Local correctness smoke

The workload ran on the `mem01` Guest with the HDFS filesystem hosted by the
`mem02` Guest. The database used an HDFS URI supplied through `--env_uri` and
an isolated test directory. The remote-compaction flag was explicitly false;
no CSA or ProCP process was started.

Workload parameters were intentionally small:

```text
fillrandom, 5,000 operations, 512-byte values
write_buffer_size=524288
level0_file_num_compaction_trigger=2
max_background_compactions=2
compression_type=none
allow_remote_compaction=false
```

The write phase returned zero. Five input SSTs were visible in HDFS. A
separate `compactall` phase returned zero and reported one compaction over
five input files; the HDFS directory then contained one output SST and a new
Manifest. Finally, `db_bench` reopened the same directory with
`--use_existing_db=true` and `--verify_checksum=true`; both random-read and
sequential-read reopen gates returned zero.

The reopen commands exported Hadoop's `CLASSPATH` in addition to the native
library paths. A preliminary read attempt without that variable was not
counted as evidence; the correctly configured rerun passed.

This establishes the narrow claim that RocksDB's local compaction process can
write, compact, publish a Manifest, and reopen data while its files are in
the Guest-hosted HDFS smoke setup. It does not establish remote compaction,
RDMA performance, elasticity, or a CaaS-LSM service result.

## Boundary and follow-up

- No `csa_server` or `procp_server` was started in this stage.
- No LSM workload, HDFS daemon, or package was installed on the physical
  `gpu01`/`gpu02` hosts.
- The upstream tree contains defaults for its remote-compaction endpoints;
  they were not used because remote compaction was disabled. They must be
  parameterized inside the Guests before bringing up Disaggre-RocksDB or
  CaaS-LSM remote workers.
- The next stage is a separate Disaggre-RocksDB/CaaS-LSM service gate. It must
  reuse these two Guests and keep the Rocks-Local run as the local baseline.
