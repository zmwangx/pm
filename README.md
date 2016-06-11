# pm

[![Latest release](https://img.shields.io/github/release/zmwangx/pm.svg)](https://github.com/zmwangx/pm/releases/latest)
[![License: WTFPL](https://img.shields.io/badge/license-WTFPL-blue.svg)](COPYING)

*Man page editing made less painful.*

## Prerequisites

- A C++11 compiler;
- A POSIX system with `man(1)`;
- Python 3.3 or later;
- A modern web browser with support for server-sent events
  ([no Microsoft for you!](http://caniuse.com/#feat=eventsource)).

Optional:

- [`py-setproctitle`](https://github.com/dvarrazzo/py-setproctitle) — used to set the server process title to `pmserver`, useful for monitoring server status (otherwise, the process will have a generic title `python`).
