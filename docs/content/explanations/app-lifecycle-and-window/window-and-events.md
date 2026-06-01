+++
title = 'Window & events'
weight = 4
+++

# Window & events

The window is an SDL3 handle plus its current size plus a set of typed event signals. Like
everything else in the engine it is plain data, not a class. Input reaches the rest of the
program through the signals: a layer subscribes in `onAttach` and gets called back when the
matching event happens. The old engine routed input through an event/dispatch object; the
rewrite reuses the same signal/slot primitive it uses everywhere else (selection, for one)
rather than introducing a second mechanism.

```cpp
struct Window
{
    SDL_Window* handle = nullptr;
    u32 width = 0;
    u32 height = 0;
    bool shouldClose = false;

    SubscriberList<> onClose;
    SubscriberList<u32, u32> onResize;          // width, height (pixels)
    SubscriberList<i32, bool> onKeyPressed;     // keycode, isRepeat
    SubscriberList<i32> onKeyReleased;          // keycode
    SubscriberList<std::string> onFileDropped;  // dropped file path

    std::vector<std::function<void(const SDL_Event&)>> eventSinks;
};
```

## Polling and dispatch

`newWindow` initializes SDL video and creates the window with Vulkan, resizable, and
high-pixel-density flags, returning a `Result<Window>` — a failure comes back as an `Err`
string, never an exception (see [error handling](../../core-and-conventions/error-handling/)).

`pollEvents` runs once at the top of each loop iteration. It drains the SDL queue and, for
each event, forwards the raw event to every sink, then translates the events it recognizes
into typed signal publishes.

## Typed signals

Each signal is a `SubscriberList<Args...>`, the engine-wide signal/slot type. A subscriber is
a closure that returns `true` to stop propagation or `false` to let later subscribers also see
the event. SDL events map in as:

| Signal | SDL event | Payload |
|---|---|---|
| `onClose` | `SDL_EVENT_QUIT`, `SDL_EVENT_WINDOW_CLOSE_REQUESTED` | none; also sets `shouldClose` |
| `onResize` | `SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED` | new width, height in pixels |
| `onKeyPressed` | `SDL_EVENT_KEY_DOWN` | keycode, `isRepeat` |
| `onKeyReleased` | `SDL_EVENT_KEY_UP` | keycode |
| `onFileDropped` | `SDL_EVENT_DROP_FILE` | dropped file path |

`onResize` publishes the **pixel** size, not the logical size, which matters under
high-pixel-density (the window carries `SDL_WINDOW_HIGH_PIXEL_DENSITY`). `run` subscribes
`onClose` right after creating the window so closing it flips `app.running` to false; the
editor subscribes `onFileDropped` to import dropped models and textures.

## Raw event sinks

Some consumers need the whole `SDL_Event`, not a typed slice. ImGui is the main one: its SDL3
backend wants every event to track mouse, focus, and text input. Rather than couple `Window`
to ImGui, `Window` exposes `eventSinks`, a list of `std::function<void(const SDL_Event&)>`,
and the UI layer pushes a sink that forwards each event to the backend.

Sinks run *before* typed dispatch, so ImGui sees an event even when a typed signal later
consumes it. This keeps `Window` ignorant of who is listening: it knows how to forward raw
events and how to publish typed ones, nothing about the consumers.

## In the code

| What | File | Symbols |
|---|---|---|
| Window data + signals | `window.cppm` | `Window`, `WindowConfig` |
| Create / destroy | `window.cppm` | `newWindow`, `destroyWindow` |
| Event drain + dispatch | `window.cppm` | `pollEvents`, `eventSinks` |
| Signal primitive | `signal.cppm` | `SubscriberList`, `subscribe`, `publish` |

> [!TIP]
> `width`/`height` are 0 until the first pixel-size event, and a minimized window reports a 0
> dimension. `run` treats a 0 dimension as minimized and skips the frame, so don't divide by
> the window size in `onUpdate` without guarding against zero.

## Related

- [Main loop](../main-loop-and-run/) — where `pollEvents` is called and `onClose` is wired
- [Layers as a struct of closures](../layer-system/) — a layer subscribes to these signals in `onAttach`
