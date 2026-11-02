# Microsoft-signed driver package

These opaque evaluation binaries are driver version **0.1.0.0**. The kernel
implementation source is private. The [binary demo terms](../../licenses/BINARY-LICENSE.txt)
apply separately from the public user-mode source license.

`Build.ps1` and CMake embed this matching INF, CAT and both SYS files into each
standalone setup EXE. Keep all four files together and unmodified: changing an
INF or SYS invalidates its catalog membership. Building does not install them.

Normal installation uses the UI or console setup, with administrator rights and
a Windows restart when requested. Test mode and development certificates are
not required.
