# Nova-LSM two-node reproduction

Date: 2026-08-21

This is a small, functional reproduction of the Nova-LSM LTC--StoC path for
the SLSM project. It is not a paper-scale performance reproduction.

## Environment

- mem01 Guest: LTC, server id `0`
- mem02 Guest: StoC, server id `1`
- RDMA transport: RoCE IPv4 GID selected by the local Guest RDMA device
- workload: 10,000 integer keys, 1 KiB fixed values
- source tree: `/home/liumx/nova-lsm-baseline-20260821`
- configuration: `config/nova-2node-small-10000`

The configuration has one fragment, key range `[0, 10000)`, and assigns the
fragment to LTC `0` with StoC `1` as its metadata/data destination.

## Verified baseline

Run with `enable_lookup_index=false`, `enable_range_index=false`,
`enable_subrange=false`, and `major_compaction_type=no`.

- both RDMA QPs connected successfully;
- mem01 reported `Completed loading data 10000`;
- mem02 stored multiple non-zero `.ldb` and `.ldb-meta` files;
- the largest observed SST data files were about 1.06 MiB;
- no `Processed by both client and server` completion assertion occurred.

The detailed run evidence is on the Guests under
`/home/liumx/nova-repro-fix2-20260821`.

## Local fixes kept in this branch

- select the IPv4 RoCE GID instead of assuming GID index zero;
- route an RDMA completion to the handler for the local LTC or StoC role and
  only fall back to the other handler if it did not consume the completion;
- retry `sem_wait` after `EINTR`;
- seed a fresh database's range-index state before the first memtable rotation.

## Follow-up status

Fresh two-node runs with `enable_lookup_index=true` or
`enable_range_index=true` exited with `SIGABRT` before a verifiable
`Complete Load` result could be established. They produced no non-zero SST
evidence. These switches remain follow-up work and are deliberately not
presented as verified results in this small-paper baseline.

## Reproduction command shape

Start the StoC (`server_id=1`) and then the LTC (`server_id=0`) with the same
flags, changing only `enable_load_data` and the server id. The essential role
flag is:

```text
--number_of_ltcs=1
```

Use fresh `db_path` and `stoc_files_path` directories for each run. Do not
reboot or modify the physical hosts as part of this experiment.
