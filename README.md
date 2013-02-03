timer-pool
==========

Timer has timeout period and a handler; imagine possibility to have many concurrent timers.


------------- license
FreeBSD license

------------- how it works
Check test/test.cpp.

------------- TODOs
- error handling philosophy
  - error return codes
  - allow / disallow exceptions
- added support for milliseconds
- many listeners to a single timer
- listener is provided to timer in constructor
- supported multi-threading level explained
- periodic emission of events
- executor count is minimized over time, if they are not needed
- etc.

