# vveye-p2p-client

A Linux CLI wrapper for the [VVEye/T2u client SDK](http://www.vveye.com/SDK_Download.html). It connects to the specified
relay/signalling server, exposes a local TCP port and forwards its traffic to
an arbitrary TCP port of a remote device identified by UID.

## Important limitation

VVEye does not publish a Linux T2u client library, so this project uses the
x86_64 `libt2u.so` from the Android SDK.  That library expects the
Android/Bionic `addrinfo` layout, so the build produces a small compatibility
shim (`build/liblog.so`) that is loaded before it.

`vendor/fetch-libt2u.sh` downloads the SDK archive, verifies its SHA-256 values,
and installs both the x86_64 library and SDK header.

To use an SDK obtained separately, copy its `libs/x86_64/libt2u.so` to
`vendor/libt2u.so` and `libt2u.h` to `include/libt2u.h`.

## Build

Prerequisites:

- a C compiler and `make`
- `curl`
- `bsdtar`, `7z`, or `7zz`
- `sha256sum`, `shasum`, or `openssl`

```sh
make
make fetch-sdk
```

## Use

```sh
./vveye-p2p-client \
  --server hostname \
  --uid DEVICE_UID \
  --remote-port 80 \
  --local-port 8080
```

The mapped service is then reachable at `127.0.0.1:8080`.  Pass `--local-port
0` to let the SDK pick a free port.

`./vveye-p2p-client` is a launcher that sets the runtime library search path.
The `build/vveye-p2p-client` ELF can also be run directly when `LD_LIBRARY_PATH`
contains both `build/` and `vendor/`.

Run `./vveye-p2p-client --help` for the full option list.  Common options:

```text
--server HOST             relay/signalling server (required)
--server-port PORT        relay server port (default 8000)
--remote-port PORT        destination port on the device
--local-port PORT         local listener, 0 = SDK-selected
--password PASSWORD       use the password-capable v3 API
--remote-ip ADDRESS       device-side destination (default 127.0.0.1 with v3)
--skip-query              skip the preliminary online query
--timeout SECONDS         per-phase connection timeout (default 60)
--verbose                 print mapping status while connecting; refresh stats once per minute
```

`VV_P2P_PASSWORD` and `VV_P2P_SERVER_KEY` supply secrets without putting them on
the command line.

## Diagnostics and troubleshooting

Run with `--verbose` to print the mapping status on each poll while connecting.
Once connected, statistics are refreshed once per minute; without `--verbose`,
status is printed only when it changes.

Mapping status values: `1` connected, `0` connecting, `-1` unknown device or
mapping, `-2` P2P failure, `-3` interrupted, `-4` device offline, `-5` password
rejected.

The client creates one mapping and waits up to `--timeout` for it to connect.
A run that reaches a connected mapping exits `0` on Ctrl-C; all
connection/setup failures (including timeout) exit `1`.

If the loader reports `libc.so`, `libm.so`, `libdl.so`, `libstdc++.so`, or
`liblog.so` errors, run `make fetch-sdk` (or ensure the vendor runtime aliases
and `build/liblog.so` are present).  `--no-compat` is only appropriate for a
native Linux build of the SDK.
