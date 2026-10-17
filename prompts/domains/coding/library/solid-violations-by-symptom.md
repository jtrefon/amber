trigger: when a type is painful to change and the principle at stake is unclear — read the symptom, then move the dependency
tags: #design #architecture
weight: 55
verified: 2026-10
reference: https://web.archive.org/web/20190905005047/http://www.butunclebob.com/ArticleS.UncleBob.PrinciplesOfOod
sources: Martin, Agile Software Development (SRP ch. 8, LSP ch. 10, DIP ch. 11, ISP ch. 12); Martin, Clean Code ch. 10; Danilov, Refactoring with C++ ch. 2; Fowler, Refactoring 2ed ch. 3; the archived Principles of OOD page; claims executed by their run block

The acronym is not the diagnosis. Start from the change that hurt, let the
symptom name the principle, and fix the direction of the dependency rather than
adding abstraction.

- **A subclass throws, returns an empty result, or refuses an inherited method.**
  The override cannot honour what the base promised: it strengthened a
  precondition, weakened a postcondition, or broke an invariant the base's
  callers may rely on. Callers then grow a type test before using the value.
  Fix: do not inherit the promise — narrow the hierarchy so only types that can
  keep the contract implement it (`SerializableMessage` beside `Message`), or
  compose the shared part instead.
- **Editing one class forces a rebuild, retest or re-import of callers that never
  use the edited part.** A fat interface makes every client depend on the union
  of every client's needs. Fix: split by client, not by implementation — the
  timer's interface has no `set_alarm`, so an alarm change no longer reaches it.
- **Each construction site names a concrete collaborator; the test needs the real
  clock, socket or database.** The policy module builds its own tools, so no
  seam exists to substitute. Fix: the high-level module declares the small
  interface it needs and is handed one; the concrete type is named once, where
  the program is assembled. DIP is about who owns the interface, not about
  stacking layers.
- **Two unrelated changes keep landing in the same file** — a version bump and a
  rendering tweak. That is two reasons to change, and each diff risks the other.
  Fix: split along the axis that changes together, then check that each kind of
  change now touches one file.
- **A new case means editing a switch in the middle of working code.** The
  extension point is missing. Fix: let the new case be a new implementation of
  the interface, and add it when the second or third case actually arrives, not
  on speculation.

The trap is running the principles as a checklist: an interface per class, a
factory for the one implementation, a hierarchy for a case that never came.
The other trap is repairing the symptom at the call site — the type test, the
cast, the mock that only satisfies the test — which hides the broken contract
where the next reader will not see it. Judge the fix by what moved: which files
rebuild, which headers a client must include, which constructor the test must
satisfy.

## Run it

```run
# The compile-time half of the symptom: a header is the interface a client
# depends on, so a fat one makes both clients rebuild for a change only one of
# them uses. Split by client, and only the client that needs it rebuilds.
cat > clock.hpp <<'HPP'
#pragma once
class Clock {                            // one interface, two clients' needs
public:
    virtual ~Clock() = default;
    virtual int now_ms() const = 0;      // the timer's need
    virtual void set_alarm(int ms) = 0;  // the scheduler's need
};
HPP
cat > timer.cpp <<'CPP'
#include "clock.hpp"
int tick(const Clock& c) { return c.now_ms(); }
CPP
cat > scheduler.cpp <<'CPP'
#include "clock.hpp"
void arm(Clock& c) { c.set_alarm(1000); }
CPP
cat > Makefile <<'MK'
all: timer.o scheduler.o
timer.o: clock.hpp
scheduler.o: clock.hpp
MK
make -s all
[ "$(make -W clock.hpp -n all | grep -c '\.cpp')" -eq 2 ] && echo "fat header: every client rebuilds"

cat > clock.hpp <<'HPP'
#pragma once
class Clock { public: virtual ~Clock() = default; virtual int now_ms() const = 0; };
HPP
cat > alarms.hpp <<'HPP'
#pragma once
#include "clock.hpp"
class Alarms : public Clock { public: virtual void set_alarm(int ms) = 0; };
HPP
cat > scheduler.cpp <<'CPP'
#include "alarms.hpp"
void arm(Alarms& a) { a.set_alarm(1000); }
CPP
cat > Makefile <<'MK'
all: timer.o scheduler.o
timer.o: clock.hpp
scheduler.o: alarms.hpp clock.hpp
MK
make -s all
[ "$(make -W alarms.hpp -n all | grep -c '\.cpp')" -eq 1 ] && echo "split by client: only the scheduler rebuilds"
```
