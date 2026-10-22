# Bad Apple

Mainline Linux on the Apple A12 (T8020), tested on the iPhone XS Max.

The boot is tethered and reads nothing off the NAND, so the NAND can hold ext4.
Hardware notes are in [docs](docs/README.md).

## Boot chain

```text
SecureROM, pwned by usbliter8
  -> patched iBSS
  -> device tree and an empty trustcache
  -> m1n1, in place of a restore kernelcache
  -> Linux, 16K pages, initramfs
```

## Dependencies

| Host | For |
|---|---|
| `clang`, `ld.lld` | kernel and m1n1 |
| Rust with `rust-src` | m1n1 |
| `dtc`, `cpp` | `make check` |
| `python3` | `tools/` |
| `git`, `curl`, `unzip` | `make deps`, `make initramfs` |
| `ipsw` | the IPSW and img4 |
| `irecovery` | recovery mode |
| [usbliter8ra1n-patch](https://github.com/Trussh/usbliter8ra1n-patch) | pwned DFU |

And an iPhone11,6 IPSW.

## Build

```sh
make deps     # clone the trees and apply patches/
make images IPSW=/path/to/unpacked/ipsw IPSW_FILE=/path/to.ipsw
make boot     # builds the rest and boots
```

`make images` runs once. `IPSW_FILE` is the `.ipsw` itself, since the wifi
firmware is in its root image.

For the GPU coprocessor, run `make ticket TICKET_IPSW=/path/to/signed.ipsw`
with the phone in DFU before `make images`. It fetches a ticket for your phone,
which iBoot needs to load coprocessor firmware.

## Run

Pwn the phone, then run `make boot`. It shows up as `05ac:4142` with telnet
on `172.16.42.1`. `tools/tsh.py "cmd"` runs one command. The first USB attach
often fails and the phone retries, so give it a minute.

## Status

| Display | Touch | USB | Storage | SMP | cpufreq | Wifi | Bluetooth | Audio | Cellular | GPU |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| :white_check_mark: | :white_check_mark: | :white_check_mark: | :white_check_mark: | :white_check_mark: | :white_check_mark: | :white_check_mark: | :white_check_mark: | :x: | :x: | :x: |

Full list in [docs](docs/README.md#status).

## License

`GPL-2.0+ OR MIT`, see [LICENSE](LICENSE). `patches/linux-*` are GPL-2.0,
`patches/m1n1-*` MIT. Dependencies are in [NOTICES.md](NOTICES.md).
