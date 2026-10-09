# Noisy

Noisy is a small program that crawls randomly selected HTTP and HTTPS links from configured root URLs. DNS queries happen as a side effect of hostname resolution; Noisy does not generate independent DNS traffic.

This is a C fork of [1tayH/noisy](https://github.com/1tayH/noisy), originally written in Python. The port was motivated by the runtime overhead and instability observed when running the Python process as a long-lived service. The original Python source remains in this repository as a behavior reference.

32-bit compatibility is retained alongside 64-bit builds. Tested on macOS 27 Golden Gate and OpenBSD 7.8. Build instructions are provided below for macOS, OpenBSD, Debian, and Slackware.

## Requirements

A C compiler, `make`, `pkg-config`, libcurl, and Jansson development files are required. Install the development packages for the architecture you intend to build, including for 32-bit builds.

## Build

### macOS

Install dependencies with Homebrew:

```sh
brew install curl jansson pkg-config
```

Clone Git Repo

```sh
git clone https://github.com/nker150/cnoisy
```

Compile from source in the directory of the cloned Git repo:

```sh
make
```

### OpenBSD

The commands below are assuming root privileges.

Install libcurl and Jansson:

```sh
pkg_add curl jansson
```

Clone Git Repo

```sh
cd /opt; git clone https://github.com/nker150/cnoisy; cd cnoisy
```

Build with BSD make:

```sh
make
```

If `pkg-config` is unavailable, install `pkgconf` first.

### Debian

Install the compiler, pkg-config, and development packages:

```sh
sudo apt update
sudo apt install build-essential pkg-config libcurl4-openssl-dev libjansson-dev
```

Clone Git Repo

```sh
cd /opt; git clone https://github.com/nker150/cnoisy; cd cnoisy
```

Then build:

```sh
make
```

## Run

The default configuration is in `config.json`:

```sh
./noisy --config config.json
```

The program accepts `--config` (`-c`), `--log` (`-l`), `--timeout` (`-t`), and `--help` (`-h`). The JSON file remains the normal source of crawler settings; a nonzero `--timeout` overrides its timeout. Only HTTP and HTTPS URLs are accepted, including redirect destinations.

Run the local integration tests with `make test`; the test harness requires Python 3.

## Optional System Installation

Install the executable and man page under `/usr/local`:

```sh
sudo make install
```

Use `doas make install` on OpenBSD. The default manual path is `/usr/local/man/man1/noisy.1`. On Debian, the share-man convention can be selected explicitly:

```sh
sudo make install MANDIR=/usr/local/share/man/man1
```

`PREFIX`, `BINDIR`, `MANDIR`, and `DESTDIR` can be overridden. Installation includes only the executable and man page, not the configuration or a service. See [noisy(1)](noisy.1) for the complete CLI and configuration reference.

## Services

OpenBSD `rc.d` and Debian systemd examples are documented in [examples/README.md](examples/README.md). Both run the checkout from `/opt/noisy` as an unprivileged service account.

## History and Attribution

The original Python program was written by Itay Hury. This repository is a C fork intended to reduce the overhead and improve the reliability of long-running service use.

## License

This project is licensed under the GNU GPLv3 License; see [LICENSE](LICENSE).
