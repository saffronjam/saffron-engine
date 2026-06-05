+++
title = 'Signals'
weight = 6
+++

# Signals

A signal is a typed broadcast channel: a producer publishes an event, and any number of
subscribed handlers receive it in turn. The pattern decouples the producer from its
consumers — the producer knows nothing about who listens, only the event's payload type.

Saffron expresses this with one struct template, `SubscriberList<Args...>` in
`Saffron.Signal`. A handler may stop the event from reaching the rest, which makes the
list both a fan-out channel and a prioritized chain. The window reports input through it,
and selection changes ripple through the editor on it.

## The shape

```cpp
template <typename... Args>
struct SubscriberList
{
    struct Entry { u64 id = 0; std::function<bool(Args...)> handler; };

    std::vector<Entry> entries;
    u64 nextId = 1;

    auto subscribe(std::function<bool(Args...)> handler) -> SubscriptionId;
    void unsubscribe(SubscriptionId id);
    void publish(Args... args) const;
};
```

The `Args...` are the event payload, fixed at the type. A `SubscriberList<Entity>` carries
an entity to each handler; a `SubscriberList<u32, u32>` carries a resize's width and
height. This is a struct with a method set, not a class hierarchy — the
[Go-flavored](../go-flavored-design/) shape applied to events.

## Subscription tokens

`subscribe` stores the handler under a monotonically increasing id and returns a
`SubscriptionId`, a thin `u64` wrapper. The caller holds it to call `unsubscribe(id)`
later, which erases the matching entry. Ids only ever increase, so a stale token cannot
match a newer subscription.

## Stop-propagation dispatch

A handler returns `bool` to mean "stop here". `publish` walks the subscribers in order and
breaks the moment one returns `true`, so each list is also a priority chain. The decision
is explicit: returning `true` is a visible statement in the handler, not a hidden
`event.consumed` flag mutated elsewhere. A handler claims an event this way — returning
`true` when it wants a keystroke or click, so later handlers never see it.

## Snapshot iteration

A handler may subscribe or unsubscribe during dispatch, which would mutate the vector being
iterated. `publish` guards against that by copying `entries` into a local snapshot before
looping. The snapshot fixes the set of handlers for this publish at the moment it starts;
any change takes effect on the next event. The cost is one vector copy per publish — cheap
for the handful of subscribers these lists carry — and it removes a class of reentrancy bug.

```cpp
void publish(Args... args) const
{
    std::vector<Entry> snapshot = entries;
    for (const Entry& entry : snapshot)
    {
        if (entry.handler(args...)) { break; }
    }
}
```

## Where it is used

The [window](../../app-lifecycle-and-window/window-and-events/) owns the most-used lists:
`onResize`, `onKeyPressed`, and the rest are each a `SubscriberList`, alongside a raw
`eventSinks` list the gizmo and editor camera feed off. The editor uses a
`SubscriberList<Entity>` for selection, so the scene-edit state and the gizmo stay in sync
without knowing about each other.

## In the code

| What | File | Symbols |
|---|---|---|
| The primitive | `signal.cppm` | `SubscriberList`, `Entry`, `SubscriptionId` |
| Subscribe / unsubscribe | `signal.cppm` | `subscribe`, `unsubscribe` |
| Stop-propagation dispatch | `signal.cppm` | `publish` (snapshot + `break`) |
| Typed window signals | `window.cppm` | `onResize`, `onKeyPressed`, `eventSinks` |

> [!NOTE]
> A subscribe/unsubscribe done inside a handler doesn't change who else receives the
> current event — `publish` froze the list before the loop. The change lands on the next
> publish.

## Related

- [Go-flavored design](../go-flavored-design/) — events as a struct with a method set, not a hierarchy
- [Window and events](../../app-lifecycle-and-window/window-and-events/) — the typed signals built on this
