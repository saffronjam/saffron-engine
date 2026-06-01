+++
title = 'Core & conventions'
weight = 1
+++

# Core & conventions

The aliases and helper types in `Saffron.Core`, the signal/slot system, and the
Go-flavored style the whole codebase follows. The error model and ownership rules here
reappear on every rendering page, so read this first.

## Pages

| Page | Covers | Code |
|---|---|---|
| [go-flavored-design](go-flavored-design/) | one `se` namespace, free functions, no inheritance, errors as values | `CONVENTIONS.md` |
| [error-handling](error-handling/) | `Result<T>` = `std::expected`, `Err`, check-at-call-site, no exceptions | `core.cppm` · `Result`, `Err` |
| [type-aliases-and-primitives](type-aliases-and-primitives/) | `u8…f64`, `TimeSpan`, `Uuid`, `newUuid` | `core.cppm` · aliases |
| [ownership-and-raii](ownership-and-raii/) | `Ref<T>` = `shared_ptr`, move-only GPU wrappers, teardown order | `core.cppm` · `Ref`; `app.cppm` · `waitGpuIdle` |
| [logging](logging/) | `logInfo` / `logWarn` / `logError` | `core.cppm` · log fns |
| [signals-and-slots](signals-and-slots/) | `SubscriberList<Args…>`, `subscribe`, `SubscriptionId`, stop-propagation | `signal.cppm` |
| [json-gateway](json-gateway/) | error-as-value JSON over nlohmann, `JSON_NOEXCEPTION` | `json.cppm` |
