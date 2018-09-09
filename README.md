# snaps

Take secure snapshots of remote locations with ease.

Features:
* Securely backup servers that you don't fully trust
* Easy to read and write config file format
* Privilege separated, pledged and chrooted

Status: **public beta**

The main improvements of snaps over other backup software are better security
through isolation, a reduced attack surface, and easier to configure and
maintain through a simple config file format and because only one unprivileged
user is needed in order to obtain proper isolation of backups from different
servers.

snaps is inspired by [rsnapshot] and relies on [hrsync], a hardened version of
[rsync].


## Requirements

snaps itself must be installed on an [OpenBSD] host since it requires
[pledge(2)]. Each remote backup location must only have [rsync] (or [hrsync])
installed.


## Installation

Compile and install snaps:

```sh
$ git clone https://github.com/timkuijsten/snaps.git
$ cd snaps
$ make
$ doas make install
```


## Usage

A couple of things need to be done the first time right after snaps is installed:
1. Create a config file with all remote backup locations
2. Create an unprivileged user and ssh key pair
3. Each remote location must allow incoming ssh access
4. Snaps must be run manually or scheduled via cron

### 1. Create a config file with all remote backup locations

Create a config file with the right permissions. You can use the installed
example as a starting point:

```sh
$ doas install -m 0640 /etc/examples/snaps.conf /etc/snaps.conf
```

After a basic config file is in place, adjust it to your needs. In the following
example only two locations are being backed up, namely `bar.example.com:/home`
and `foo.nl:/srv`. All snapshots are being stored under a directory in
`/srv/snaps`. Six daily, three weekly and three monthly snapshots are being
retained.

```
$ cat /etc/snaps.conf
root /srv/snaps

# local user
user _snaps

daily 6
weekly 3
monthly 3

backup bar.example.com:/home
backup foo.nl:/srv
```

### 2. Create an unprivileged user and ssh key pair

Make sure the configured local user exists on your system and is not being used
by any other programs. Here we create the user `_snaps` and a new ssh
public/private key pair. We make sure the _snaps process can not write into it's
home directory.

```sh
$ doas useradd -s /sbin/nologin -m _snaps
$ doas -u _snaps ssh-keygen -qt ed25519 -f /home/_snaps/.ssh/id_ed25519
Enter passphrase (empty for no passphrase):
Enter same passphrase again:
$ doas touch /home/_snaps/.ssh/known_hosts
$ doas chown -R root /home/_snaps
$ doas chown _snaps /home/_snaps/.ssh/id_ed25519
$ doas chmod 710 /home/_snaps/.ssh
$ doas chmod 640 /home/_snaps/.ssh/{id_ed25519.pub,known_hosts}
```

### 3. Each remote location must allow incoming ssh access

Make sure all configured backup locations authorize access to the previously
created public key. Do this by adding the public key (which is located in
`/home/_snaps/.ssh/id_ed25519.pub`) to the file `/root/.ssh/authorized_keys` on
each remote host (see the "user" and "ruser" option in [snaps.conf(5)] for more
information). Also add the hostname and public key of each remote host to the
known_hosts file of the _snaps user in `/home/_snaps/.ssh/known_hosts`.

### 4. Snaps must be run manually or scheduled via cron

Run snaps manually in verbose mode to test if everything goes well:
```
$ doas snaps -v
snaps: bar.example.com:/home: updated ownership and permissions of "/srv/snaps"
snaps: bar.example.com:/home: updated ownership and permissions of "/srv/snaps/bar.example.com_home"
snaps: foo.nl:/srv: updated ownership and permissions of "/srv/snaps/foo.nl_srv"
bar.example.com:/home -> /srv/snaps/bar.example.com_home as 1001:1001 (first daily backup)
 prsync -az --delete --numeric-ids --no-specials --no-devices --chroot /srv/snaps/bar.example.com_home --dropsuper 1001 root@bar.example.com:/home .
foo.nl:/srv -> /srv/snaps/foo.nl_srv as 1001:1001 (first daily backup)
 prsync -az --delete --numeric-ids --no-specials --no-devices --chroot /srv/snaps/foo.nl_srv --dropsuper 1001 root@foo.nl:/srv .
```

If all looks good, then schedule it to run periodically via cron(8), i.e. at
2am:

```sh
echo '0 2 * * * root snaps' >> /etc/crontab
```

snaps will only create a new snapshot if the last one taken has expired or if
the -f option is given.


## Documentation

For reference documentation and examples please refer to the manual [snaps(8)]
and [snaps.conf(5)].


## Threat model

When using a central backup server it is important to protect backups from
different locations from each other since not every location has the same
trustworthiness. I.e. an sftp-only server is less likely to be compromised than
a webserver running php and third-party client code.

If one of the remote locations is compromised and if the attacker can exploit
rsync (the process that runs on the central backup server), this can give access
to the other locations the central backup server has access to.

One way to mitigate against this is by using [different system accounts] on the
central backup server, one for each backup location. However this is cumbersome
and tedious.

Snaps requires only one unprivileged system user thanks to a hardened version of
rsync that supports [pledge(2)] and chroot(2). This greatly simplifies the setup
and maintenance of your backup system without the overhead of virtual machines
or containers and mitigates against one compromised location getting access to
the data of another location or backups that have been taken in the past.


## Architecture

There is one master process that forks worker processes for each configured
backup location. Each location has two processes associated with it, a rotator
and an instance of hrsync(8). The rotator maintains all the snapshots for a
location. It cleans up snapshots that were left behind by previous instances and
makes sure hrsync has access to a new directory for a new snapshot and the
previously taken snapshot so that files that have not changed can be hard
linked.

Processes are separated on the basis of trust and function. The rotator is
trusted to rotate only, meaning it is chrooted to the directory of the backup
location and is pledged to do only disk operations (ie. no network, user or
process management). The hrsync process is pledged, chrooted and is running as
an unprivileged user that can only access the previous snapshot of one specific
location.

```ascii

o---------------------o             o--------------------------o
|  MASTER (user root) |   fork(2)   | ROTATOR (user root)      |
|                     |             |                          |
|  chroot /var/empty  | ----------> | chroot per location      |
|  pledge stdio       |       |     | pledge stdio rpath cpath |
o---------------------o       |     |   fattr                  |
                              |     o--------------------------o
                              |
                              v
                         o-------------------------o
                         | HRSYNC(8) (user _snaps) |
                         |                         |
                         | chroot per location     |
                         | pledge stdio proc fattr |
                         |   rpath cpath wpath     |
                         o-------------------------o

```

### Communication protocol

Communication between all processes is done over a socketpair(2) using a small
set of commands. Each command consists of a single integer. After the master has
forked the process-pair for each backup location both children initialize and
wait for a start signal from the master. The master will first signal the
rotator to start. The rotator makes sure a new empty directory is created for a new
snapshot. After the rotator has done this, it will send a signal back to the
master process that it's done. Once the master process receives the signal that
the rotator is done initializing it will signal the hrsync process to start
doing it's job. The hrsync process can exit either successfully or fail. Once
the master process receives the exit status of the hrsync process it will signal
the rotator to either add the new snapshot to the existing list of snapshots or,
in case hrsync had a non-successful exit, remove and cleanup the new snapshot.

The protocol consists of the following commands.

* *CMDCLOSED*
	Sent when the communication channel between two processes is closed
	unexpectedly.

* *CMDSTART*
	Sent from the master process to both the rotator and hrsync. This way
	the master can signal the other processes to proceed.

* *CMDSTOP*
	Sent from the master process to hrsync in case the rotator did not send
	a CMDREADY back to the master.

* *CMDREADY*
	Sent from the rotator to the master when it is done creating a new
	directory.

* *CMDROTCLEANUP*
	Sent from the master to the rotator in case hrsync signalled an
	unsuccessful exit status.

* *CMDROTINCLUDE*
	Sent from the master to the rotator in case the hrsync signalled a
	successful exit status.

* *CMDCUST*
	In case a custom script is configured to run after hrsync is done (see
	exec in snaps.conf(5)) this signal is sent from the master process to
	the postexec process. After this command is sent, it is followed by a
	custom integer that contains the exit code of hrsync to the postexec
	process.


### Simple config file format

A new small parser was written using yacc(1) to support a config file that is
easy to read and write by humans. All existing config file formats like TOML,
YAML, INI, JSON, JSON5 and Human JSON were not optimal, either in writing the
actual config files or in writing a parser that supported the syntax while
keeping dependencies small and not overly complex.


## License

ISC

Copyright (c) 2018 Tim Kuijsten

Permission to use, copy, modify, and/or distribute this software for any
purpose with or without fee is hereby granted, provided that the above
copyright notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

---

snaps relies on [hrsync] which is distributed under the GPL version 3.

[different system accounts]: https://sourceforge.net/p/rsnapshot/mailman/message/35031846/
[manpage]: https://netsend.nl/mongovi/mongovi.1.html
[rsync]: https://rsync.samba.org/
[hrsync]: https://github.com/timkuijsten/hrsync
[rsnapshot]: http://rsnapshot.org/
[pledge(2)]: http://man.openbsd.org/pledge.2
[snaps(8)]: https://netsend.nl/snaps/snaps.8.html
[snaps.conf(5)]: https://netsend.nl/snaps/snaps.conf.5.html
[OpenBSD]: https://www.openbsd.org/
