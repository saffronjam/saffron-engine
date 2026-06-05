import * as React from "react";
import { Toast as ToastPrimitive } from "radix-ui";

import { cn } from "@/lib/utils";

function ToastProvider({ ...props }: React.ComponentProps<typeof ToastPrimitive.Provider>) {
  return <ToastPrimitive.Provider data-slot="toast-provider" {...props} />;
}

function ToastViewport({
  className,
  ...props
}: React.ComponentProps<typeof ToastPrimitive.Viewport>) {
  return (
    <ToastPrimitive.Viewport
      data-slot="toast-viewport"
      className={cn(
        "fixed right-4 bottom-4 z-100 flex max-h-screen w-[360px] max-w-[calc(100vw-2rem)] flex-col gap-2 outline-none",
        className,
      )}
      {...props}
    />
  );
}

function Toast({ className, ...props }: React.ComponentProps<typeof ToastPrimitive.Root>) {
  return (
    <ToastPrimitive.Root
      data-slot="toast"
      className={cn(
        "rounded-md border bg-popover px-3 py-2 text-popover-foreground shadow-md",
        "data-[state=open]:animate-in data-[state=open]:fade-in-0 data-[state=open]:slide-in-from-bottom-2",
        "data-[state=closed]:animate-out data-[state=closed]:fade-out-0 data-[state=closed]:slide-out-to-right-2",
        className,
      )}
      {...props}
    />
  );
}

function ToastTitle({ className, ...props }: React.ComponentProps<typeof ToastPrimitive.Title>) {
  return (
    <ToastPrimitive.Title
      data-slot="toast-title"
      className={cn("truncate text-xs", className)}
      {...props}
    />
  );
}

export { Toast, ToastProvider, ToastTitle, ToastViewport };
