trigger: when one change has to reach more than one consumer, or a request must write in two services and someone says "just publish an event"
tags: #architecture #patterns #async
also: event sourcing, CQRS, audit log, event-driven
weight: 50
verified: 2026-10
reference: https://martinfowler.com/articles/201701-event-driven.html
sources: Fowler, "What do you mean by Event-Driven?" (2017), bliki "CQRS" (2011); Richardson, microservices.io "Transactional outbox" and "Saga"; Khononov, Learning Domain-Driven Design ch. 8, 15; FreeWheel, Cloud-Native Application Architecture ch. 5; Head First Software Architecture ch. 11

An event is a fact that has already happened; a command is an instruction that
can be refused. Publishing the fact decouples the producer from who reacts:
consumers can be added later, a consumer can be down without failing the write,
and the stream of facts is an audit trail that comes free. That is the buy; the
cost shows up in four places.

- **Consistency becomes eventual.** A read after the write can miss it, and
  business rules can act on the stale version — know the window before
  shipping, because "it's there after a refresh" is a product decision.
- **Delivery is at-least-once.** A broker redelivers after a crash and a
  producer retries after a timeout, so the consumer deduplicates on an event id
  or a natural key; a create endpoint retried without one makes duplicate rows.
- **Order lives per partition, not per system.** Events about one aggregate
  must share a partition, or a consumer applying them out of order projects a
  state that never happened; retries and slow consumers reorder work in ways no
  stack trace shows.
- **Debugging looks different.** The flow is a graph, not a stack: no traceback
  crosses the broker, so correlation ids, tracing, a dead-letter queue and a
  replay path are design elements, not tooling added after the incident.

Decision rule: keep the synchronous call until at least one of these is true —
more than one consumer needs the fact, the producer must not know who reacts,
the consumer's availability or latency must not sit in the request path, or the
history is itself the requirement. Otherwise a method call is cheaper than a
topic.

CQRS is the same trade applied to models: one model for the write side
(invariants, the source of truth) and derived read models where reads and
writes genuinely want different shapes or different scale — search, feeds,
reporting. Read models stay rebuildable from the write side, and the split
belongs on the context that needs it rather than system-wide.

When the requirement really is a transaction across services, the honest shape
is outbox plus saga. Commit the state change and the event in one local
transaction, then relay the outbox: publish happens if and only if the commit
did, and the relay may publish twice, which is why consumers deduplicate. The
saga chains local transactions with compensating actions — orchestration when
one component owns the flow, choreography when each service knows only its own
step — and it has no isolation and no automatic rollback. If the boundary can
move so that one service owns the transaction instead, that split is cheaper
than the distributed transaction you would otherwise debug later.

## Run it

```run
# manual: needs a broker and two services; the shape is
#   begin; insert into orders ...; insert into outbox(event_id, payload) ...; commit;
#   relay: select * from outbox where published_at is null order by id; publish; mark sent
# then kill the relay mid-run and check that the consumer deduplicates on event_id.
```
