+++
title = 'Signals'
weight = 6
+++

# Signals

Events flow through one primitive: `SubscriberList<Args...>`. A producer publishes, any
number of handlers subscribe, and a handler can stop the event from reaching the rest. It
is a single struct template in `Saffron.Signal`, and it is how the window reports input and
how selection changes ripple through the editor.

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

A `SubscriberList<Entity>` carries an entity to each handler; a `SubscriberList<u32, u32>`
carries a resize's width and height. The `Args...` are the event payload, fixed at the
type. This is a struct with a method set, not a class hierarchy — the
[Go-flavored](../go-flavored-design/) shape applied to events.

## Subscribe returns a token

`subscribe` stores the handler under a monotonically increasing id and hands back a
`SubscriptionId`, a thin `u64` wrapper. You hold it to call `unsubscribe(id)` later, which
erases the matching entry. Ids only ever increase, so a stale token can't accidentally
match a newer subscription.

## A handler returns bool to stop

The handler returns `bool`, meaning "stop here". `publish` walks the subscribers in order
and breaks the moment one returns `true`. This is explicit, Go-style control flow:
returning `true` is a visible decision in the handler, not a hidden `event.consumed` flag
mutated somewhere. It is exactly how ImGui takes priority over the rest of the app — its
event sink returns `true` when it wants a keystroke or click, and later handlers never see
it.

## Publish iterates a snapshot

`publish` copies `entries` into a local snapshot before looping, because a handler is
allowed to subscribe or unsubscribe during dispatch — which would otherwise mutate the
vector being iterated. Iterating the copy fixes the set of handlers for this publish at the
moment it starts; changes take effect on the next event. It costs one vector copy per
publish, cheap for the handful of subscribers these lists carry, and it removes a whole
class of reentrancy bug.

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
`onResize`, `onKeyPressed`, and friends are each a `SubscriberList`, plus a raw
`eventSinks` list ImGui feeds off. The editor uses a `SubscriberList<Entity>` for
selection, so the hierarchy, inspector, and gizmo all stay in sync without knowing about
each other. A producer publishes; whoever cares subscribed.

## In the code

| What | File | Symbols |
|---|---|---|
| The primitive | `signal.cppm` | `SubscriberList`, `Entry`, `SubscriptionId` |
| Subscribe / unsubscribe | `signal.cppm` | `subscribe`, `unsubscribe` |
| Stop-propagation dispatch | `signal.cppm` | `publish` (snapshot + `break`) |
| Typed window signals | `window.cppm` | `onResize`, `onKeyPressed`, `eventSinks` |

> [!NOTE]
> A subscribe/unsubscribe done inside a handler doesn't change who else receives the
> current event — `publish` froze the list before the loop. Your change lands on the next
> publish.

## Related

- [Go-flavored design](../go-flavored-design/) — events as a struct with a method set, not a hierarchy
- [Window and events](../../app-lifecycle-and-window/window-and-events/) — the typed signals built on this
