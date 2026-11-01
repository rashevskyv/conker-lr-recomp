# Guest event / DPC ordering regressions

These Windows tests link the actual kernel bridge with synthetic imports. They
need no retail executable, generated title code, disc assets, or controller.

- `dpc_atomic`: a passive guest thread queues behind an ISR/DPC. Buffer cleanup,
  event signalling and DPC queue insertion/removal must finish before that thread
  can resume.
  Checks the guest stack cleanup and queued-DPC state as well.
- `dpc_timeout`: a simulated translated DPC takes longer than the waiting
  thread's host timeout, cleans up the USB buffer, then signals an inline
  synchronization event. Timeout finalization must wait for the guest CPU and observe that signal. Separate
  checks retain immediate polling, genuine timeout, and event consumption.
- `dpc_blocking`: a raised DPC waits for a critical section held by another
  guest thread. Both the guest CPU and dispatch lock must be released so the
  holder can run. A watchdog makes an accidental deadlock fail within five seconds.

Configure this directory with a Windows C compiler and build it, then run
`ctest --output-on-failure` in the build directory. MinGW/OpenGL builds also need
`-DXBOXRECOMP_WIN_GL=ON`, an SDL2 CMake package, and SDL2.dll beside the executables.

The timing fixture intentionally delays while holding the virtual CPU. The delay
stands for slow translated code; it is not a guest blocking system call.
