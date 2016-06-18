# pm

[![Latest release](https://img.shields.io/github/release/zmwangx/pm.svg)](https://github.com/zmwangx/pm/releases/latest)
[![License: WTFPL](https://img.shields.io/badge/license-WTFPL-blue.svg)](COPYING)

*Man page editing made less painful.*

This utility takes the path to a man page source file, then

- Generates an HTML version of how `man(1)` would render the page in a tty;
- Opens the HTML page in the user's default web browser;
- Watches for changes in the source file, and updates the HTML page's content
  as necessary.

This utility started
[here](https://github.com/jarun/googler/pull/109#issuecomment-223862199), and
was partially inspired by [joeyespo/grip](https://github.com/joeyespo/grip) and
[mgedmin/restview](https://github.com/mgedmin/restview).

<!-- START doctoc generated TOC please keep comment here to allow auto update -->
<!-- DON'T EDIT THIS SECTION, INSTEAD RE-RUN doctoc TO UPDATE -->

## Table of Contents

- [Supported Operating Systems](#supported-operating-systems)
- [Prerequisites](#prerequisites)
- [Installation](#installation)
- [Usage](#usage)
- [Screenshot](#screenshot)
- [See Also](#see-also)
- [License](#license)

<!-- END doctoc generated TOC please keep comment here to allow auto update -->

## Supported Operating Systems

| Operating System | Support |
| --- | --- |
| OS X / macOS | ✓ — Primary target |
| Linux | ✓ — Primary target |
| Other flavors of BSD | Probably ✓ — Secondary target |
| Other POSIX / UNIX | Possibly ✓ — No guarantee |
| Windows NT | Hmm, what is man? |

## Prerequisites

- A C++11 compiler;
- Python 3.3 or later;
- A modern web browser with support for server-sent events
  ([no Microsoft for you!](http://caniuse.com/#feat=eventsource));
- Relative recent `autoconf` and `automake`.

Optional:

- [`py-setproctitle`](https://github.com/dvarrazzo/py-setproctitle) — used to
  set the server process title to `pmserver`, useful for monitoring server
  status (otherwise, the process will have a generic title `python`).

## Installation

```bash
$ autoreconf -i
$ ./configure
$ make
$ make install
```

The program is installed into `/usr/local` by default. To install into a
different prefix, run

```bash
$ ./configure --prefix=/path/to/prefix
```

instead in the second step. Staged installation with `DESTDIR` is also
supported by the `install` target.

Note that although `configure -h` shows an entire list of standard options,
only `--prefix` is officially supported. In particular, the program relies on
the relative location of `server.py` with respect to `pm`, so one should NOT
mess with paths other than setting a custom `prefix`.

To uninstall, simply replace the `install` target with `uninstall`.

## Usage

```bash
$ pm /path/to/man/page
```

Press `^C` when you are done.

## Screenshot

![Screenshot of `pm(1)`.](https://i.imgur.com/emPAatA.png)

## See Also

- [sveinbjornt/ManDrake](https://github.com/sveinbjornt/ManDrake) — A native OS
  X / macOS man page editor with live preview especially good for editing
  [`mdoc`](http://mdocml.bsd.lv/man/mdoc.7.html).

## License

**Copyright © 2016 <a href="mailto:zmwangx@gmail.com">Zhiming Wang</a>**

This work is free. You can redistribute it and/or modify it under the terms of
the Do What The Fuck You Want To Public License, Version 2, as published by Sam
Hocevar. See the [`COPYING`](COPYING) file for more details.
