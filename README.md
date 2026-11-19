# Bad Apple

Mainline Linux on the Apple A12 (T8020), tested on the iPhone XS Max.
Tethered boot, root on the NVMe. Hardware notes and postmarketOS are in
[docs](docs/README.md).

Written with help from an LLM, including the reverse engineering of Apple
firmware, so this is not clean-room work. Human reviewed and tested on real
hardware.

## Boot chain

```text
SecureROM, pwned by usbliter8
  -> patched iBSS
  -> device tree, empty trustcache, coprocessor firmware
  -> m1n1, in place of a restore kernelcache
  -> Linux, 16K pages
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
| [germin8](https://forgejo.toe.quest/kiki/germin8) on a Pico 2 W | pwned DFU, booting without a PC |

And an iPhone11,6 IPSW.

## Build

```sh
make deps     # clone the trees, apply patches/
make images IPSW=/path/to/unpacked/ipsw IPSW_FILE=/path/to.ipsw
make boot     # build the rest and boot
```

`make images` runs once. `IPSW_FILE` is the `.ipsw` itself, for the wifi
firmware in its root image.

For the GPU, run `make ticket TICKET_IPSW=/path/to/signed.ipsw` with the phone
in DFU before `make images`. iBoot loads coprocessor firmware only with a
ticket for your phone.

## Run

Put the phone in DFU on the jig and hold BOOTSEL for a second to run only the
exploit. Move the cable to the PC and run `make boot`. The phone shows up as
`05ac:4142` with telnet on `172.16.42.1`; `tools/tsh.py "cmd"` runs one
command. The first USB attach often fails and retries, so give it a minute.

## Status

| Display | Touch | USB | Storage | SMP | cpufreq | Wifi | Bluetooth | GPU | Audio | Sensors | Camera | Cellular |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| :white_check_mark: | :white_check_mark: | :white_check_mark: | :white_check_mark: | :white_check_mark: | :white_check_mark: | :white_check_mark: | :white_check_mark: | :construction: | :construction: | :x: | :x: | :x: |

Full list in [docs](docs/README.md#status).

## License

`GPL-2.0+ OR MIT`, see [LICENSE](LICENSE). `patches/linux-*` are GPL-2.0,
`patches/m1n1-*` MIT. Dependencies are in [NOTICES.md](NOTICES.md).
