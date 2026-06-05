/// Bottom-right transient operation toasts.
import { useEffect, useState } from "react";
import { useRef } from "react";
import { subscribeNotifications } from "../lib/flash";
import { Toast, ToastProvider, ToastTitle, ToastViewport } from "@/components/ui/toast";

interface NotificationToast {
  id: number;
  message: string;
  ms: number;
  open: boolean;
}

export function Notifications() {
  const [toasts, setToasts] = useState<NotificationToast[]>([]);
  const nextId = useRef(0);

  useEffect(() => {
    return subscribeNotifications(({ message: next, ms }) => {
      nextId.current = nextId.current + 1;
      setToasts((current) => [
        ...current.slice(-2),
        { id: nextId.current, message: next, ms, open: true },
      ]);
    });
  }, []);

  return (
    <ToastProvider swipeDirection="right">
      {toasts.map((toast) => (
        <Toast
          key={toast.id}
          open={toast.open}
          duration={toast.ms}
          onOpenChange={(open) => {
            setToasts((current) =>
              open
                ? current.map((candidate) =>
                    candidate.id === toast.id ? { ...candidate, open } : candidate,
                  )
                : current.filter((candidate) => candidate.id !== toast.id),
            );
          }}
        >
          <ToastTitle title={toast.message}>{toast.message}</ToastTitle>
        </Toast>
      ))}
      <ToastViewport />
    </ToastProvider>
  );
}
