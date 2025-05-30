FTS Xapian plugin for Dovecot
=============================

What is this?
-------------

This project intends to provide a straightforward, simple and maintenance free, way to configure FTS plugin for [Dovecot](https://github.com/dovecot/), leveraging the efforts by the [Xapian.org](https://xapian.org/) team.

This effort came after Dovecot team decided to deprecate "fts_squat" included in the dovecot core, and due to the complexity of the Solr plugin capabilitles, unneeded for most users.

If you feel donating, kindly use Paypal : moreaujoan@gmail.com


Debugging/Support
-----------------
Please submit requests/bugs via the [GitHub issue tracker](https://github.com/grosjo/fts-xapian/issues).
A Matrix Room exists also at : #xapian-dovecot:matrix.grosjo.net


Availability in major distributions
-----------------------------------

THis plugin is readly available in major distributions under the name "dovecot-fts-xapian"
- Archlinux : https://archlinux.org/packages/?q=dovecot-fts-xapian
- Debian : https://packages.debian.org/bookworm/dovecot-fts-xapian
- Fedora : https://src.fedoraproject.org/rpms/dovecot-fts-xapian


Configuration - dovecot.conf file
---------------------------------

You need to setup LMTP properly with your SMTP server. Kindly refer to:
- For Postfix : https://doc.dovecot.org/2.3/configuration_manual/howto/postfix_dovecot_lmtp/
- For Exim : https://doc.dovecot.org/2.3/configuration_manual/howto/dovecot_lmtp_exim/


Update your dovecot.conf file with something similar to:

*VERSION 2.3.x*

```
(...)
protocols = imap pop3 sieve lmtp

mail_plugins = (...) fts fts_xapian

plugin {
    fts = xapian
    fts_xapian = verbose=0

    fts_autoindex = yes
    fts_enforced = yes

    (...)
}

service indexer-worker {
    # Increase vsz_limit to 2GB or above.
    # Or 0 if you have rather large memory usable on your server, which is preferred for performance)
    vsz_limit = 2G
    # This one must be 0
    process_limit = 0
}
(...)

```

*VERSION 2.4.x*

```
(...)
protocols = imap pop3 sieve lmtp

mail_plugins = (...) fts fts_xapian

fts_autoindex = yes

language "en" {
        default = yes
}
// Note : the 'language' settings is set mandatory by dovecot 
//        but has totally NO impact on FTS Xapian module

fts xapian {
// Note : All variables are optional
        verbose = 0
        maxthreads = 4
        lowmemory = 500
        partial = 3
}
(...)

```

Configuration options
--------------------------------

| Option         | Optional | Description                     | Possible values                                     | Default value |
|----------------|----------|---------------------------------|-----------------------------------------------------|---------------|
| partial        |   yes    | Minimum size of search keyword  | 3 or above                                          | 3             |
| verbose        |   yes    | Logs verbosity                  | 0 (silent), 1 (verbose) or 2 (debug)                | 0             |
| lowmemory      |   yes    | Memory limit before disk commit | 0 (default, meaning 300MB), or set value (in MB)    | 0             |
| maxthreads     |   yes    | Maximum number of threads       | 0 (default, hardware limit), or value above 2       | 0             |



Index updating
------------------------------

Just restart Dovecot:

```sh
sudo service restart dovecot
```

You shall put in a cron the following command (daily for instance) to cleanup indexes :

```sh
doveadm fts optimize -A
```

If this is not a fresh install of dovecot, you need to re-index your mailboxes:

```sh
doveadm index -A -q \*
```
With argument -A, it will re-index all mailboxes, therefore may take a while.
With argument -q, doveadm queues the indexing to be run by indexer process. Remove -q if you want to index immediately.


Building yourself - Prerequisites
----------------------------------

You are going to need the following things to get this going:

```
* Dovecot 2.2.x (or above)
* Xapian 1.2.x (or above)
* ICU 50.x (or above)
* SQlite 3.x
```

You will need to configure properly [Users Home Directories](https://doc.dovecot.org/2.3/configuration_manual/home_directories_for_virtual_users/) in dovecot configuration



Building yourself - Installing the Dovecot plugin
-----------------------------

First install the following packages, or equivalent for your operating system.

```
Ubuntu:
apt-get build-dep dovecot-core 
apt-get install dovecot-dev git libxapian-dev libicu-dev libsqlite3-dev autoconf automake libtool pkg-config

Archlinux:
pacman -S dovecot
pacman -S xapian-core icu git sqlite

FreeBSD:
pkg install xapian-core
pkg install xapian-bindings
pkg install icu
pkg install git

Fedora:
dnf install sqlite-devel libicu-devel xapian-core-devel
dnf install dovecot-devel git 
```

Clone this project:

```
git clone https://github.com/grosjo/fts-xapian
cd fts-xapian
```

Compile and install the plugin.

```
autoupdate
autoreconf -vi
./configure --with-dovecot=/path/to/dovecot
make
sudo make install
```

Note: if your system is quite old, you may change gnu++20 by gnu++11 in src/Makefile.in

Replace /path/to/dovecot by the actual path to 'dovecot-config'.
Type 'locate dovecot-config' in a shell to figure this out. On ArchLinux , it is /usr/lib/dovecot.

For specific configuration, you may have to 'export PKG_CONFIG_PATH=...'. To check that, type 'pkg-config --cflags-only-I icu-uc icu-io icu-i18n', it shall return no error.

The module will be placed into the module directory of your dovecot configuration



------


Thanks to Aki Tuomi <aki.tuomi@open-xchange.com>, Stephan Bosch <stephan@rename-it.nl>, Paul Hecker <paul@iwascoding.com>
