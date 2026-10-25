trigger: when someone proposes splitting the system into services — or every release has become a negotiation between teams
tags: #architecture #design
also: microservices, monolith, services
weight: 50
verified: 2026-10
reference: https://martinfowler.com/bliki/MonolithFirst.html
sources: Fowler, bliki "MonolithFirst" (2015), "MicroserviceTradeOffs" (2015), "MicroservicePrerequisites" (2014); Khononov, Learning Domain-Driven Design ch. 14; Ford et al., Building Evolutionary Architectures 2ed, "Architectural Quanta and Granularity"; Wells, Enabling Microservice Success ch. 1, 3, 10; Head First Software Architecture ch. 7, 10

The default is one deployable. In-process calls do not fail, one transaction
covers a change, one artefact is deployed, one log is read. Distribution buys
independent deployability and independent scaling, and charges for it with
network failure, version skew, a pipeline and on-call surface per service, and
a debugging problem where the request path is not the source. Leave the default
only when a constraint that can be measured says to.

Constraints that can justify a split:

- **Team boundaries.** The number of people who must touch one build has
  outgrown it, so the interfaces between them want to be contracts rather than
  code review. Conway's Law means the architecture will follow the teams
  anyway — the choice is whether it does so deliberately.
- **Independent scaling.** One part has a measured load or resource profile
  that differs by an order of magnitude from the rest, or a spike the rest does
  not share.
- **Independent release cadence.** One part changes far more often, and
  deploying the whole system is what makes every change expensive.
- A mandate from above is a constraint too — write it down as one, because the
  modularity work below is owed either way.

Until one of those holds, the work is inside the deployable: modules by domain,
boundaries enforced by the build rather than by a wiki page. That is the
modular monolith, and it keeps the option of extracting a service later — often
one at the edges, once its shape has stopped moving.

The failure to recognise is the distributed monolith: several deployables that
share a database, must be released in lockstep, or call each other in a
synchronous chain to answer one request. The distribution price is paid and the
coupling it was meant to remove is still there. A shared database keeps the
system one unit of change no matter how many processes run; two services that
must be deployed together are one service with a network hop.

Symptom of splitting too early: a feature that was a day becomes a week of
contract changes, nobody can run the system locally, and the incident channel
has three teams in it. Measure before and after — deploys per week, test-suite
wall clock, how many teams a change touches — and let the number, not the
diagram, choose.

## Run it

```run
# manual: needs the project's own pipeline; the shape is to measure before splitting:
#   time make -j                                   # build and test wall clock, per change
#   git log --format=%cI -- src/part-to-extract | head -20   # release cadence of that part
```
