#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

class ULoomaSceneSyncSubsystem;
class SEditableTextBox;

/**
 * The bare-bones login form (HAM-182). Deliberately in `Private/` — nothing outside this
 * module constructs it; `ULoomaLoginUI` is the front door, and a consumer wanting their
 * own screen should build it against the `Looma|Auth` Blueprint surface rather than
 * subclass this.
 *
 * ## Why this polls through TAttribute instead of binding the delegates
 *
 * The obvious design is to bind `OnAuthStateChanged` / `OnIdentityChanged` and push
 * state in. It cannot be done from here: both are `DECLARE_DYNAMIC_MULTICAST_DELEGATE`,
 * which requires a `UFUNCTION` on a `UObject`, and an `SCompoundWidget` is neither. The
 * options were a `UObject` controller owning this widget and bridging the two delegates,
 * or Slate attributes reading the subsystem as it paints. This takes the second, and it
 * is not merely the cheaper one:
 *
 *   - **An attribute cannot miss a transition.** A binding can: bound a frame after the
 *     event it wanted, or torn down just before it. The state this widget shows is
 *     derived entirely from three getters, so re-deriving it is always right and never
 *     needs reconciling.
 *   - **No lifetime coupling.** A `UObject` bridge would have to outlive the widget,
 *     unbind on teardown, and survive the subsystem going away first. A weak pointer
 *     plus a null check is the whole of it here.
 *   - **It is what Slate is for.** Attributes are evaluated only while the widget is
 *     constructed, and this one collapses itself whenever there is nothing to show, so
 *     the cost is a handful of inline getters per frame on a widget that is usually not
 *     even visible.
 *
 * The delegates remain the right tool for a *consumer's* UI, which is why HAM-181
 * exposed them. They are simply not reachable from a Slate widget without a bridge that
 * would cost more than it buys at this size.
 *
 * ## What it shows, and the one state that matters
 *
 * Nothing at all until `IsAuthStateKnown()`, then nothing unless `IsAuthEnabled()`.
 * **Undecided is not "no login needed"** — and since HAM-181's probe retries with
 * backoff, that window can be seconds on a scene busy enough to saturate the HTTP
 * connection pool, not the millisecond it looks like. A form flashed there and withdrawn
 * would undo the exact reason the auth state is a tri-state.
 */
class SLoomaLoginWidget : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SLoomaLoginWidget) {}
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs, ULoomaSceneSyncSubsystem* InSubsystem);

private:
    /** Weak: the subsystem dies with the game instance, and this widget may outlive it. */
    TWeakObjectPtr<ULoomaSceneSyncSubsystem> Subsystem;

    // --- Which of the three panels is showing ----------------------------------
    //
    // Mutually exclusive by construction: no token means the form, a token with an
    // unresolved kind means a restored session we have not yet confirmed, and a token
    // with a resolved one means signed in.
    EVisibility GetRootVisibility() const;
    EVisibility GetFormVisibility() const;
    EVisibility GetProvisionalVisibility() const;
    EVisibility GetSignedInVisibility() const;

    FText GetIdentityLabel() const;
    FText GetErrorText() const;
    EVisibility GetErrorVisibility() const;

    bool IsSubmitEnabled() const;
    FText GetSubmitLabel() const;

    FReply OnSubmitClicked();
    FReply OnLogoutClicked();
    void OnPasswordCommitted(const FText& Text, ETextCommit::Type CommitType);

    TSharedPtr<SEditableTextBox> UsernameBox;
    TSharedPtr<SEditableTextBox> PasswordBox;

    /**
     * True from the moment a login is sent until its completion runs. Gates the submit
     * button — a login was measured taking seconds on a busy scene (HAM-181's
     * connection-pool finding), and a second click would start a second request. The
     * subsystem's `SessionSerial` guard would survive that, but a UI that cannot create
     * the situation is better than one that relies on being rescued from it.
     */
    bool bLoginInFlight = false;

    FText ErrorText;
};
