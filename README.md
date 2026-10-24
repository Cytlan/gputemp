# gputemp

NVIDIA GPU temperature monitor (core, junction/hotspot, memory), plus
optional `--target` fan control and a `--find` mode for identifying a
physical card.

This is a C99 source reconstruction of the prebuilt, source-less `gputemp`
binary published at <https://github.com/r0ver2/gputemp>. It was rebuilt by
hand from a disassembly of the shipped x86-64 ELF
(sha256 `2b9dc9162f20c5a18b1631b9c605f025a97bff7340bb481be96c913c9a3b3554`)
and checked against it with a fake NVML library. The original and this
build gave the same output, error messages and exit codes across about 50
option, error-path and root/non-root runs.

## Build

    make            # or: cc -std=c99 -O2 -o gputemp gputemp.c -ldl

The only runtime dependency is the NVIDIA driver's `libnvidia-ml.so.1`,
which is loaded with `dlopen()`. No CUDA or NVML headers are needed.

## Usage

    gputemp [--json] [--watch] [--interval SECONDS]
            [--target TEMP] [--min PERCENT] [--find ID]

Run `gputemp --help` for details.

## How it works / safety notes

* Core temperature, fan, power: standard NVML calls.
* Memory temperature: NVML field `NVML_FI_DEV_MEMORY_TEMP` where supported.
  As root, it falls back to:
  * Ampere/Ada: reading BAR0 register `0xE2A8` through
    `/sys/bus/pci/devices/<bdf>/resource0` (mapped read-only).
  * Blackwell: allocating an RM `RM_USER_SHARED_DATA` (0xDE) object through
    `/dev/nvidiactl`, mapping it read-only, and scanning it for a plausible
    reading.
* Junction temperature (root only): read-only BAR0 reads of
  `0x2046C`/`0x20470` (Ampere/Ada) or `0xAD0A90`-`0xAD0AA0` (Blackwell).
  These registers are undocumented, so treat the result as best-effort.
* `--target` and `--find` change fan speeds through NVML. Automatic fan
  control is restored on Ctrl-C or SIGTERM, but **not** on SIGKILL or a
  crash. If that happens, run `nvidia-smi` or reboot to get the default
  fan curve back.

The program makes no network calls, starts no other programs, and writes
no files.

## License

GPL-2.0-only. See [LICENSE](LICENSE).
