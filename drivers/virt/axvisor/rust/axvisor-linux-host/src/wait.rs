// SPDX-License-Identifier: Apache-2.0
//! Predicate waits must sample notifications before testing the predicate.

pub(crate) fn wait_until(
    condition: impl Fn() -> bool,
    mut generation: impl FnMut() -> u64,
    mut wait_since: impl FnMut(u64),
) {
    loop {
        // The provider's acquire snapshot precedes the predicate read. A
        // notification after this point makes wait_since return, even if it
        // arrives after condition() returned false but before registration.
        let observed = generation();
        if condition() {
            return;
        }
        wait_since(observed);
    }
}

#[cfg(test)]
mod tests {
    use super::wait_until;
    use std::cell::Cell;

    #[test]
    fn already_ready_does_not_wait() {
        wait_until(|| true, || 7, |_| panic!("unexpected wait"));
    }

    #[test]
    fn notify_between_false_predicate_and_wait_is_observed() {
        let generation = Cell::new(0);
        let ready = Cell::new(false);
        let waits = Cell::new(0);
        wait_until(
            || {
                let was_ready = ready.get();
                if !was_ready {
                    ready.set(true);
                    generation.set(generation.get() + 1);
                }
                was_ready
            },
            || generation.get(),
            |observed| {
                assert_ne!(observed, generation.get(), "lost notification");
                waits.set(waits.get() + 1);
            },
        );
        assert_eq!(waits.get(), 1);
    }

    #[test]
    fn notification_during_wait_rechecks_predicate() {
        let generation = Cell::new(0);
        let ready = Cell::new(false);
        wait_until(
            || ready.get(),
            || generation.get(),
            |observed| {
                assert_eq!(observed, generation.get());
                ready.set(true);
                generation.set(observed + 1);
            },
        );
    }

    #[test]
    fn unrelated_notification_does_not_satisfy_predicate() {
        let generation = Cell::new(0);
        wait_until(
            || generation.get() == 3,
            || generation.get(),
            |observed| generation.set(observed + 1),
        );
        assert_eq!(generation.get(), 3);
    }

    #[test]
    fn generation_wrap_still_detects_change() {
        let generation = Cell::new(u64::MAX);
        wait_until(
            || generation.get() == 0,
            || generation.get(),
            |observed| generation.set(observed.wrapping_add(1)),
        );
    }

    #[test]
    #[should_panic(expected = "lost notification")]
    fn old_order_negative_control() {
        let generation = Cell::new(0);
        let ready = Cell::new(false);
        // Reproduce the old sequence: false predicate, publish + notify,
        // then sample. The new token looks current, so waiting would block.
        let was_ready = ready.get();
        ready.set(true);
        generation.set(generation.get() + 1);
        if !was_ready {
            let observed = generation.get();
            assert_ne!(observed, generation.get(), "lost notification");
        }
    }
}
