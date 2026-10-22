# NVMe

ANS2 with SART and an RTKit mailbox. Reads about 467 MB/s, writes 132 MB/s.

```sh
mount -t ext4 /dev/nvme0n1 /mnt
```

| what | address |
|---|---|
| ASC wrapper | 0x277400000 |
| mailbox | 0x277408000, T8015 type |
| coprocessor run bit | 0x277400044 bit 4 |
| NVMe registers | 0x27acc0000 |
| SART | 0x27ac50000, version 1 |

What it needed:

1. Write `AQA`, `ASQ` and `ACQ` before starting the coprocessor. iBoot leaves
   its admin queue programmed.
2. Keep `ps_pcie_ref`, `ps_apcie`, `ps_apcie_gp` and `ps_apcie_st` on.
3. NVMMU without a linear SQ: `has_lsq_nvmmu` split into two flags.
4. Set Features before Create IO CQ.
5. The NVMMU indexes by ring slot, not tag. Invalidation uses the tag.

Interrupts come in descending pairs (365, 364, 367, 366): not empty, empty.
NVMe is 371. ANS keeps old crash logs across resets, so check the time.
