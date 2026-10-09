# Service Setup

These examples run the checkout under `/opt/noisy` as the unprivileged `noisy` account. Keep the executable and config readable and the directory searchable by that account; the service does not need write access to the checkout.

Build the program before enabling either service. Review `config.json` and use trusted root URLs. Noisy follows page links, including links to loopback and private-network addresses.

## OpenBSD

Create a service account and check permissions:

```sh
doas useradd -m -s /sbin/nologin noisy
doas chmod 755 /opt/noisy /opt/noisy/noisy
doas chmod 644 /opt/noisy/config.json
```

Install the `rc.d` script and enable the service:

```sh
doas install -o root -g wheel -m 555 examples/rc.d/noisy /etc/rc.d/noisy
doas rcctl enable noisy
doas rcctl start noisy
doas rcctl check noisy
```

Use `doas rcctl stop noisy` to stop it. The service reads `/opt/noisy/config.json` and runs as `noisy`.

## Debian

Create a system account and check permissions:

```sh
sudo adduser --system --group --no-create-home noisy
sudo chmod 755 /opt/noisy /opt/noisy/noisy
sudo chmod 644 /opt/noisy/config.json
```

Install the systemd unit and start it:

```sh
sudo install -m 644 examples/systemd/noisy.service /etc/systemd/system/noisy.service
sudo systemctl daemon-reload
sudo systemctl enable --now noisy
sudo systemctl status noisy
```

Follow logs with `journalctl -u noisy -f`; stop the service with `sudo systemctl stop noisy`.
